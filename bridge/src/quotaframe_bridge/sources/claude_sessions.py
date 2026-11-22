"""Claude Code session state, collected from this computer's Claude data.

The store in `control.cc_sessions` reads `~/.claude`; this adapter reduces its
running sessions to the display pairs a panel's sessions page draws, and watches
the directory Claude publishes them in, so a change reaches the panel while it
happens instead of on a timer. Names and busy states travel to the panel; session
ids, working directories, timestamps and transcript contents stay on the computer.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from quotaframe_bridge.control.cc_sessions import ClaudeSessionStore
from quotaframe_bridge.domain.models import SessionsPublication

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
        pairs = tuple(
            (
                entry["name"] or entry["project"] or entry["session_id"][:8],
                entry["status"] == "busy",
            )
            for entry in live
            if entry["alive"]
        )
        return SessionsPublication(live=pairs[:MAX_LIVE_SESSIONS])

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
