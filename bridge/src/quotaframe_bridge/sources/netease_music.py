"""Now-playing source for the NetEase Cloud Music desktop app on macOS.

The desktop client records every track it starts in its own local database.
The newest row of ``historyTracks`` carries the song id, the title, the
artists, the album, the duration and the millisecond timestamp the track
started at, which is enough to name the song and to place the playhead
without asking the application for anything.

Lyrics are read from the public song endpoints the same client uses for its
own lyric panel. They are anonymous requests: no cookie, no account and no
credential leaves the computer, and the response is cached per song so a
request is made once per track, on disk as well as in memory.

Only the song title, the artists, the album and the lyric text are read from
the local database and from the lyric endpoint. Nothing else in the client's
history is touched.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import re
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from quotaframe_bridge.domain.models import NowPlayingPublication

LOGGER = logging.getLogger(__name__)

BUNDLE_ID = "com.netease.163music"
DATABASE_RELATIVE = (
    Path("Library/Containers/com.netease.163music/Data/Documents/storage")
    / "sqlite_storage.sqlite3"
)
LYRIC_ENDPOINT = "https://music.163.com/api/song/lyric"
REQUEST_TIMEOUT_SECONDS = 10.0
MAX_RESPONSE_BYTES = 256 * 1024
# Lines kept before and after the current one so the panel can show context.
WINDOW_BEFORE = 1
WINDOW_AFTER = 3
# A track that ended long ago is history, not a song in progress.
ENDED_GRACE_SECONDS = 5
CACHE_DIR_RELATIVE = Path("Library/Caches/quotaframe/lyrics")
# A cached document is only reused by the parser version that wrote it, so an
# entry turns stale instead of outliving a change to parsing or filtering.
CACHE_VERSION = 1
# Small text files per song; the ceiling keeps a long listening history from
# growing without bound.
MAX_CACHE_ENTRIES = 512
_CACHE_NAME = re.compile(r"^[0-9A-Za-z_-]{1,40}$")

_LRC_TIMESTAMP = re.compile(r"\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]")
# Credits the client appends to a lyric file: roles before a colon, and the promo block.
CREDIT_ROLES = (
    "作词", "作曲", "编曲", "制作人", "出品", "发行", "监制", "录音", "混音", "母带",
    "统筹", "吉他", "贝斯", "鼓", "键盘", "和声", "弦乐", "演唱", "原唱", "翻唱",
    "策划", "企划", "宣传", "后期", "人声", "封面", "美术", "总监", "乐队", "钢琴",
    "古筝", "二胡", "琵琶", "童声", "伴唱", "导演", "编剧", "词", "曲", "OP", "SP",
)
CREDIT_MARKERS = (
    "本歌曲来自", "业务联系", "现金激励", "流量扶持", "未经许可", "侵权必究",
    "版权", "http", "@",
)
# Roles arrive bare ("混音：…") and with an English gloss ("录音师（Recording Engineer）: …").
_CREDIT_PREFIX = re.compile(r"^\s*([^：:]{1,32})[：:]")
_CREDIT_GLOSS = re.compile(r"[（(][^）)]*[）)]")


class NeteaseMusicError(RuntimeError):
    """The local database or the lyric endpoint did not answer as expected."""


@dataclass(frozen=True, slots=True)
class LyricLine:
    """One timed lyric line."""

    time_seconds: int
    text: str


@dataclass(frozen=True, slots=True)
class Track:
    """The track the client started last."""

    song_id: str
    title: str
    artist: str
    album: str
    duration_seconds: int
    started_at_ms: int


def default_database() -> Path:
    """Path of the client's local database for the current user."""

    return Path.home() / DATABASE_RELATIVE


def default_cache_dir() -> Path:
    """Path of the per-user lyric cache directory."""

    return Path.home() / CACHE_DIR_RELATIVE


def _artists(track: Mapping[str, object]) -> str:
    artists = track.get("artists")
    if not isinstance(artists, Sequence) or isinstance(artists, (str, bytes)):
        return ""
    names = []
    for artist in artists:
        if isinstance(artist, Mapping):
            name = artist.get("name")
            if isinstance(name, str) and name:
                names.append(name)
    return " / ".join(names)


def _album(track: Mapping[str, object]) -> str:
    album = track.get("album")
    if isinstance(album, Mapping):
        name = album.get("name")
        if isinstance(name, str):
            return name
    return ""


def parse_track(song_id: str, playtime_ms: int, payload: object) -> Track:
    """Decode one ``historyTracks`` row into a track."""

    if not isinstance(payload, Mapping):
        raise NeteaseMusicError("track payload is not an object")
    title = payload.get("name")
    duration = payload.get("duration")
    return Track(
        song_id=song_id,
        title=title if isinstance(title, str) else "",
        artist=_artists(payload),
        album=_album(payload),
        duration_seconds=max(0, int(duration)) // 1000
        if isinstance(duration, (int, float))
        else 0,
        started_at_ms=playtime_ms,
    )


def read_current_track(database: Path) -> Track | None:
    """Read the track the client started last from its own database.

    The database is opened read-only so the client keeps full ownership of
    the file and no journal is written next to it.
    """

    uri = f"file:{urllib.parse.quote(str(database))}?mode=ro"
    try:
        connection = sqlite3.connect(uri, uri=True, timeout=1.0)
    except sqlite3.Error as exc:
        raise NeteaseMusicError(f"local database is unavailable: {exc}") from exc
    try:
        connection.execute("PRAGMA busy_timeout = 1000")
        row = connection.execute(
            "SELECT id, playtime, jsonStr FROM historyTracks "
            "ORDER BY playtime DESC LIMIT 1"
        ).fetchone()
    except sqlite3.Error as exc:
        raise NeteaseMusicError(f"local database is unreadable: {exc}") from exc
    finally:
        connection.close()
    if row is None:
        return None
    song_id, playtime, payload = row
    if not isinstance(song_id, str) or not isinstance(playtime, int):
        return None
    try:
        decoded = json.loads(payload) if isinstance(payload, str) else payload
    except (TypeError, ValueError):
        decoded = None
    return parse_track(song_id, playtime, decoded)


def is_credit_line(text: str) -> bool:
    """Whether a lyric line is a credit or a promotion rather than a sung line.

    The lyric service ends a track's file with the performer credits and, on some
    releases, a promotional block. The panel shows the words being sung, so those
    lines are dropped before the window is chosen.
    """

    stripped = text.strip()
    if not stripped:
        return True
    if any(marker in stripped for marker in CREDIT_MARKERS):
        return True
    match = _CREDIT_PREFIX.match(stripped)
    if not match:
        return False
    # Drop the gloss so that "录音师（Recording Engineer）" reduces to the role itself.
    prefix = _CREDIT_GLOSS.sub("", match.group(1)).strip()
    for role in CREDIT_ROLES:
        # A compound role such as "混音/母带" still names the credit.
        if prefix == role if len(role) == 1 else role in prefix:
            return True
    return False


def parse_lrc(text: str) -> tuple[LyricLine, ...]:
    """Decode an LRC document into timed lines.

    Metadata tags such as ``[ti:...]`` carry no timestamp and are skipped, and
    a line may repeat its text under several timestamps.
    """

    lines: list[LyricLine] = []
    for raw in text.splitlines():
        stamps = list(_LRC_TIMESTAMP.finditer(raw))
        if not stamps:
            continue
        content = raw[stamps[-1].end():].strip()
        for stamp in stamps:
            minutes = int(stamp.group(1))
            seconds = int(stamp.group(2))
            fraction = stamp.group(3)
            if fraction:
                # Two digits are hundredths, three digits are milliseconds.
                divisor = 100 if len(fraction) == 2 else 1000
                seconds += int(fraction) / divisor
            lines.append(LyricLine(int(minutes * 60 + seconds), content))
    lines.sort(key=lambda line: line.time_seconds)
    return tuple(lines)


def select_window(
    lines: Sequence[LyricLine],
    position_seconds: int,
    *,
    before: int = WINDOW_BEFORE,
    after: int = WINDOW_AFTER,
) -> tuple[LyricLine, ...]:
    """Return the lines around the one playing at `position_seconds`."""

    if not lines:
        return ()
    index = 0
    for candidate, line in enumerate(lines):
        if line.time_seconds <= position_seconds:
            index = candidate
        else:
            break
    start = max(0, index - before)
    return tuple(lines[start:index + after + 1])


def current_index(window: Sequence[LyricLine], position_seconds: int) -> int:
    """Index of the line being sung inside `window`."""

    current = 0
    for index, line in enumerate(window):
        if line.time_seconds <= position_seconds:
            current = index
        else:
            break
    return current


def fetch_lyrics(
    song_id: str,
    *,
    urlopen: Callable[..., object] = urllib.request.urlopen,
    timeout: float = REQUEST_TIMEOUT_SECONDS,
) -> tuple[LyricLine, ...]:
    """Read the lyric of one song; blocking, so callers run it in a thread.

    The original lyric is preferred. The translation is used only when the
    original is empty, which happens for instrumental tracks.
    """

    query = urllib.parse.urlencode(
        {"id": song_id, "lv": 1, "kv": 1, "tv": -1, "rv": -1}
    )
    request = urllib.request.Request(
        f"{LYRIC_ENDPOINT}?{query}",
        headers={
            "Accept": "application/json, text/plain, */*",
            "Referer": "https://music.163.com/",
            "User-Agent": "Mozilla/5.0",
        },
        method="GET",
    )
    try:
        with urlopen(request, timeout=timeout) as response:
            raw = response.read(MAX_RESPONSE_BYTES)
    except urllib.error.HTTPError as exc:
        raise NeteaseMusicError(f"lyric request failed with HTTP {exc.code}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise NeteaseMusicError(f"lyric request failed: {exc}") from exc
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise NeteaseMusicError("lyric response is not valid JSON") from exc
    if not isinstance(payload, Mapping):
        raise NeteaseMusicError("lyric response is not an object")
    for key in ("lrc", "tlyric"):
        section = payload.get(key)
        if not isinstance(section, Mapping):
            continue
        text = section.get("lyric")
        if isinstance(text, str) and text.strip():
            return tuple(
                line for line in parse_lrc(text) if not is_credit_line(line.text)
            )
    return ()


class LyricCache:
    """Filtered lyric lines kept on disk, one small JSON document per song.

    The panel asks for the window of the song that is playing, so a song is
    requested again every time it comes back. Keeping the parsed lines removes
    that request: replaying a track, or restarting the Bridge while it plays,
    costs no network at all.

    A document is written only after a successful fetch. A request that failed
    leaves no entry, so the next attempt still tries the endpoint. An entry
    that recorded "no lyrics" is kept, because an instrumental track would
    otherwise be requested on every playback.
    """

    def __init__(self, directory: Path | None = None) -> None:
        self.directory = directory or default_cache_dir()

    def _path(self, song_id: str) -> Path | None:
        if not _CACHE_NAME.match(song_id):
            return None
        return self.directory / f"{song_id}.json"

    def read(self, song_id: str) -> tuple[LyricLine, ...] | None:
        """Cached lines for `song_id`, or None when the entry is missing or stale."""

        path = self._path(song_id)
        if path is None:
            return None
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            return None
        if not isinstance(document, Mapping):
            return None
        if document.get("v") != CACHE_VERSION or document.get("id") != song_id:
            return None
        entries = document.get("lines")
        if not isinstance(entries, Sequence) or isinstance(entries, (str, bytes)):
            return None
        lines: list[LyricLine] = []
        for entry in entries:
            if not isinstance(entry, Mapping):
                return None
            at, text = entry.get("t"), entry.get("x")
            if not isinstance(at, int) or not isinstance(text, str):
                return None
            lines.append(LyricLine(at, text))
        return tuple(lines)

    def write(self, song_id: str, lines: Sequence[LyricLine]) -> None:
        """Store `lines` as this song's entry, replacing any earlier one."""

        path = self._path(song_id)
        if path is None:
            return
        document = {
            "v": CACHE_VERSION,
            "id": song_id,
            "lines": [{"t": line.time_seconds, "x": line.text} for line in lines],
        }
        try:
            self.directory.mkdir(parents=True, exist_ok=True)
            # The reader may run while a document is being written, so the
            # replace happens on a complete file.
            temporary = path.with_suffix(".tmp")
            temporary.write_text(
                json.dumps(document, ensure_ascii=False), encoding="utf-8"
            )
            os.replace(temporary, path)
        except OSError as exc:
            LOGGER.debug("lyric cache write failed: %s", type(exc).__name__)
            return
        self._prune()

    def _prune(self) -> None:
        """Drop the oldest entries once the cache holds more than its ceiling."""

        try:
            entries = sorted(
                (entry for entry in self.directory.glob("*.json")),
                key=lambda entry: entry.stat().st_mtime,
            )
        except OSError:
            return
        for stale in entries[: max(0, len(entries) - MAX_CACHE_ENTRIES)]:
            try:
                stale.unlink()
            except OSError:
                continue


class NeteaseMusicSource:
    """Publish the track and lyric lines the NetEase client is playing."""

    label = "NetEase Cloud Music"

    def __init__(
        self,
        *,
        database: Path | None = None,
        timeout: float = REQUEST_TIMEOUT_SECONDS,
        clock: Callable[[], float] = time.time,
        urlopen: Callable[..., object] = urllib.request.urlopen,
        audio_probe: Callable[[], frozenset[str]] | None = None,
        cache: LyricCache | None = None,
    ) -> None:
        self.database = database or default_database()
        self.timeout = timeout
        self._clock = clock
        self._urlopen = urlopen
        self._audio_probe = audio_probe
        self._cache = cache
        self._lyrics: dict[str, tuple[LyricLine, ...]] = {}
        self._lyrics_song_id: str | None = None

    @property
    def supported(self) -> bool:
        """Whether this host can read the NetEase client."""

        return sys.platform == "darwin"

    def _playing_bundle_ids(self) -> frozenset[str]:
        if self._audio_probe is not None:
            return self._audio_probe()
        from quotaframe_bridge.sources.macos_audio import playing_bundle_ids

        return playing_bundle_ids()

    def _lyrics_for(self, song_id: str) -> tuple[LyricLine, ...]:
        if self._lyrics_song_id == song_id:
            return self._lyrics.get(song_id, ())
        lines: tuple[LyricLine, ...] | None = None
        cached = self._cache.read(song_id) if self._cache is not None else None
        if cached is not None:
            LOGGER.debug("lyrics for song %s came from the cache", song_id)
            lines = cached
        else:
            try:
                lines = fetch_lyrics(
                    song_id, urlopen=self._urlopen, timeout=self.timeout
                )
            except NeteaseMusicError as exc:
                # A failed request leaves no cache entry, so the next playback
                # tries the endpoint again.
                LOGGER.warning("lyrics for song %s are unavailable: %s", song_id, exc)
                lines = ()
            else:
                if self._cache is not None:
                    self._cache.write(song_id, lines)
        self._lyrics = {song_id: lines}
        self._lyrics_song_id = song_id
        return lines

    def read(self) -> NowPlayingPublication | None:
        """Read one publication; blocking, so callers run it in a thread."""

        try:
            track = read_current_track(self.database)
        except NeteaseMusicError as exc:
            LOGGER.debug("NetEase track is unavailable: %s", exc)
            return None
        if track is None:
            return None
        now_ms = int(self._clock() * 1000)
        elapsed = max(0, (now_ms - track.started_at_ms) // 1000)
        # A track that has run out keeps the playhead at its end instead of
        # running past the duration while the client sits paused.
        position = (
            min(elapsed, track.duration_seconds)
            if track.duration_seconds > 0
            else elapsed
        )
        ended = (
            track.duration_seconds > 0
            and elapsed > track.duration_seconds + ENDED_GRACE_SECONDS
        )
        playing = not ended and BUNDLE_ID in self._playing_bundle_ids()
        window = select_window(self._lyrics_for(track.song_id), position)
        current = current_index(window, position)
        return NowPlayingPublication(
            title=track.title,
            artist=track.artist,
            album=track.album,
            position_seconds=position,
            duration_seconds=track.duration_seconds,
            playing=playing,
            lines=tuple(line.text for line in window),
            current_line=current,
        )

    async def collect(self) -> NowPlayingPublication | None:
        """Read the current song without blocking the event loop."""

        if not self.supported:
            return None
        return await asyncio.to_thread(self.read)


__all__ = [
    "BUNDLE_ID",
    "CACHE_VERSION",
    "MAX_CACHE_ENTRIES",
    "LyricCache",
    "LyricLine",
    "NeteaseMusicError",
    "NeteaseMusicSource",
    "Track",
    "default_cache_dir",
    "default_database",
    "fetch_lyrics",
    "is_credit_line",
    "parse_lrc",
    "parse_track",
    "read_current_track",
    "current_index",
    "select_window",
]
