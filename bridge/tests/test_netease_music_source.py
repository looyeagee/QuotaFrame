"""Now-playing source and the now-playing wire format."""

from __future__ import annotations

import asyncio
import json
import sqlite3
import tempfile
import time
import unittest
import urllib.error
from decimal import Decimal
from pathlib import Path

from quotaframe_bridge.domain.models import (
    NowPlayingPublication,
    Provider,
    ProviderUsage,
    SourceState,
)
from quotaframe_bridge.protocol.messages import (
    MAX_LINE_BYTES,
    MAX_LINES,
    MAX_TITLE_BYTES,
    NOW_PLAYING_CAPABILITY,
    encode_now_playing,
)
from quotaframe_bridge.protocol.messages import DeviceStatus
from quotaframe_bridge.service.multi_device import (
    DeviceSession,
    MultiDeviceBridgeService,
)
from quotaframe_bridge.sources.macos_nowplaying import NowPlayingState
from quotaframe_bridge.sources.netease_music import (
    BUNDLE_ID,
    CACHE_VERSION,
    MAX_CACHE_ENTRIES,
    LyricCache,
    LyricLine,
    NeteaseMusicError,
    NeteaseMusicSource,
    current_index,
    display_text,
    line_end_ms,
    fetch_lyrics,
    is_credit_line,
    parse_lrc,
    parse_track,
    read_current_track,
    select_window,
)

def no_mirror():
    """Keep the system now-playing mirror out of these tests."""

    return None


TRACK_JSON = {
    "id": "3455592377",
    "name": "后会友期",
    "duration": 231435,
    "artists": [{"id": "9941", "name": "许飞"}],
    "album": {"id": 406935459, "name": "后会友期 主题曲"},
}


def make_database(directory: Path, rows: list[tuple[str, int, str]]) -> Path:
    """Create a minimal copy of the client's history table."""

    path = directory / "sqlite_storage.sqlite3"
    connection = sqlite3.connect(str(path))
    connection.execute(
        "CREATE TABLE historyTracks (playtime BIGINT NULL, id VARCHAR(40) NOT NULL, "
        "jsonStr TEXT NULL, PRIMARY KEY (id))"
    )
    connection.executemany(
        "INSERT INTO historyTracks (id, playtime, jsonStr) VALUES (?, ?, ?)", rows
    )
    connection.commit()
    connection.close()
    return path


class FakeResponse:
    """Minimal stand-in for the object ``urlopen`` returns."""

    def __init__(self, payload: object) -> None:
        self._raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")

    def read(self, _size: int = -1) -> bytes:
        return self._raw

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *_exc) -> None:
        return None


class LrcParsingTest(unittest.TestCase):
    def test_timestamps_become_sorted_milliseconds(self) -> None:
        lines = parse_lrc(
            "[00:12.34]first\n"
            "[00:05.00]second\n"
            "[01:02.5]third\n"
        )
        self.assertEqual(
            lines,
            (
                LyricLine(5_000, "second"),
                LyricLine(12_340, "first"),
                LyricLine(62_500, "third"),
            ),
        )

    def test_metadata_tags_are_skipped(self) -> None:
        lines = parse_lrc("[ti:title]\n[ar:artist]\n[00:01.00]text\n")
        self.assertEqual(lines, (LyricLine(1_000, "text"),))

    def test_one_text_line_can_carry_several_timestamps(self) -> None:
        lines = parse_lrc("[00:01.00][00:09.00]chorus\n")
        self.assertEqual(lines, (LyricLine(1_000, "chorus"), LyricLine(9_000, "chorus")))

    def test_milliseconds_are_three_digits(self) -> None:
        self.assertEqual(parse_lrc("[00:01.250]x\n")[0].time_ms, 1_250)

    def test_two_digits_are_hundredths_and_one_is_a_tenth(self) -> None:
        self.assertEqual(parse_lrc("[00:01.50]x\n")[0].time_ms, 1_500)
        self.assertEqual(parse_lrc("[00:01.5]x\n")[0].time_ms, 1_500)

    def test_lines_arrive_in_the_forms_the_panel_draws(self) -> None:
        lines = parse_lrc("[00:01.00]\u5355\u4f9d\u7eaf\uff1a\u2f00\u4e2a\u2f08\n")
        self.assertEqual(lines, (LyricLine(1_000, "\u5355\u4f9d\u7eaf:\u4e00\u4e2a\u4eba"),))


class DisplayTextTest(unittest.TestCase):
    """The panel's font covers neither Kangxi radicals nor full-width forms."""

    def test_kangxi_radicals_fold_onto_the_ideographs(self) -> None:
        self.assertEqual(display_text("\u2f00\u4e2a\u2f08"), "\u4e00\u4e2a\u4eba")

    def test_full_width_punctuation_folds_onto_the_ascii_form(self) -> None:
        self.assertEqual(
            display_text("\u4ed6\u8bf4\uff1a\u597d\uff0c\u8d70\u5427\uff08\u505c\uff09\uff1f"),
            "\u4ed6\u8bf4:\u597d,\u8d70\u5427(\u505c)?",
        )

    def test_zero_width_marks_are_dropped(self) -> None:
        # They would reach the panel as a box where nothing should be drawn.
        self.assertEqual(display_text("\u4f60\u200b\u597d"), "\u4f60\u597d")
        self.assertEqual(display_text("a\ufeffb"), "ab")

    def test_ideographs_and_ideographic_punctuation_are_untouched(self) -> None:
        text = "\u4e00\u4e2a\u4eba\u3002\u3001"
        self.assertEqual(display_text(text), text)


class WindowTest(unittest.TestCase):
    LINES = tuple(LyricLine(index * 10_000, f"line {index}") for index in range(6))

    def test_window_keeps_context_around_the_current_line(self) -> None:
        window = select_window(self.LINES, 22_000, before=1, after=3)
        self.assertEqual([line.text for line in window],
                         ["line 1", "line 2", "line 3", "line 4", "line 5"])

    def test_window_stops_at_the_song_end(self) -> None:
        window = select_window(self.LINES, 50_000, before=1, after=3)
        self.assertEqual([line.text for line in window], ["line 4", "line 5"])

    def test_default_window_fills_the_panel_rows_after_the_sung_line(self) -> None:
        # The music page draws five rows from the sung line onward, so the default window
        # has to carry four lines after it rather than stopping at three.
        lines = tuple(LyricLine(index * 10_000, f"line {index}") for index in range(12))
        window = select_window(lines, 32_000)
        current = current_index(window, 32_000)
        self.assertEqual(len(window) - current, 5)
        self.assertEqual(window[current].text, "line 3")

    def test_position_before_the_first_line_keeps_the_start(self) -> None:
        self.assertEqual(current_index(self.LINES, 0), 0)

    def test_current_index_tracks_the_sung_line(self) -> None:
        window = select_window(self.LINES, 31_000)
        self.assertEqual(current_index(window, 31_000), 1)

    def test_a_line_starts_on_the_millisecond_it_states(self) -> None:
        # The window follows the song in milliseconds, so the line that begins at
        # 12.340 s is the sung one from that instant on.
        lines = (
            LyricLine(10_000, "before"),
            LyricLine(12_340, "first"),
            LyricLine(15_000, "second"),
        )
        self.assertEqual(current_index(select_window(lines, 12_339), 12_339), 0)
        self.assertEqual(current_index(select_window(lines, 12_340), 12_340), 1)


class LineEndTest(unittest.TestCase):
    LINES = tuple(LyricLine(index * 10_000, f"line {index}") for index in range(4))

    def test_line_end_is_the_start_of_the_next_line(self) -> None:
        self.assertEqual(line_end_ms(self.LINES, 0), 10_000)
        self.assertEqual(line_end_ms(self.LINES, 12_000), 20_000)
        self.assertEqual(line_end_ms(self.LINES, 29_000), 30_000)

    def test_the_last_line_and_an_empty_song_report_no_end(self) -> None:
        self.assertEqual(line_end_ms(self.LINES, 35_000), 0)
        self.assertEqual(line_end_ms((), 5_000), 0)


class TrackReadingTest(unittest.TestCase):
    def test_parse_track_joins_artists_and_converts_duration(self) -> None:
        track = parse_track("42", 1_700_000_000_000, {
            **TRACK_JSON,
            "artists": [{"name": "甲"}, {"name": "乙"}],
        })
        self.assertEqual(track.song_id, "42")
        self.assertEqual(track.title, "后会友期")
        self.assertEqual(track.artist, "甲 / 乙")
        self.assertEqual(track.album, "后会友期 主题曲")
        self.assertEqual(track.duration_seconds, 231)

    def test_title_and_artist_arrive_in_the_forms_the_panel_draws(self) -> None:
        track = parse_track("42", 1_700_000_000_000, {
            **TRACK_JSON,
            "name": "\u2f00\u4e2a\u2f08\uff1a",
            "artists": [{"name": "\u5355\u4f9d\u7eaf\uff08\u7ffb\u5531\uff09"}],
        })
        self.assertEqual(track.title, "\u4e00\u4e2a\u4eba:")
        self.assertEqual(track.artist, "\u5355\u4f9d\u7eaf(\u7ffb\u5531)")

    def test_newest_row_wins(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = make_database(Path(directory), [
                ("1", 1_000, json.dumps({"name": "old", "duration": 1000})),
                ("2", 2_000, json.dumps({"name": "new", "duration": 2000})),
            ])
            track = read_current_track(path)
        self.assertIsNotNone(track)
        assert track is not None
        self.assertEqual((track.song_id, track.title), ("2", "new"))

    def test_missing_database_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(NeteaseMusicError):
                read_current_track(Path(directory) / "absent.sqlite3")


class LyricFetchTest(unittest.TestCase):
    def test_original_lyric_is_preferred(self) -> None:
        requested = []

        def urlopen(request, timeout):  # noqa: ARG001 - signature of urlopen
            requested.append(request.full_url)
            return FakeResponse({
                "lrc": {"lyric": "[00:01.00]sung\n"},
                "tlyric": {"lyric": "[00:01.00]translated\n"},
            })

        lines = fetch_lyrics("42", urlopen=urlopen)
        self.assertEqual(lines, (LyricLine(1_000, "sung"),))
        self.assertIn("id=42", requested[0])

    def test_empty_original_falls_back_to_the_translation(self) -> None:
        def urlopen(_request, timeout):  # noqa: ARG001
            return FakeResponse({
                "lrc": {"lyric": ""},
                "tlyric": {"lyric": "[00:02.00]translated\n"},
            })

        self.assertEqual(fetch_lyrics("42", urlopen=urlopen),
                         (LyricLine(2_000, "translated"),))


class SourceTest(unittest.TestCase):
    def _source(self, directory: Path, *, probe=frozenset({BUNDLE_ID})):
        return NeteaseMusicSource(
            database=make_database(directory, [
                ("3455592377", 1_700_000_000_000, json.dumps(TRACK_JSON)),
            ]),
            clock=lambda: 1_700_000_042.0,
            audio_probe=lambda: probe,
            now_playing=no_mirror,
            urlopen=lambda _request, timeout: FakeResponse({
                "lrc": {"lyric": "[00:00.00]a\n[00:40.00]b\n[00:50.00]c\n",
                        "tlyric": {"lyric": ""}},
            }),
        )

    def test_read_reports_the_song_and_its_playhead(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(Path(directory)).read()
        self.assertIsNotNone(publication)
        assert publication is not None
        self.assertEqual(publication.title, "后会友期")
        self.assertEqual(publication.artist, "许飞")
        self.assertEqual(publication.position_seconds, 42)
        self.assertEqual(publication.duration_seconds, 231)
        self.assertTrue(publication.playing)
        self.assertEqual(publication.lines, ("a", "b", "c"))
        self.assertEqual(publication.current_line, 1)
        self.assertEqual(publication.position_ms, 42_000)
        # The panel places the song on this timeline, so every line carries its start.
        self.assertEqual(publication.line_times_ms, (0, 40_000, 50_000))

    def test_ended_track_keeps_the_playhead_at_its_end(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = self._source(Path(directory))
            source._clock = lambda: 1_700_000_900.0
            publication = source.read()
        assert publication is not None
        # 900 s after a 231 s track started: the playhead stops at the end.
        self.assertEqual(publication.position_seconds, 231)
        self.assertFalse(publication.playing)

    def test_silence_marks_the_song_as_paused(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(Path(directory), probe=frozenset()).read()
        assert publication is not None
        self.assertFalse(publication.playing)

    def test_lyrics_are_requested_once_per_song(self) -> None:
        calls = []

        def urlopen(_request, timeout):  # noqa: ARG001
            calls.append(1)
            return FakeResponse({"lrc": {"lyric": "[00:01.00]x\n"}})

        with tempfile.TemporaryDirectory() as directory:
            source = NeteaseMusicSource(
                database=make_database(Path(directory), [
                    ("3455592377", 1_700_000_000_000, json.dumps(TRACK_JSON)),
                ]),
                clock=lambda: 1_700_000_010.0,
                audio_probe=lambda: frozenset({BUNDLE_ID}),
                now_playing=no_mirror,
                urlopen=urlopen,
            )
            source.read()
            source.read()
        self.assertEqual(len(calls), 1)


class LyricCacheTest(unittest.TestCase):
    """Cached lines survive a restart and keep the endpoint out of the loop."""

    LRC = "[00:01.00]第一句\n[00:05.00]第二句\n"

    def _source(
        self, database: Path, cache: LyricCache, calls: list[int]
    ) -> NeteaseMusicSource:
        def urlopen(_request, timeout):  # noqa: ARG001
            calls.append(1)
            return FakeResponse({"lrc": {"lyric": self.LRC}})

        return NeteaseMusicSource(
            database=database,
            clock=lambda: 1_700_000_010.0,
            audio_probe=lambda: frozenset({BUNDLE_ID}),
            now_playing=no_mirror,
            urlopen=urlopen,
            cache=cache,
        )

    def test_replay_reads_the_cache_instead_of_the_endpoint(self) -> None:
        calls: list[int] = []
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = make_database(root, [
                ("3455592377", 1_700_000_000_000, json.dumps(TRACK_JSON)),
            ])
            cache = LyricCache(root / "lyrics")
            first = self._source(database, cache, calls).read()
            second = self._source(database, cache, calls).read()
            stored = sorted(entry.name for entry in (root / "lyrics").iterdir())
        assert first is not None and second is not None
        self.assertEqual(len(calls), 1)
        self.assertEqual(second.lines, first.lines)
        self.assertEqual(stored, ["3455592377.json"])

    def test_missing_cache_is_not_created_by_a_failed_request(self) -> None:
        def urlopen(_request, timeout):  # noqa: ARG001
            raise urllib.error.URLError("offline")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = NeteaseMusicSource(
                database=make_database(root, [
                    ("3455592377", 1_700_000_000_000, json.dumps(TRACK_JSON)),
                ]),
                clock=lambda: 1_700_000_010.0,
                audio_probe=lambda: frozenset({BUNDLE_ID}),
                now_playing=no_mirror,
                urlopen=urlopen,
                cache=LyricCache(root / "lyrics"),
            )
            publication = source.read()
            entries = list((root / "lyrics").glob("*.json")) if (
                root / "lyrics").exists() else []
        assert publication is not None
        self.assertEqual(publication.lines, ())
        self.assertEqual(entries, [])

    def test_no_lyrics_is_cached_so_an_instrumental_is_asked_once(self) -> None:
        calls: list[int] = []

        def urlopen(_request, timeout):  # noqa: ARG001
            calls.append(1)
            return FakeResponse({"lrc": {"lyric": ""}})

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = make_database(root, [
                ("3455592377", 1_700_000_000_000, json.dumps(TRACK_JSON)),
            ])
            cache = LyricCache(root / "lyrics")
            for _ in range(2):
                NeteaseMusicSource(
                    database=database,
                    clock=lambda: 1_700_000_010.0,
                    audio_probe=lambda: frozenset({BUNDLE_ID}),
                    now_playing=no_mirror,
                    urlopen=urlopen,
                    cache=cache,
                ).read()
        self.assertEqual(len(calls), 1)

    def test_a_document_from_another_parser_version_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cache = LyricCache(Path(directory))
            cache.write("42", (LyricLine(1_000, "旧"),))
            path = Path(directory) / "42.json"
            document = json.loads(path.read_text(encoding="utf-8"))
            document["v"] = CACHE_VERSION + 1
            path.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
            self.assertIsNone(cache.read("42"))

    def test_a_damaged_document_reads_as_a_miss(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cache = LyricCache(Path(directory))
            cache.write("42", (LyricLine(1_000, "第一句"),))
            (Path(directory) / "42.json").write_text("{not json", encoding="utf-8")
            self.assertIsNone(cache.read("42"))

    def test_a_song_id_outside_the_cache_namespace_is_not_a_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cache = LyricCache(Path(directory))
            self.assertIsNone(cache.read("../../etc/passwd"))
            self.assertIsNone(cache.read(""))

    def test_the_cache_stops_growing_past_its_ceiling(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cache = LyricCache(Path(directory))
            for index in range(MAX_CACHE_ENTRIES + 8):
                cache.write(str(index), (LyricLine(index * 1000, "行"),))
            stored = sorted((Path(directory)).glob("*.json"))
        self.assertEqual(len(stored), MAX_CACHE_ENTRIES)


class PlayerRevisionTest(unittest.TestCase):
    """The publisher watches the player, so a seek does not wait for a tick."""

    def _source(self, directory: Path, holder: list) -> NeteaseMusicSource:
        return NeteaseMusicSource(
            database=make_database(directory, [
                ("3455592377", 1_700_000_000_000, json.dumps(TRACK_JSON)),
            ]),
            clock=lambda: 1_700_000_042.0,
            audio_probe=lambda: frozenset({BUNDLE_ID}),
            now_playing=lambda: holder[0],
            urlopen=lambda _request, timeout: FakeResponse({"lrc": {"lyric": ""}}),
        )

    def test_only_a_new_player_report_advances_the_revision(self) -> None:
        holder = [NeteaseMirrorTest()._state()]
        with tempfile.TemporaryDirectory() as directory:
            source = self._source(Path(directory), holder)
            first = source.revision()
            # The mirror keeps handing out the same payload: nothing happened.
            self.assertEqual(first, source.revision())
            # The client seeked, so the payload it publishes changed.
            holder[0] = NeteaseMirrorTest()._state(elapsed_seconds=30.0)
            self.assertEqual(first + 1, source.revision())

    def test_a_missing_payload_leaves_the_revision_alone(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = self._source(Path(directory), [None])
            self.assertEqual(0, source.revision())
            self.assertEqual(0, source.revision())


class StubUsageSource:
    """Minimal usage source so the service loop has its usual publication."""

    providers = (Provider.DEEPSEEK,)

    async def collect(self, attempted_at: int | None = None):
        return {
            Provider.DEEPSEEK: ProviderUsage(
                provider=Provider.DEEPSEEK,
                state=SourceState.OK,
                sampled_at=attempted_at or 0,
                balance=Decimal("1.00"),
                currency="CNY",
            )
        }


class StubMusicSource:
    def __init__(self, publication: NowPlayingPublication, delay: float = 0.0) -> None:
        self.publication = publication
        self.delay = delay
        self.calls = 0

    async def collect(self) -> NowPlayingPublication:
        self.calls += 1
        if self.delay:
            await asyncio.sleep(self.delay)
        return self.publication


class _ReportingMusicSource(StubMusicSource):
    """A source whose player reports something new after a delay."""

    def __init__(self, publication: NowPlayingPublication, *, report_after: float) -> None:
        super().__init__(publication)
        self._started = time.monotonic()
        self._report_after = report_after

    def revision(self) -> int:
        return int(time.monotonic() - self._started >= self._report_after)


PUBLICATION = NowPlayingPublication(
    title="证据", artist="杨乃文", album="", position_seconds=54,
    duration_seconds=275, playing=True, lines=("第一句", "第二句"), current_line=0,
)


class _RecordingTransport:
    """Device stub that records the commands a session sends."""

    def __init__(self, capabilities: frozenset[str]) -> None:
        self.connected = False
        self.sent: list[tuple[bytes, str, int]] = []
        self.device_status = DeviceStatus(
            name="Panel", secure=True, protocol=1, page="overview",
            capabilities=capabilities,
        )

    async def connect(self) -> None:
        self.connected = True

    async def send_command(self, payload: bytes, command: str, sequence: int) -> None:
        self.sent.append((payload, command, sequence))

    async def close(self) -> None:
        self.connected = False


class ServicePublishTest(unittest.IsolatedAsyncioTestCase):
    """The music page is fed on its own cadence, only to capable devices."""

    async def _run_briefly(self, capabilities: frozenset[str]) -> "_RecordingTransport":
        transport = _RecordingTransport(capabilities)
        service = MultiDeviceBridgeService(
            StubUsageSource(),
            (DeviceSession("Panel", transport),),
            interval=10.0,
            publish_interval=10.0,
            music_source=StubMusicSource(PUBLICATION),
            music_interval=0.01,
        )
        task = asyncio.create_task(service.run())
        try:
            for _ in range(200):
                if any(command == "now_playing" for _p, command, _n in transport.sent):
                    break
                await asyncio.sleep(0.01)
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        return transport

    async def test_capable_device_receives_the_song(self) -> None:
        transport = await self._run_briefly(
            frozenset({"usage.v1", NOW_PLAYING_CAPABILITY})
        )
        frames = [payload for payload, command, _ in transport.sent if command == "now_playing"]
        self.assertTrue(frames)
        message = json.loads(frames[0].decode("utf-8"))
        self.assertEqual(message["title"], "证据")
        self.assertEqual(message["state"], "playing")

    async def test_device_without_the_capability_is_left_alone(self) -> None:
        transport = await self._run_briefly(frozenset({"usage.v1"}))
        self.assertFalse([command for _p, command, _n in transport.sent
                          if command == "now_playing"])

    async def _first_frame_delay(self, source: object, interval: float) -> float:
        transport = _RecordingTransport(
            frozenset({"usage.v1", NOW_PLAYING_CAPABILITY})
        )
        service = MultiDeviceBridgeService(
            StubUsageSource(),
            (DeviceSession("Panel", transport),),
            interval=10.0,
            publish_interval=10.0,
            music_source=source,
            music_interval=interval,
        )
        task = asyncio.create_task(service.run())
        try:
            started = time.monotonic()
            for _ in range(500):
                if any(command == "now_playing" for _p, command, _n in transport.sent):
                    break
                await asyncio.sleep(0.01)
            return time.monotonic() - started
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    async def test_a_player_report_is_published_before_the_next_tick(self) -> None:
        # The cadence is a second, and the player reports a seek after three
        # tenths of one: the panel must hear about it then, not a tick later.
        source = _ReportingMusicSource(PUBLICATION, report_after=0.3)
        self.assertLess(await self._first_frame_delay(source, 1.0), 0.7)

    async def test_cadence_is_not_lengthened_by_collection_cost(self) -> None:
        transport = _RecordingTransport(
            frozenset({"usage.v1", NOW_PLAYING_CAPABILITY})
        )
        service = MultiDeviceBridgeService(
            StubUsageSource(),
            (DeviceSession("Panel", transport),),
            interval=10.0,
            publish_interval=10.0,
            music_source=StubMusicSource(PUBLICATION, delay=0.04),
            music_interval=0.05,
        )
        task = asyncio.create_task(service.run())
        try:
            started = time.monotonic()
            for _ in range(500):
                sent = [c for _p, c, _n in transport.sent if c == "now_playing"]
                if len(sent) >= 5:
                    break
                await asyncio.sleep(0.005)
            elapsed = time.monotonic() - started
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        # Five deadline-aligned ticks take 0.25 s; sleeping after the collection
        # instead would take 0.45 s because every tick pays the collection cost.
        self.assertLess(elapsed, 0.36)


class CreditFilterTest(unittest.TestCase):
    LRC = (
        "[00:00.00] 作词 : 琴路漫漫\n"
        "[00:00.05]演唱：张德伊玲\n"
        "[00:00.29]看晨曦吻大地  看落日寻天际\n"
        "[02:51.03]吉他：潘春宇\n"
        "[00:02.54]混音/母带：吴润林\n"
        "[03:02.17]本歌曲来自〖飓风计划〗\n"
        "[03:03.57]10亿现金激励，千亿流量扶持！\n"
        "[03:04.92]业务联系：jf399@vip.163.com\n"
        "[03:05.10]大提琴：张平\n"
        "[03:05.60]和声编写 BACKGROUND VOCAL ARRANGEMENT: Billy Koh\n"
        "[03:06.00]女：你不是喜欢亚比啦\n"
    )

    def test_roles_and_promotions_are_credits(self) -> None:
        self.assertTrue(is_credit_line("作词 : 琴路漫漫"))
        self.assertTrue(is_credit_line("混音/母带：吴润林"))
        self.assertTrue(is_credit_line("本歌曲来自〖飓风计划〗"))
        self.assertTrue(is_credit_line("业务联系：jf399@vip.163.com"))
        self.assertFalse(is_credit_line("曲终人散：谁在唱"))

    def test_roles_with_english_gloss_are_credits(self) -> None:
        self.assertTrue(is_credit_line("录音师（Recording Engineer） : 文魁"))
        self.assertTrue(is_credit_line("混音工程师（Mixing Engineer） : 文魁"))
        self.assertTrue(is_credit_line("母带工程师（Mastering Engineer） : 全相彦"))
        self.assertFalse(is_credit_line("时间（Time）：谁在等"))

    def test_the_players_named_on_their_own_line_are_credits(self) -> None:
        self.assertTrue(is_credit_line("大提琴 : 张平"))
        self.assertTrue(is_credit_line("单簧管：吴乐"))
        self.assertTrue(is_credit_line("萨克斯：李世海"))
        self.assertTrue(is_credit_line("合成器：Terence Teo"))
        self.assertTrue(is_credit_line("Cello: 张平"))

    def test_a_role_that_carries_an_english_gloss_is_a_credit(self) -> None:
        self.assertTrue(is_credit_line("和声编写 BACKGROUND VOCAL ARRANGEMENT: Billy Koh"))
        self.assertTrue(is_credit_line("制作协力 PRODUCTION ASSISTANCE: 黄冠龙"))
        self.assertTrue(is_credit_line("制作团队Producer  Studio: 某某"))
        self.assertTrue(is_credit_line("词Lyrics: 某某"))
        self.assertTrue(is_credit_line("Rap词: 某某"))

    def test_a_role_with_a_qualifier_in_front_or_a_noun_behind_is_a_credit(self) -> None:
        self.assertTrue(is_credit_line("制作统筹 : 潘卉琦"))
        self.assertTrue(is_credit_line("杜比全景声混音工程师 : 张步若"))
        self.assertTrue(is_credit_line("音乐混音：崔禄"))
        self.assertTrue(is_credit_line("音响总监 : 何飚"))
        self.assertTrue(is_credit_line("PGM: 郭锦阳"))

    def test_a_sung_line_that_mentions_a_role_is_not_a_credit(self) -> None:
        # Only the start of the line names a role; the instrument is in the words.
        self.assertFalse(is_credit_line("我听见大提琴的声音：远了"))
        self.assertFalse(is_credit_line("女：你不是喜欢亚比啦，喊我出来干嘛哦"))
        self.assertFalse(is_credit_line("男：我觉得你蛮亚的啊"))

    def test_only_sung_lines_survive_the_fetch(self) -> None:
        def urlopen(_request, timeout):  # noqa: ARG001
            return FakeResponse({"lrc": {"lyric": self.LRC}})

        self.assertEqual(
            fetch_lyrics("42", urlopen=urlopen),
            (
                LyricLine(290, "看晨曦吻大地  看落日寻天际"),
                LyricLine(186_000, "女:你不是喜欢亚比啦"),
            ),
        )


class EncodingTest(unittest.TestCase):
    def test_fields_and_capability(self) -> None:
        publication = NowPlayingPublication(
            title="后会友期", artist="许飞", album="主题曲",
            position_seconds=54, duration_seconds=231, playing=False,
            lines=("第一句", "第二句"), current_line=1,
        )
        raw = encode_now_playing(publication, 42)
        self.assertTrue(raw.endswith(b"\n"))
        message = json.loads(raw.decode("utf-8"))
        self.assertEqual(message["cmd"], "now_playing")
        self.assertEqual(message["seq"], "42")
        self.assertEqual(message["title"], "后会友期")
        self.assertEqual(message["state"], "paused")
        self.assertEqual(message["lines"], "第一句\n第二句")
        self.assertEqual(message["current"], "1")
        self.assertEqual(NOW_PLAYING_CAPABILITY, "nowplaying.v1")

    def test_the_millisecond_timeline_travels_with_the_lines(self) -> None:
        publication = NowPlayingPublication(
            title="歌", artist="", album="", position_seconds=54,
            duration_seconds=231, playing=True,
            lines=("第一句", "第二句"), current_line=1,
            position_ms=54_320, line_times_ms=(53_000, 54_320),
        )
        message = json.loads(encode_now_playing(publication, 1).decode("utf-8"))
        self.assertEqual(message["position_ms"], "54320")
        self.assertEqual(message["line_times"], "53000\n54320")

    def test_a_source_without_a_timeline_sends_no_timestamps(self) -> None:
        publication = NowPlayingPublication(
            title="歌", artist="", album="", position_seconds=54,
            duration_seconds=231, playing=True,
            lines=("第一句", "第二句"), current_line=0,
        )
        message = json.loads(encode_now_playing(publication, 1).decode("utf-8"))
        self.assertNotIn("position_ms", message)
        self.assertNotIn("line_times", message)

    def test_line_end_travels_only_when_it_is_known(self) -> None:
        known = NowPlayingPublication(
            title="歌", artist="", album="", position_seconds=54,
            duration_seconds=231, playing=True,
            lines=("第一句", "第二句"), current_line=0, line_end_seconds=60,
        )
        self.assertEqual(
            json.loads(encode_now_playing(known, 1).decode("utf-8"))["line_end"], "60"
        )
        unknown = NowPlayingPublication(
            title="歌", artist="", album="", position_seconds=54,
            duration_seconds=231, playing=True,
        )
        self.assertNotIn("line_end", json.loads(encode_now_playing(unknown, 2).decode("utf-8")))

    def test_chinese_travels_as_utf8_not_escapes(self) -> None:
        raw = encode_now_playing(
            NowPlayingPublication("证据", "杨乃文", "", 1, 2, True), 1,
        )
        self.assertIn("证据".encode("utf-8"), raw)

    def test_lines_and_text_are_bounded(self) -> None:
        publication = NowPlayingPublication(
            title="长" * 400, artist="", album="", position_seconds=0,
            duration_seconds=0, playing=True,
            lines=tuple("行" * 200 for _ in range(MAX_LINES + 4)), current_line=0,
        )
        message = json.loads(encode_now_playing(publication, 7).decode("utf-8"))
        self.assertLessEqual(len(message["title"].encode("utf-8")), MAX_TITLE_BYTES)
        lines = message["lines"].split("\n")
        self.assertEqual(len(lines), MAX_LINES)
        for line in lines:
            self.assertLessEqual(len(line.encode("utf-8")), MAX_LINE_BYTES)

    def test_frame_stays_inside_the_protocol_limit(self) -> None:
        publication = NowPlayingPublication(
            "标" * 100, "歌" * 100, "", 0, 0, True,
            tuple("词" * 100 for _ in range(MAX_LINES)), 0,
        )
        self.assertLessEqual(len(encode_now_playing(publication, 1)), 4096)


class NeteaseMirrorTest(unittest.TestCase):
    """The system mirror supplies what the database cannot: pause and seeks."""

    def _source(
        self,
        directory: Path,
        state: NowPlayingState | None,
        *,
        record: dict[str, object] | None = None,
        probe=frozenset({BUNDLE_ID}),
    ) -> NeteaseMusicSource:
        return NeteaseMusicSource(
            database=make_database(directory, [
                ("3455592377", 1_700_000_000_000, json.dumps(record or TRACK_JSON)),
            ]),
            clock=lambda: 1_700_000_042.0,
            audio_probe=lambda: probe,
            now_playing=lambda: state,
            urlopen=lambda _request, timeout: FakeResponse({
                "lrc": {"lyric": "[00:00.00]a\n[00:40.00]b\n[00:50.00]c\n",
                        "tlyric": {"lyric": ""}},
            }),
        )

    def _state(self, **overrides: object) -> NowPlayingState:
        fields: dict[str, object] = {
            "bundle_id": BUNDLE_ID,
            "process_id": 90106,
            "title": "后会友期",
            "artist": "许飞",
            "album": "后会友期 主题曲",
            "duration_seconds": 231.435,
            "elapsed_seconds": 42.0,
            "anchor_epoch": 1_700_000_042.0,
            "playback_rate": 1.0,
            "playing": True,
            "received_at": 1_700_000_000.0,
        }
        fields.update(overrides)
        return NowPlayingState(**fields)  # type: ignore[arg-type]

    def test_a_pause_is_reported_at_the_anchored_position(self) -> None:
        state = self._state(
            playing=False, playback_rate=0.0, elapsed_seconds=120.65,
        )
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(Path(directory), state).read()
        assert publication is not None
        # The database clock alone would say 42 s and "playing".
        self.assertFalse(publication.playing)
        self.assertEqual(publication.position_seconds, 120)
        # The mirror states the playhead in fractions of a second, and the panel
        # needs them: the line that is sung at 120.650 s is not the one at 120 s.
        self.assertEqual(publication.position_ms, 120_650)

    def test_a_title_in_another_compatibility_form_still_matches(self) -> None:
        # The client draws the radical, the history row keeps the ideograph.
        state = self._state(title="\u2f00\u4e2a\u2f08", elapsed_seconds=120.65)
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(
                Path(directory), state, record={**TRACK_JSON, "name": "\u4e00\u4e2a\u4eba"},
            ).read()
        assert publication is not None
        # The mirror's 120 s wins only when the two are read as the same song.
        self.assertEqual(publication.position_seconds, 120)
        self.assertEqual(publication.title, "\u4e00\u4e2a\u4eba")

    def test_a_seek_moves_the_playhead_without_a_new_track(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(Path(directory), self._state()).read()
        assert publication is not None
        self.assertEqual(publication.position_seconds, 42)
        self.assertTrue(publication.playing)

    def test_the_mirror_of_another_client_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(
                Path(directory),
                self._state(bundle_id="com.apple.Music", elapsed_seconds=120.0),
                probe=frozenset(),
            ).read()
        assert publication is not None
        self.assertEqual(publication.position_seconds, 42)
        self.assertFalse(publication.playing)

    def test_the_mirror_of_another_song_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(
                Path(directory), self._state(title="另一首歌", elapsed_seconds=120.0)
            ).read()
        assert publication is not None
        self.assertEqual(publication.position_seconds, 42)

    def test_an_edition_suffix_still_names_the_same_song(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(
                Path(directory), self._state(title="后会友期 (Live)")
            ).read()
        assert publication is not None
        self.assertEqual(publication.position_seconds, 42)

    def test_a_row_without_a_duration_takes_the_mirror_duration(self) -> None:
        record = dict(TRACK_JSON, duration=0)
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(
                Path(directory), self._state(), record=record
            ).read()
        assert publication is not None
        self.assertEqual(publication.duration_seconds, 231)

    def test_without_a_mirror_the_database_clock_still_answers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            publication = self._source(Path(directory), None).read()
        assert publication is not None
        self.assertEqual(publication.position_seconds, 42)
        self.assertTrue(publication.playing)

    def test_a_client_that_quits_stops_the_song(self) -> None:
        # The mirror reports the client while it plays, then stops reporting:
        # the client quit, so the song must not keep playing on the panel.
        states: list[NowPlayingState | None] = [self._state(elapsed_seconds=120.0), None]
        # The client is gone once it quits, so it holds no audio output any more.
        clients: list[frozenset[str]] = [frozenset()]

        def mirror() -> NowPlayingState | None:
            return states.pop(0) if states else None

        def probe() -> frozenset[str]:
            return clients.pop(0) if clients else frozenset()

        with tempfile.TemporaryDirectory() as directory:
            source = NeteaseMusicSource(
                database=make_database(Path(directory), [
                    ("3455592377", 1_700_000_000_000, json.dumps(TRACK_JSON)),
                ]),
                clock=lambda: 1_700_000_042.0,
                audio_probe=probe,
                now_playing=mirror,
                urlopen=lambda _request, timeout: FakeResponse({
                    "lrc": {"lyric": "[00:00.00]a\n[00:40.00]b\n"},
                }),
            )
            playing = source.read()
            stopped = source.read()
        assert playing is not None and stopped is not None
        self.assertTrue(playing.playing)
        self.assertEqual(playing.position_seconds, 120)
        self.assertFalse(stopped.playing)
        # The playhead stays where the mirror left it instead of running on.
        self.assertEqual(stopped.position_seconds, 120)


if __name__ == "__main__":
    unittest.main()
