"""Now-playing source for the NetEase Cloud Music desktop app on macOS.

The desktop client records every track it starts in its own local database.
The newest row of ``historyTracks`` carries the song id, the title, the
artists, the album, the duration and the millisecond timestamp the track
started at, which is enough to name the song and to place the playhead
without asking the application for anything.

The database alone cannot answer two things the panel needs: it never records
the pause state, and it never records a seek, so a computed playhead drifts
until the next track. Both come from the system now-playing mirror instead
(`macos_nowplaying`), which the client feeds through `MPNowPlayingInfoCenter`.
The database stays the source of the song identity and of the lyric lookup key;
the mirror only overrides the playhead and the playing flag, and only while it
reports this same song.

A client that quits stops publishing, and the mirror says so. The last track
then stays on the panel with the playhead where the mirror left it and the
song marked as stopped, so the lyrics stop scrolling instead of running on
with a player that is gone.

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
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from quotaframe_bridge.domain.models import NowPlayingPublication

if TYPE_CHECKING:  # pragma: no cover - annotation only, never imported to run
    from quotaframe_bridge.sources.macos_nowplaying import NowPlayingState

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
# The panel's music page draws five lyric rows from the line being sung onward,
# so the window has to carry four lines after it; the lead-in keeps a consumer
# that shows context above the highlight working.
WINDOW_AFTER = 4
# A track that ended long ago is history, not a song in progress.
ENDED_GRACE_SECONDS = 5
CACHE_DIR_RELATIVE = Path("Library/Caches/quotaframe/lyrics")
# A cached document is only reused by the parser version that wrote it, so an
# entry turns stale instead of outliving a change to parsing or filtering.
CACHE_VERSION = 4
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
    "古筝", "二胡", "琵琶", "童声", "伴唱", "导演", "编剧", "词", "曲",
    # Players and sections named on their own line.
    "大提琴", "小提琴", "中提琴", "低音提琴", "单簧管", "双簧管", "萨克斯", "长笛",
    "短笛", "小号", "长号", "圆号", "大号", "竖琴", "合成器", "架子鼓", "打击乐",
    "手风琴", "口琴", "曼陀林", "演奏", "念白",
    # Production credits that appear beside them.
    "合音", "合声", "和音", "艺术顾问", "对唱设计", "推广营销", "音乐工程",
    "制作协力", "制作团队",
)
# Nouns that follow a role in the full name of the credit, as in
# "杜比全景声混音工程师".
CREDIT_ROLE_NOUNS = ("工程师", "制作人", "助理", "指导", "设计", "编写", "协力", "统筹", "团队", "师")
# Roles the client writes in Latin script, either alone or after the Chinese name.
CREDIT_LATIN_ROLES = (
    "op", "sp", "cello", "violin", "viola", "piano", "guitar", "bass", "drums",
    "strings", "synth", "sax", "flute", "harp", "trumpet", "trombone", "sitar",
    "clarinet", "oboe", "mandolin", "accordion", "pgm",
)
CREDIT_MARKERS = (
    "本歌曲来自", "业务联系", "现金激励", "流量扶持", "未经许可", "侵权必究",
    "版权", "http", "@",
)
# Roles arrive bare ("混音：…") and with an English gloss ("录音师（Recording Engineer）: …").
_CREDIT_PREFIX = re.compile(r"^\s*([^：:]{1,48})[：:]")
# The Latin part a role may carry: a gloss after the Chinese name.
_CREDIT_LATIN = re.compile(r"[A-Za-z0-9\s]+")
_CREDIT_GLOSS = re.compile(r"[（(][^）)]*[）)]")


class NeteaseMusicError(RuntimeError):
    """The local database or the lyric endpoint did not answer as expected."""


# Zero-width marks survive compatibility normalisation and would reach the panel
# as a box where nothing should be drawn.
_ZERO_WIDTH = dict.fromkeys(map(ord, "\u200b\u200c\u200d\u200e\u200f\ufeff\u00ad"), None)


def display_text(text: str) -> str:
    """Fold `text` onto the characters the panel can draw.

    Lyrics and metadata reach the panel in compatibility forms the 12 x 12 font
    does not cover: Kangxi radicals standing in for the ideographs they derive
    from (``U+2F00`` for ``一``, ``U+2F08`` for ``人``) and full-width
    punctuation, which has no glyph while the built-in ASCII font draws the
    plain form. Compatibility normalisation folds both onto covered characters,
    so the panel never draws the box it uses for a missing glyph. Folding runs
    before any byte limit, so a line is measured in the bytes that will arrive.
    """

    return unicodedata.normalize("NFKC", text).translate(_ZERO_WIDTH)


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


_TITLE_SEPARATOR = re.compile(r"[\s\u3000]+")


def _comparable_title(text: str) -> str:
    # The client and its history row can spell the same title with different
    # compatibility forms, so they are compared as the panel would draw them.
    return _TITLE_SEPARATOR.sub("", display_text(text)).casefold()


def _same_title(mirrored: str, recorded: str) -> bool:
    """Whether the mirror names the song the history row names.

    The client publishes the title it shows; the row keeps the name it stored.
    Edition suffixes ("(Live)") and spacing differ between the two, so a shared
    prefix counts as the same song.
    """

    left, right = _comparable_title(mirrored), _comparable_title(recorded)
    if not left or not right:
        return False
    return left == right or left.startswith(right) or right.startswith(left)


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
        title=display_text(title) if isinstance(title, str) else "",
        artist=display_text(_artists(payload)),
        album=display_text(_album(payload)),
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


def _names_a_credit(prefix: str) -> bool:
    """Whether the text before a colon names a role rather than a sung word.

    The Latin part is optional in both directions: a Chinese role can carry an
    English gloss ("和声编写 BACKGROUND VOCAL ARRANGEMENT"), and an English role
    can stand on its own ("Cello"). Roles are matched from the start so that a
    sung line may mention an instrument without being taken for a credit, and a
    one-character role only counts when it is the whole prefix.
    """

    written = prefix.casefold()
    leading = _CREDIT_LATIN.sub("", prefix).strip().casefold()
    for role in CREDIT_ROLES:
        if len(role) == 1:
            if leading == role:
                return True
            continue
        # A compound role such as "混音/母带" still names the credit, and a
        # qualifier can sit in front of the role ("音乐混音", "制作统筹") or a noun
        # behind it ("杜比全景声混音工程师").
        if leading.startswith(role) or written.startswith(role):
            return True
        if leading.endswith(role) or any(
            leading.endswith(role + noun) for noun in CREDIT_ROLE_NOUNS
        ):
            return True
    return written in CREDIT_LATIN_ROLES


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
    # Drop the bracketed gloss so that "录音师（Recording Engineer）" reduces to the
    # role itself; `_names_a_credit` then drops the unbracketed one.
    prefix = _CREDIT_GLOSS.sub("", match.group(1)).strip()
    return _names_a_credit(prefix)


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
        content = display_text(raw[stamps[-1].end():].strip())
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


def line_end_seconds(lines: Sequence[LyricLine], position_seconds: int) -> int:
    """Position at which the line being sung stops, or 0 when it is unknown.

    A panel only uses it as an upper bound for scrolling a line that does not fit, so an
    interlude after the line simply leaves it resting at its end. The last line of a song has
    no successor, so it reports nothing rather than guessing an end.
    """

    if not lines:
        return 0
    index = 0
    for candidate, line in enumerate(lines):
        if line.time_seconds <= position_seconds:
            index = candidate
        else:
            break
    if index + 1 >= len(lines):
        return 0
    return lines[index + 1].time_seconds


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


@dataclass(frozen=True, slots=True)
class _MirrorPlayhead:
    """What the system mirror contributes to one publication."""

    position_seconds: int
    playing: bool
    duration_seconds: int


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
        now_playing: Callable[[], NowPlayingState | None] | None = None,
    ) -> None:
        self.database = database or default_database()
        self.timeout = timeout
        self._clock = clock
        self._urlopen = urlopen
        self._audio_probe = audio_probe
        self._cache = cache
        self._now_playing = now_playing
        self._lyrics: dict[str, tuple[LyricLine, ...]] = {}
        self._lyrics_song_id: str | None = None
        # The last playhead the mirror reported, with the song it belongs to.
        self._last_mirror: tuple[str, _MirrorPlayhead] | None = None

    @property
    def supported(self) -> bool:
        """Whether this host can read the NetEase client."""

        return sys.platform == "darwin"

    def _playing_bundle_ids(self) -> frozenset[str]:
        if self._audio_probe is not None:
            return self._audio_probe()
        from quotaframe_bridge.sources.macos_audio import playing_bundle_ids

        return playing_bundle_ids()

    def _mirror_state(self) -> NowPlayingState | None:
        """The system now-playing mirror, or None when it has nothing to say."""

        if self._now_playing is not None:
            return self._now_playing()
        from quotaframe_bridge.sources.macos_nowplaying import current_state

        return current_state()

    def _mirror_for(self, track: Track, now: float) -> _MirrorPlayhead | None:
        """Playhead and playing flag from the mirror for this track, if it has them.

        The mirror only speaks for whichever client the system considers active,
        and a track change may reach it before the client writes its history
        row. A mismatch means the two are not describing the same song, so the
        caller keeps its own timing rather than mixing the two.
        """

        state = self._mirror_state()
        if state is None or state.bundle_id != BUNDLE_ID:
            return None
        if not _same_title(state.title, track.title):
            LOGGER.debug("now-playing mirror describes a different song")
            return None
        playhead = _MirrorPlayhead(
            position_seconds=int(state.position_seconds(now)),
            playing=state.playing,
            duration_seconds=int(round(state.duration_seconds)),
        )
        # Kept for the case where the client stops publishing: the playhead then
        # stays where the mirror left it instead of running on with the clock.
        self._last_mirror = (track.song_id, playhead)
        return playhead

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
        now = self._clock()
        now_ms = int(now * 1000)
        elapsed = max(0, (now_ms - track.started_at_ms) // 1000)
        mirrored = self._mirror_for(track, now)
        duration = track.duration_seconds
        if mirrored is not None:
            position = mirrored.position_seconds
            playing = mirrored.playing
            if duration <= 0:
                duration = mirrored.duration_seconds
        else:
            remembered = self._last_mirror
            if remembered is not None and remembered[0] == track.song_id:
                # The client stopped publishing: it quit. The playhead stays
                # where the mirror left it, and the song is not playing any
                # more, so the panel stops scrolling its lyrics.
                position = remembered[1].position_seconds
                playing = (
                    remembered[1].playing and BUNDLE_ID in self._playing_bundle_ids()
                )
            else:
                # Without the mirror the playhead is the distance from the moment
                # the client wrote the row, so a pause reads as playing until the
                # audio output stream tears down.
                position = elapsed
                ended = duration > 0 and elapsed > duration + ENDED_GRACE_SECONDS
                playing = not ended and BUNDLE_ID in self._playing_bundle_ids()
        # A track that has run out keeps the playhead at its end instead of
        # running past the duration while the client sits paused.
        if duration > 0:
            position = min(position, duration)
        lyrics = self._lyrics_for(track.song_id)
        window = select_window(lyrics, position)
        current = current_index(window, position)
        return NowPlayingPublication(
            title=track.title,
            artist=track.artist,
            album=track.album,
            position_seconds=position,
            duration_seconds=duration,
            playing=playing,
            lines=tuple(line.text for line in window),
            current_line=current,
            line_end_seconds=line_end_seconds(lyrics, position),
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
    "display_text",
    "fetch_lyrics",
    "is_credit_line",
    "parse_lrc",
    "parse_track",
    "read_current_track",
    "current_index",
    "line_end_seconds",
    "select_window",
]
