"""Now-playing state as the system media remote daemon sees it.

A music client publishes its current track through ``MPNowPlayingInfoCenter``:
title, artist, album, duration, an elapsed-time anchor with the moment that
anchor was taken, the playback rate and whether it is playing. ``mediaremoted``
mirrors that on the system bus, which is the only source that shows a pause, a
seek or a track change while it happens — the NetEase client itself writes
neither its playhead nor its pause state anywhere on disk.

The mirror is read through the vendored `media-control` adapter
(`assets/media-control/`, BSD-3-Clause). ``mediaremoted`` refuses the query to
every client without Apple's platform entitlements: an ad-hoc signed process is
registered with ``entitlements=0`` and its playback-queue request answers
``kMRMediaRemoteFrameworkErrorDomain`` 3 ("Operation not permitted"), which is
why ``MRMediaRemoteGetNowPlayingInfo`` hands back null. The adapter is
ad-hoc signed too, so it has to be loaded by a host that *is* Apple signed;
``/usr/bin/perl`` is one. This module therefore never loads the framework into
this process — it spawns that host and reads the JSON stream it writes.

The reader is advisory in the same way the CoreAudio probe is: a missing payload,
a host that refuses to run it, or a stream that dies all degrade to "no mirror
state", and callers fall back to their own timing.
"""

from __future__ import annotations

import atexit
import json
import logging
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from quotaframe_bridge.paths import media_control_directory

LOGGER = logging.getLogger(__name__)

# `mediaremoted` withholds now-playing data from ad-hoc signed callers, so the
# adapter runs under an Apple-signed interpreter rather than in this process.
HOST_INTERPRETER = Path("/usr/bin/perl")
ADAPTER_SCRIPT = Path("lib/media-control/mediaremote-adapter.pl")
ADAPTER_FRAMEWORK = Path("Frameworks/MediaRemoteAdapter.framework")

# The stream pushes a line per change; the debounce only coalesces bursts.
DEBOUNCE_MILLISECONDS = 100
# A stream that exits is replaced, backing off so a broken host does not spin.
RESTART_DELAYS_SECONDS = (1.0, 5.0, 30.0)
# Without a live stream the last line is trusted this long, then ignored: the
# player may have quit while the reader was restarting.
SILENCE_TOLERANCE_SECONDS = 5.0
# A fresh stream writes its snapshot a few milliseconds after the spawn. The
# first reader waits for it rather than answer from stale timing.
FIRST_LINE_TIMEOUT_SECONDS = 0.15
# Reading the child's stdout must never block shutdown indefinitely.
SHUTDOWN_TIMEOUT_SECONDS = 2.0

_MICROS = 1_000_000.0


@dataclass(frozen=True)
class NowPlayingState:
    """One snapshot of the mirror, anchored at ``anchor_epoch``."""

    bundle_id: str
    process_id: int | None
    title: str
    artist: str
    album: str
    duration_seconds: float
    elapsed_seconds: float
    anchor_epoch: float
    playback_rate: float
    playing: bool
    received_at: float

    def position_seconds(self, now: float) -> float:
        """Playhead at ``now``, advanced from the anchor the player published.

        The client refreshes the anchor on pause, resume, seek and track change
        but not between them, so a frozen ``elapsed_seconds`` is the normal
        steady state and the distance from ``anchor_epoch`` carries the rest.
        """

        position = self.elapsed_seconds
        if self.playing and self.playback_rate > 0:
            position += max(0.0, now - self.anchor_epoch) * self.playback_rate
        if self.duration_seconds > 0:
            position = min(position, self.duration_seconds)
        return max(0.0, position)


def _seconds(payload: Mapping[str, object], micros_key: str, seconds_key: str) -> float:
    micros = payload.get(micros_key)
    if isinstance(micros, (int, float)):
        return float(micros) / _MICROS
    value = payload.get(seconds_key)
    return float(value) if isinstance(value, (int, float)) else 0.0


def _text(payload: Mapping[str, object], key: str) -> str:
    value = payload.get(key)
    return value if isinstance(value, str) else ""


def _anchor_epoch(payload: Mapping[str, object]) -> float:
    """The instant the player took its elapsed-time anchor, in epoch seconds."""

    micros = payload.get("timestampEpochMicros")
    if isinstance(micros, (int, float)):
        return float(micros) / _MICROS
    stamp = payload.get("timestamp")
    if isinstance(stamp, str):
        try:
            return datetime.fromisoformat(stamp.replace("Z", "+00:00")).timestamp()
        except ValueError:
            return 0.0
    return 0.0


def parse_payload(
    payload: Mapping[str, object], *, received_at: float
) -> NowPlayingState | None:
    """Build a state from one adapter payload; None when it names no track."""

    bundle_id = _text(payload, "bundleIdentifier")
    title = _text(payload, "title")
    if not bundle_id or not title:
        return None
    process_id = payload.get("processIdentifier")
    rate = payload.get("playbackRate")
    anchor = _anchor_epoch(payload)
    if anchor <= 0:
        LOGGER.debug("now-playing payload carries no usable anchor")
        return None
    return NowPlayingState(
        bundle_id=bundle_id,
        process_id=process_id if isinstance(process_id, int) else None,
        title=title,
        artist=_text(payload, "artist"),
        album=_text(payload, "album"),
        duration_seconds=_seconds(payload, "durationMicros", "duration"),
        elapsed_seconds=_seconds(payload, "elapsedTimeMicros", "elapsedTime"),
        anchor_epoch=anchor,
        playback_rate=float(rate) if isinstance(rate, (int, float)) else 0.0,
        playing=payload.get("playing") is True,
        received_at=received_at,
    )


def parse_stream_line(line: str, *, received_at: float) -> NowPlayingState | None:
    """Decode one line of the adapter's stream; unknown lines read as no state."""

    text = line.strip()
    if not text:
        return None
    try:
        document = json.loads(text)
    except ValueError:
        LOGGER.debug("now-playing stream line is not JSON")
        return None
    if not isinstance(document, dict) or document.get("type") != "data":
        return None
    if document.get("diff"):
        # Full payloads are requested, so a delta means the request was ignored.
        LOGGER.debug("now-playing stream produced an unexpected diff")
        return None
    payload = document.get("payload")
    if not isinstance(payload, dict):
        return None
    return parse_payload(payload, received_at=received_at)


def stream_command(directory: Path) -> list[str] | None:
    """Return the command that mirrors the system state, or None without it."""

    script = directory / ADAPTER_SCRIPT
    framework = directory / ADAPTER_FRAMEWORK
    if not script.is_file() or not framework.is_dir() or not HOST_INTERPRETER.is_file():
        return None
    return [
        str(HOST_INTERPRETER),
        str(script),
        str(framework),
        "stream",
        "--no-artwork",
        "--no-diff",
        "--micros",
        f"--debounce={DEBOUNCE_MILLISECONDS}",
    ]


class MediaRemoteStream:
    """A long-lived mirror of the system now-playing state.

    The child writes one JSON line per change, so the panel learns about a pause
    as soon as the daemon does instead of waiting for the audio output probe to
    notice the stream tearing down (8 s on a measured pause).
    """

    def __init__(
        self,
        *,
        directory: Path | None = None,
        clock: Callable[[], float] = time.time,
        spawn: Callable[..., object] | None = None,
        platform: str = sys.platform,
    ) -> None:
        self._directory = directory
        self._clock = clock
        self._spawn = spawn or subprocess.Popen
        self._platform = platform
        self._lock = threading.Lock()
        self._state: NowPlayingState | None = None
        self._process: object | None = None
        self._thread: threading.Thread | None = None
        self._next_attempt_at = 0.0
        self._attempt = 0
        self._stopped = False

    def state(self) -> NowPlayingState | None:
        """Return the latest mirror state, starting the stream on first use."""

        with self._lock:
            self._start_if_due()
            state = self._state
            running = self._process is not None
        if state is None:
            return self._await_first_line() if running else None
        silent_for = self._clock() - state.received_at
        if not running and silent_for > SILENCE_TOLERANCE_SECONDS:
            return None
        return state

    def _await_first_line(self) -> NowPlayingState | None:
        """Wait briefly for a freshly started stream to report one line."""

        deadline = time.monotonic() + FIRST_LINE_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            time.sleep(0.005)
            with self._lock:
                if self._state is not None:
                    return self._state
                if self._process is None:
                    return None
        return None

    def _start_if_due(self) -> None:
        if self._stopped or self._platform != "darwin" or self._process is not None:
            return
        now = self._clock()
        if now < self._next_attempt_at:
            return
        directory = self._directory or media_control_directory()
        command = stream_command(directory)
        if command is None:
            # The payload is missing from this installation; retrying cannot
            # help until it is there, so stop asking for a while.
            LOGGER.debug("media-control payload is incomplete at %s", directory)
            self._next_attempt_at = now + RESTART_DELAYS_SECONDS[-1]
            return
        try:
            process = self._spawn(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
        except OSError as exc:
            LOGGER.debug("now-playing stream did not start: %s", type(exc).__name__)
            self._schedule_restart(now)
            return
        self._process = process
        self._attempt = 0
        self._thread = threading.Thread(
            target=self._pump, args=(process,), name="now-playing-stream", daemon=True
        )
        self._thread.start()

    def _schedule_restart(self, now: float) -> None:
        delay = RESTART_DELAYS_SECONDS[
            min(self._attempt, len(RESTART_DELAYS_SECONDS) - 1)
        ]
        self._attempt += 1
        self._next_attempt_at = now + delay

    def _pump(self, process: object) -> None:
        """Read the child's lines until it exits."""

        stdout = getattr(process, "stdout", None)
        try:
            for line in stdout or ():
                state = parse_stream_line(line, received_at=self._clock())
                if state is None:
                    continue
                with self._lock:
                    self._state = state
        except (OSError, ValueError):
            LOGGER.debug("now-playing stream read failed", exc_info=True)
        finally:
            _close(process)
            with self._lock:
                self._process = None
                self._thread = None
                if not self._stopped:
                    self._schedule_restart(self._clock())

    def stop(self) -> None:
        """Stop the child and wait for the reader thread."""

        with self._lock:
            self._stopped = True
            process = self._process
            thread = self._thread
            self._process = None
            self._thread = None
        if process is not None:
            actions = (
                getattr(process, "terminate", None),
                getattr(process, "kill", None),
            )
            for action in actions:
                if action is None:
                    continue
                try:
                    action()
                    break
                except OSError:
                    continue
            _close(process)
        if thread is not None and thread.is_alive():
            thread.join(timeout=SHUTDOWN_TIMEOUT_SECONDS)


def _close(process: object) -> None:
    stdout = getattr(process, "stdout", None)
    for stream in (stdout, getattr(process, "stderr", None)):
        try:
            stream.close()  # type: ignore[union-attr]
        except (AttributeError, OSError):
            continue
    wait = getattr(process, "wait", None)
    if wait is not None:
        try:
            wait(timeout=SHUTDOWN_TIMEOUT_SECONDS)
        except Exception:  # noqa: BLE001 - the child is gone either way
            pass


_default: MediaRemoteStream | None = None
_default_lock = threading.Lock()


def current_state() -> NowPlayingState | None:
    """Mirror state from the process-wide stream, or None when unavailable."""

    global _default
    with _default_lock:
        stream = _default
        if stream is None:
            stream = MediaRemoteStream()
            _default = stream
    return stream.state()


def _reset_default() -> None:
    """Drop the process-wide stream so the next call starts a fresh one."""

    global _default
    with _default_lock:
        stream, _default = _default, None
    if stream is not None:
        stream.stop()


# The child outlives this process unless it is asked to stop, and the Bridge
# exits without tearing sources down.
atexit.register(_reset_default)


__all__ = (
    "HOST_INTERPRETER",
    "MediaRemoteStream",
    "NowPlayingState",
    "current_state",
    "parse_payload",
    "parse_stream_line",
    "stream_command",
)
