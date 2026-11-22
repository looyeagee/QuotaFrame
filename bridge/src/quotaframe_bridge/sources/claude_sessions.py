"""Claude Code session state, collected from this computer's Claude data.

The store in `control.cc_sessions` reads `~/.claude`; this adapter reduces its
running sessions to the display pairs a panel's sessions page draws, and watches
the directory Claude publishes them in, so a change reaches the panel while it
happens instead of on a timer. Names and states travel to the panel; session
ids, working directories, timestamps and transcript contents stay on the
computer.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from quotaframe_bridge.control.cc_sessions import ClaudeSessionStore
from quotaframe_bridge.domain.models import (
    LiveSession,
    SessionState,
    SessionsPublication,
    WaitingReason,
)

# The publication bounds are the protocol's; the adapter cuts before encoding.
from quotaframe_bridge.protocol.messages import MAX_LIVE_SESSIONS

try:
    from watchdog.events import FileSystemEventHandler
    from watchdog.observers import Observer
except ImportError:  # A build without the watch wheel still publishes, on a timer.
    FileSystemEventHandler = None  # type: ignore[assignment,misc]
    Observer = None  # type: ignore[assignment,misc]

# One status change reaches the filesystem as several writes: Claude rewrites the
# file it publishes, and the directory is modified along with it. Letting the
# directory settle keeps one transition from becoming several frames.
SETTLE_SECONDS = 0.15

# Without an observer the directory is read on this cadence, so a build that
# shipped without the watch wheel still follows the sessions instead of freezing
# at the first snapshot.
FALLBACK_POLL_SECONDS = 2.0

# The state Claude Code publishes for a running session. A session that waits
# for its user is the one that spends the reader's time rather than its own, so
# it keeps a state of its own instead of joining the working ones.
SESSION_STATES = {
    "idle": SessionState.IDLE,
    "busy": SessionState.BUSY,
    "waiting": SessionState.WAITING,
    "shell": SessionState.SHELL,
}

# What a waiting session waits for, in Claude Code's words. A reason it does
# not name yet is still a dialog that holds the session, so it reads as one.
WAITING_REASONS = {
    "input needed": WaitingReason.ASK,
    "permission prompt": WaitingReason.PERMISSION,
    "sandbox request": WaitingReason.SANDBOX,
    "worker request": WaitingReason.WORKER,
    "goal proposal": WaitingReason.GOAL,
}


if FileSystemEventHandler is not None:

    class _SessionFileEvents(FileSystemEventHandler):  # type: ignore[misc]
        """Marks the waiting coroutine whenever a session file changes."""

        def __init__(self, loop: asyncio.AbstractEventLoop, signal: asyncio.Event) -> None:
            self._loop = loop
            self._signal = signal

        def on_any_event(self, change: object) -> None:
            # A directory event reports the directory itself, not a session state.
            if getattr(change, "is_directory", False):
                return
            try:
                self._loop.call_soon_threadsafe(self._signal.set)
            except RuntimeError:
                pass  # the loop closed while the observer was still running


class ClaudeSessionsSource:
    """Reads Claude Code's running sessions, and reports when they change."""

    def __init__(
        self,
        store: ClaudeSessionStore | None = None,
        *,
        directory: Path | None = None,
    ) -> None:
        self._store = store if store is not None else ClaudeSessionStore()
        self._directory = directory if directory is not None else self._store.sessions_dir
        self._changes: asyncio.Event | None = None
        self._observer: object | None = None

    async def collect(self) -> SessionsPublication:
        # Reading the directory stats every state file, so it runs off the loop
        # that drives the devices.
        live = await asyncio.to_thread(self._store.live_snapshot)
        sessions = [
            LiveSession(
                name=entry["name"] or entry["project"] or entry["session_id"][:8],
                state=_state(entry["status"]),
                waiting_for=_waiting_reason(entry["status"], entry["waiting_for"]),
            )
            for entry in live
            if entry["alive"]
        ]
        return SessionsPublication(live=tuple(sessions[:MAX_LIVE_SESSIONS]))

    async def wait_for_change(self, timeout: float) -> None:
        """Return once the running sessions may have changed, or after `timeout`.

        `timeout` is the longest the caller stays blind, not the cadence: a
        session that ends without deleting its own state file leaves no event
        behind, and this is what still notices it.
        """

        signal = self._watch()
        if signal is None:
            await asyncio.sleep(min(timeout, FALLBACK_POLL_SECONDS))
            return
        try:
            await asyncio.wait_for(signal.wait(), timeout)
        except TimeoutError:
            pass
        signal.clear()
        await asyncio.sleep(SETTLE_SECONDS)

    def close(self) -> None:
        """Stop the directory observer; the bridge calls this when it stops."""

        observer, self._observer = self._observer, None
        if observer is not None:
            observer.stop()  # type: ignore[attr-defined]
            observer.join(timeout=1.0)  # type: ignore[attr-defined]
        self._changes = None

    def _watch(self) -> asyncio.Event | None:
        """The change signal, starting the observer on first use."""

        if self._changes is not None:
            return self._changes
        if Observer is None:
            return None
        # A machine that never ran Claude Code has no directory to watch yet;
        # the caller's timeout covers it, and a later call picks the directory up.
        if not self._directory.is_dir():
            return None
        signal = asyncio.Event()
        observer = Observer()  # type: ignore[operator]
        observer.schedule(
            _SessionFileEvents(asyncio.get_running_loop(), signal), str(self._directory)
        )
        observer.daemon = True
        observer.start()
        self._observer = observer
        self._changes = signal
        return signal


def _state(status: str) -> SessionState:
    """The state of a running session; a word Claude Code has not used is idle."""

    return SESSION_STATES.get(status, SessionState.IDLE)


def _waiting_reason(status: str, waiting_for: str) -> WaitingReason | None:
    """What a waiting session waits for; only a waiting session has one."""

    if status != "waiting":
        return None
    return WAITING_REASONS.get(waiting_for, WaitingReason.DIALOG)
