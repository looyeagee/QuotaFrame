"""Independent device sessions over one shared usage snapshot stream."""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass
from datetime import datetime
from typing import TypeVar

from quotaframe_bridge.domain.models import (
    NowPlayingPublication,
    Provider,
    ProviderUsage,
    SessionsPublication,
    SourceState,
)
from quotaframe_bridge.protocol.messages import (
    ALERT_SET_CAPABILITY,
    BRIGHTNESS_SET_CAPABILITY,
    MAX_INVERSE_CLOCK_SKEW_SECONDS,
    NOW_PLAYING_CAPABILITY,
    SESSIONS_CAPABILITY,
    SESSIONS_STATES_CAPABILITY,
    Sequence,
    encode_alert_set,
    encode_brightness_set,
    encode_now_playing,
    encode_screen_toggle,
    encode_screen_page,
    encode_sessions,
    encode_usage,
)
from quotaframe_bridge.service.device_manager import ReconnectBackoff
from quotaframe_bridge.service.state import UsageStateStore
from quotaframe_bridge.service.time_sync import TimeSyncScheduler
from quotaframe_bridge.sources.base import (
    NowPlayingSource,
    ProgressiveUsageSource,
    SessionsSource,
    UsageSource,
)
from quotaframe_bridge.transports.base import (
    TransportCleanupError,
    TransportReset,
    UsageTransport,
    device_status,
)

LOGGER = logging.getLogger(__name__)
SCREEN_TOGGLE_CAPABILITY = "screen.toggle.v1"
SCREEN_PAGE_CAPABILITY = "screen.page.v1"
_SESSION_RESTART_INITIAL_DELAY = 1.0
_SESSION_RESTART_MAX_DELAY = 60.0
# How often the music publisher looks at the player while it waits for its
# cadence: a seek should reach the panel in a fraction of a tick, not after one.
MUSIC_WATCH_SECONDS = 0.1
T = TypeVar("T")


async def _cancel(task: asyncio.Task[None] | None) -> None:
    """Stop a session-owned background task and wait for it to unwind."""

    if task is not None and not task.done():
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


@dataclass(frozen=True, slots=True)
class UsageSnapshot:
    """One immutable publication generation.

    A provider is None when the active source does not manage it at all, which
    keeps an unused provider off the wire instead of publishing a permanent
    "unavailable" it can never clear.
    """

    deepseek: ProviderUsage | None = None
    minimax: ProviderUsage | None = None

    def publications(self) -> tuple[ProviderUsage, ...]:
        return tuple(
            usage for usage in (self.deepseek, self.minimax) if usage is not None
        )


class DeviceSession:
    """Own one transport lifecycle, sequence, and latest-value mailbox."""

    def __init__(
        self,
        label: str,
        transport: UsageTransport,
        *,
        wall_now: Callable[[], datetime] = lambda: datetime.now().astimezone(),
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        self.label = label
        self.transport = transport
        self._sequence = Sequence()
        self._wake = asyncio.Event()
        self._pending: UsageSnapshot | None = None
        self._offered_revision = 0
        self._delivered_revision = 0
        self._delivery_changed = asyncio.Condition()
        self._connected = False
        self._runner: asyncio.Task[None] | None = None
        self._connect_task: asyncio.Task[None] | None = None
        self._exclusive_lock = asyncio.Lock()
        self._exclusive_active = False
        self._exclusive_task: asyncio.Task | None = None
        self._closing = False
        self._wall_now = wall_now
        self._time_sync = TimeSyncScheduler(wall_now, monotonic)
        self._time_sync_task: asyncio.Task[None] | None = None
        self._screen_command_task: asyncio.Task[None] | None = None
        self._alert_command_task: asyncio.Task[None] | None = None
        self._brightness_command_task: asyncio.Task[None] | None = None
        # Last alert mode this session wrote or read; cleared on every reconnect
        # so the panel's own status stays authoritative across a restart.
        self._alert_mode: int | None = None
        # Last brightness percent this session wrote or read; cleared like the alert mode.
        self._brightness: int | None = None
        # Last session roster sent, so an identical one is not put on the link again.
        self._sessions_sent: SessionsPublication | None = None
        # Set by the service: a link that just came up needs the roster at once.
        self.on_link_ready: Callable[[], None] | None = None
        self._force_time_sync = True

    @property
    def connected(self) -> bool:
        return self._connected and getattr(self.transport, "connected", True)

    @property
    def exclusive_active(self) -> bool:
        return self._exclusive_active

    @property
    def screen_command_pending(self) -> bool:
        """True while a screen command is in flight and would swallow a new one."""

        return self._screen_command_task is not None and not self._screen_command_task.done()

    @property
    def alert_command_pending(self) -> bool:
        """True while an alert mode change is in flight."""

        return self._alert_command_task is not None and not self._alert_command_task.done()

    @property
    def brightness_command_pending(self) -> bool:
        """True while a brightness change is in flight."""

        return (
            self._brightness_command_task is not None
            and not self._brightness_command_task.done()
        )

    @property
    def alert_mode(self) -> int | None:
        """Alert mode the panel holds: our last acknowledged write, else its status."""

        if self._alert_mode is not None:
            return self._alert_mode
        status = device_status(self.transport)
        return None if status is None else status.alert

    @property
    def brightness(self) -> int | None:
        """Brightness percent the panel holds: our last acknowledged write, else its status."""

        if self._brightness is not None:
            return self._brightness
        status = device_status(self.transport)
        return None if status is None else status.brightness

    def screen_command_unavailable(self, capability: str) -> str | None:
        """Return why a screen command would be dropped, or None when it would run.

        Both the scheduler and the sender re-check these conditions before
        touching the transport, and both drop the command silently. Callers that
        report an outcome to a person need the same predicate in advance.
        """

        if self._closing:
            return "closing"
        if self.screen_command_pending:
            return "busy"
        if not self.connected:
            return "disconnected"
        if self._exclusive_active:
            return "exclusive"
        status = device_status(self.transport)
        if status is None:
            return "disconnected"
        if not status.secure:
            return "unsecured"
        if capability not in status.capabilities:
            return "missing_capability"
        return None

    def schedule_screen_toggle(self) -> None:
        self._schedule_screen_command(None)

    def schedule_screen_page(self, direction: int) -> None:
        # A bool would satisfy `in (-1, 1)` because True equals 1, and then the
        # sign test below would silently reverse the requested direction.
        if type(direction) is not int or direction not in (-1, 1):
            raise ValueError("page direction must be -1 or 1")
        self._schedule_screen_command(direction < 0)

    def alert_command_unavailable(self) -> str | None:
        """Return why an alert mode change would be dropped, or None.

        The mode is a persisted write, so the panel has to report the capability
        and hold a usable link before sending it is worth anything.
        """

        if self.alert_command_pending:
            return "busy"
        return self.screen_command_unavailable(ALERT_SET_CAPABILITY)

    def schedule_alert_set(self, mode: int) -> None:
        if type(mode) is not int or mode not in (0, 1, 2):
            raise ValueError("alert mode must be 0, 1 or 2")
        if self.alert_command_unavailable() is not None:
            return
        self._alert_command_task = asyncio.create_task(self._send_alert_set(mode))

    def brightness_command_unavailable(self) -> str | None:
        """Return why a brightness change would be dropped, or None.

        The percent is a persisted write, so the panel has to report the
        capability and hold a usable link before sending it is worth anything.
        """

        if self.brightness_command_pending:
            return "busy"
        return self.screen_command_unavailable(BRIGHTNESS_SET_CAPABILITY)

    def schedule_brightness_set(self, percent: int) -> None:
        if type(percent) is not int or not 0 <= percent <= 100:
            raise ValueError("brightness percent must be an int from 0 to 100")
        if self.brightness_command_unavailable() is not None:
            return
        self._brightness_command_task = asyncio.create_task(self._send_brightness_set(percent))

    async def _send_brightness_set(self, percent: int) -> None:
        # An absolute setting is idempotent, but an unknown outcome is still not
        # replayed: the next state poll shows which percent the panel kept.
        status = device_status(self.transport)
        if (not self.connected or self.exclusive_active or status is None
                or not status.secure
                or BRIGHTNESS_SET_CAPABILITY not in status.capabilities):
            return
        try:
            async with self._exclusive_lock:
                if (not self.connected or self.exclusive_active
                        or device_status(self.transport) is not status):
                    return
                sequence = self._sequence.next()
                await self.transport.send_command(
                    encode_brightness_set(sequence, percent), "brightness_set", sequence)
                # The ACK is the only fresh value until the next status query.
                self._brightness = percent
        except TransportReset:
            LOGGER.warning("%s brightness_set outcome unknown; not replayed", self.label)
        except Exception as exc:
            LOGGER.warning("%s brightness_set failed: %s", self.label, type(exc).__name__)

    async def _send_alert_set(self, mode: int) -> None:
        # An absolute setting is idempotent, but an unknown outcome is still not
        # replayed: the next state poll shows which mode the panel kept.
        status = device_status(self.transport)
        if (not self.connected or self.exclusive_active or status is None
                or not status.secure
                or ALERT_SET_CAPABILITY not in status.capabilities):
            return
        try:
            async with self._exclusive_lock:
                if (not self.connected or self.exclusive_active
                        or device_status(self.transport) is not status):
                    return
                sequence = self._sequence.next()
                await self.transport.send_command(
                    encode_alert_set(sequence, mode), "alert_set", sequence)
                # The ACK is the only fresh value until the next status query.
                self._alert_mode = mode
        except TransportReset:
            LOGGER.warning("%s alert_set outcome unknown; not replayed", self.label)
        except Exception as exc:
            LOGGER.warning("%s alert_set failed: %s", self.label, type(exc).__name__)

    def _schedule_screen_command(self, previous: bool | None) -> None:
        # Relative commands cannot be replayed safely after an unknown outcome.
        capability = SCREEN_TOGGLE_CAPABILITY if previous is None else SCREEN_PAGE_CAPABILITY
        if self.screen_command_unavailable(capability) is not None:
            return
        self._screen_command_task = asyncio.create_task(self._send_screen_command(previous))

    async def _send_screen_command(self, previous: bool | None) -> None:
        capability = SCREEN_TOGGLE_CAPABILITY if previous is None else SCREEN_PAGE_CAPABILITY
        command = "screen_toggle" if previous is None else "screen_page"
        status = device_status(self.transport)
        if (not self.connected or self.exclusive_active or status is None
                or not status.secure or capability not in status.capabilities):
            return
        try:
            async with self._exclusive_lock:
                if (not self.connected or self.exclusive_active
                        or device_status(self.transport) is not status):
                    return
                sequence = self._sequence.next()
                payload = (encode_screen_toggle(sequence) if previous is None
                           else encode_screen_page(sequence, previous))
                await self.transport.send_command(payload, command, sequence)
                await self._refresh_status()
        except TransportReset:
            self._force_time_sync = True
            LOGGER.warning("%s %s outcome unknown; not replayed", self.label, command)
        except Exception as exc:
            LOGGER.warning("%s %s failed: %s", self.label, command, type(exc).__name__)

    async def _refresh_status(self) -> None:
        """Re-read the panel's status after a command that moves the panel.

        The status a session holds is the one read at handshake, so without this
        the page the control page reports keeps showing the page the panel was on
        when it connected. A failed refresh is not a failed command: the panel
        already acted, so it must not surface as one.
        """

        query = getattr(self.transport, "query_status", None)
        if query is None:
            return
        try:
            await query()
        except Exception as exc:  # noqa: BLE001 - a stale label beats a false failure
            LOGGER.debug(
                "%s status refresh after a screen command failed: %s",
                self.label, type(exc).__name__,
            )

    async def _cancel_screen_command(self) -> None:
        await _cancel(self._screen_command_task)
        self._screen_command_task = None

    async def _cancel_alert_command(self) -> None:
        await _cancel(self._alert_command_task)
        self._alert_command_task = None

    async def run_exclusive(
        self,
        operation: Callable[[UsageTransport], Awaitable[T]],
        *,
        interrupt_connect: bool = False,
    ) -> T:
        """Run one transport operation without usage publication or time sync.

        In-flight commands finish under their transport timeout before handoff.
        Recovery operations may interrupt negotiation before acquiring the lock.
        An existing exclusive operation is always rejected.
        """

        if self._closing or self._exclusive_active:
            raise RuntimeError("device operation is already active or device is closing")
        self._exclusive_task = asyncio.current_task()
        self._exclusive_active = True
        try:
            if interrupt_connect:
                await self._cancel_time_sync()
                await self._cancel_screen_command()
                await self._cancel_alert_command()
                if self._connect_task is not None:
                    self._connect_task.cancel()
            async with self._exclusive_lock:
                return await operation(self.transport)
        except asyncio.CancelledError:
            cleanup_error = getattr(self.transport, "cleanup_error", None)
            if cleanup_error is not None:
                raise cleanup_error
            raise
        finally:
            self._exclusive_active = False
            self._exclusive_task = None

    async def _cancel_exclusive(self) -> None:
        if self._exclusive_task is not asyncio.current_task():
            await _cancel(self._exclusive_task)

    def offer(self, snapshot: UsageSnapshot) -> int:
        """Replace the pending snapshot and return its monotonically increasing revision."""

        self._pending = snapshot
        self._offered_revision += 1
        self._wake.set()
        return self._offered_revision

    async def publish_now_playing(self, publication: NowPlayingPublication) -> bool:
        """Send one now-playing publication to a device that renders it.

        Devices without the capability are left alone, and a link that drops
        while the command is in flight is reported as a failed publication
        instead of ending the session.
        """

        status = device_status(self.transport)
        if (
            not self.connected
            or self.exclusive_active
            or status is None
            or not status.secure
            or NOW_PLAYING_CAPABILITY not in status.capabilities
        ):
            return False
        try:
            async with self._exclusive_lock:
                if not self.connected or self.exclusive_active:
                    return False
                sequence = self._sequence.next()
                payload = encode_now_playing(publication, sequence)
                await self.transport.send_command(payload, "now_playing", sequence)
        except TransportReset:
            LOGGER.warning("%s now_playing outcome unknown; not replayed", self.label)
            return False
        except Exception as exc:
            LOGGER.warning(
                "%s now_playing failed: %s", self.label, type(exc).__name__
            )
            return False
        LOGGER.debug("%s now_playing sent: %s", self.label, publication.title)
        return True

    async def wait_delivered(self, revision: int) -> None:
        """Wait until this session has delivered at least the requested revision."""

        async with self._delivery_changed:
            await self._delivery_changed.wait_for(
                lambda: self._delivered_revision >= revision
            )

    async def publish_sessions(self, publication: SessionsPublication) -> bool:
        """Send one sessions publication to a device that renders it.

        The same snapshot contract as now_playing: devices without the
        capability are left alone, and a link that drops while the command is
        in flight is reported as a failed publication instead of ending the
        session. A roster identical to the last one sent stays off the link,
        because a redraw it cannot show is only wear on the glass.
        """

        status = device_status(self.transport)
        if (
            not self.connected
            or self.exclusive_active
            or status is None
            or not status.secure
            or SESSIONS_CAPABILITY not in status.capabilities
        ):
            # A link that went away has to be handed the roster again when it
            # comes back, whether or not the sessions changed meanwhile.
            self._sessions_sent = None
            return False
        if publication == self._sessions_sent:
            return False
        try:
            async with self._exclusive_lock:
                if not self.connected or self.exclusive_active:
                    self._sessions_sent = None
                    return False
                sequence = self._sequence.next()
                # A device that declares the extended states is told when a
                # session waits for its user; one that does not gets the two
                # states it knows.
                payload = encode_sessions(
                    publication,
                    sequence,
                    extended=SESSIONS_STATES_CAPABILITY in status.capabilities,
                )
                await self.transport.send_command(payload, "sessions", sequence)
        except TransportReset:
            self._sessions_sent = None
            LOGGER.warning("%s sessions outcome unknown; not replayed", self.label)
            return False
        except Exception as exc:
            self._sessions_sent = None
            LOGGER.warning("%s sessions failed: %s", self.label, type(exc).__name__)
            return False
        self._sessions_sent = publication
        LOGGER.debug("%s sessions sent: %d live", self.label, len(publication.live))
        return True

    def _link_ready(self) -> None:
        """Report a link that can carry a first snapshot of the panels."""

        # A link that just came up has no idea what the panel shows, and a board that
        # rebooted starts on a page it has no roster for, so the roster goes out again even
        # when it has not changed since the last link.
        self._sessions_sent = None
        callback = self.on_link_ready
        if callback is not None:
            callback()


    async def _connect(self) -> bool:
        """Allow lifecycle operations to interrupt negotiation without stopping publication."""
        task = asyncio.create_task(self.transport.connect())
        self._connect_task = task
        try:
            await task
            # A reconnect re-reads status, which becomes the alert mode and the
            # brightness again.
            self._alert_mode = None
            self._brightness = None
            return True
        except asyncio.CancelledError:
            if asyncio.current_task().cancelling():
                raise
            return False
        finally:
            self._connect_task = None

    async def run(self) -> None:
        """Monitor the idle link, reconnect when needed, and publish the latest snapshot."""

        current = asyncio.current_task()
        if current is None:
            raise RuntimeError("device session requires an asyncio task")
        if self._runner is not None and self._runner is not current:
            raise RuntimeError("device session is already running")
        self._runner = current
        self._closing = False
        try:
            while True:
                async with self._exclusive_lock:
                    if await self._connect():
                        break
            self._connected = True
            self._link_ready()
            while True:
                try:
                    await asyncio.wait_for(self._wake.wait(), timeout=1.0)
                except TimeoutError:
                    if self.connected or self._exclusive_active:
                        continue
                    await self._cancel_time_sync()
                    async with self._exclusive_lock:
                        if not await self._connect():
                            continue
                    self._force_time_sync = True
                    self._link_ready()
                self._wake.clear()
                snapshot = self._pending
                revision = self._offered_revision
                if snapshot is None:
                    continue
                try:
                    revision = await self._publish(snapshot)
                except TransportReset:
                    self._force_time_sync = True
                    LOGGER.warning(
                        "%s BLE delivery reset; retrying latest snapshot",
                        self.label,
                    )
                    self._wake.set()
                    await asyncio.sleep(0)
                    continue
                async with self._delivery_changed:
                    self._delivered_revision = max(
                        self._delivered_revision,
                        revision,
                    )
                    self._delivery_changed.notify_all()
                self._schedule_time_sync()
        finally:
            self._closing = True
            self._connected = False
            await self._cancel_exclusive()
            await self._cancel_screen_command()
            await self._cancel_alert_command()
            await self._cancel_time_sync()
            await self.transport.close()
            if self._pending is not None:
                self._wake.set()

    async def close(self) -> None:
        """Cancel the session runner when owned elsewhere, then close its transport."""

        self._closing = True
        self._connected = False
        await self._cancel_exclusive()
        await self._cancel_screen_command()
        await self._cancel_alert_command()
        runner = self._runner
        current = asyncio.current_task()
        if runner is not None and runner is not current and not runner.done():
            runner.cancel()
            await asyncio.gather(runner, return_exceptions=True)
            return
        if runner is None:
            await self.transport.close()

    async def _publish(self, snapshot: UsageSnapshot) -> int:
        async with self._exclusive_lock:
            if not getattr(self.transport, "connected", True):
                if not await self._connect():
                    raise TransportReset("connection interrupted for device recovery")
                self._force_time_sync = True
                # Delivering a snapshot is how a link usually comes back: the loop wakes
                # holding a snapshot and reconnects here, without ever sitting idle for the
                # second that runs the reconnect branch above. The board on the other end may
                # have rebooted while the link was down and lost its roster, so the link is
                # declared ready here as well; without this the sessions page stays dark on a
                # link that returned with a usage frame, until the roster itself changes.
                self._link_ready()
            snapshot = self._pending or snapshot
            revision = self._offered_revision
            level = logging.INFO if self._delivered_revision == 0 else logging.DEBUG
            for usage in snapshot.publications():
                sequence = self._sequence.next()
                sent_at = int(self._wall_now().timestamp())
                if sent_at + MAX_INVERSE_CLOCK_SKEW_SECONDS < usage.sampled_at:
                    # A clock rollback can make the cached timestamp invalid on
                    # the wire. Unavailable preserves the device's cached sample.
                    usage = ProviderUsage(usage.provider, SourceState.UNAVAILABLE, sent_at)
                payload = encode_usage(usage, sequence, sent_at)
                await self.transport.send_command(payload, "usage", sequence)
                LOGGER.log(
                    level, "%s usage ACK: provider=%s state=%s",
                    self.label, usage.provider.value, usage.state.value,
                )
            return revision

    def _schedule_time_sync(self) -> None:
        if self._time_sync_task is not None and not self._time_sync_task.done():
            return
        self._time_sync_task = asyncio.create_task(self._sync_time())

    async def _cancel_time_sync(self) -> None:
        await _cancel(self._time_sync_task)
        self._time_sync_task = None

    async def _sync_time(self) -> None:
        if not self._time_sync.is_supported(device_status(self.transport)):
            self._force_time_sync = False
            return
        try:
            async with self._exclusive_lock:
                if self._closing or self._exclusive_active:
                    return
                delivered = await self._time_sync.sync_if_due(
                    self.transport,
                    self._sequence,
                    force=self._force_time_sync,
                )
                if delivered:
                    self._force_time_sync = False
        except TransportReset:
            self._force_time_sync = True
            LOGGER.debug("%s time sync reset; will retry", self.label)
        except Exception:
            LOGGER.debug("%s time sync not delivered", self.label)


class MultiDeviceBridgeService:
    """Collect once and offer the latest state to independent device sessions."""

    def __init__(
        self,
        source: UsageSource,
        sessions: tuple[DeviceSession, ...],
        *,
        interval: float = 60.0,
        publish_interval: float = 30.0,
        clock: Callable[[], int] = lambda: int(time.time()),
        monotonic: Callable[[], float] = time.monotonic,
        store: UsageStateStore | None = None,
        music_source: NowPlayingSource | None = None,
        music_interval: float = 1.0,
        sessions_source: SessionsSource | None = None,
        sessions_interval: float = 30.0,
    ) -> None:
        # Zero sessions is a legitimate steady state, not a misconfiguration:
        # a user who has not adopted a board yet still gets usage collection
        # and tray percentages, and must not get a background scan for boards
        # nobody owns.
        if interval < 0 or publish_interval <= 0:
            raise ValueError(
                "interval must be non-negative and publish interval positive"
            )
        if music_interval <= 0:
            raise ValueError("music interval must be positive")
        if sessions_interval <= 0:
            raise ValueError("sessions interval must be positive")
        self.source = source
        self.sessions = sessions
        self.interval = interval
        self.publish_interval = publish_interval
        self.clock = clock
        self.monotonic = monotonic
        self.store = UsageStateStore() if store is None else store
        self.music_source = music_source
        self.music_interval = music_interval
        self.sessions_source = sessions_source
        # The longest the sessions loop stays blind, not its cadence: the source
        # wakes it on a change, and this only covers the ones no file reports.
        self.sessions_interval = sessions_interval
        declared = getattr(source, "providers", None)
        if declared is not None:
            self.store.restrict_to(declared)
        self._first_collection = True
        self._collect_now = asyncio.Event()
        # Set when a link comes up, so a board that just booted gets the session
        # roster at once instead of at the next change.
        self._sessions_now = asyncio.Event()
        self._supervisors: dict[DeviceSession, asyncio.Task[None]] = {}
        self._running = False
        self.collection_count = 0
        self.consecutive_failures = 0
        # Last accepted now-playing publication, kept for status surfaces that
        # are not the panels themselves. None until the first successful fetch.
        self.now_playing: NowPlayingPublication | None = None

    def toggle_screensavers(self) -> None:
        """Apply one tray click independently to each eligible device."""

        for session in self.sessions:
            session.schedule_screen_toggle()

    def turn_pages(self, direction: int) -> None:
        """Apply one wheel step to every connected, capable device."""
        for session in self.sessions:
            session.schedule_screen_page(direction)

    def set_alert(self, mode: int) -> None:
        """Apply one alert mode to every capable panel; each panel persists it."""
        if type(mode) is not int or mode not in (0, 1, 2):
            raise ValueError("alert mode must be 0, 1 or 2")
        for session in self.sessions:
            session.schedule_alert_set(mode)

    def request_collection(self) -> None:
        """Ask the live loop to collect now instead of after the interval."""

        self._collect_now.set()

    async def collect(self) -> None:
        """Collect once, retaining last-valid values separately from publish state."""

        attempted_at = self.clock()
        # Collection is the one step in the loop with no visible progress: it is
        # two HTTP requests, so only the first collection is announced at INFO —
        # a steady-state loop logging every minute is noise in a rotating log.
        level = logging.INFO if self._first_collection else logging.DEBUG
        LOGGER.log(level, "reading usage from %s", getattr(self.source, "label", "the usage source"))
        started_at = self.monotonic()

        def publish_provider(usage: ProviderUsage) -> None:
            self.store.merge_collection(
                {usage.provider: usage}, attempted_at=attempted_at, partial=True,
            )
            self.consecutive_failures = 0
            self._offer_current()

        try:
            if isinstance(self.source, ProgressiveUsageSource):
                collection = await self.source.collect_progressively(
                    attempted_at, on_update=publish_provider,
                )
            else:
                collection = await self.source.collect(attempted_at=attempted_at)
            self.store.merge_collection(
                collection,
                attempted_at=attempted_at,
            )
            self.consecutive_failures = 0
        except Exception as exc:
            LOGGER.warning(
                "usage collection unavailable: %s",
                type(exc).__name__,
            )
            self.consecutive_failures += 1
            self.store.mark_all_unavailable(attempted_at=attempted_at)
        finally:
            LOGGER.log(
                level,
                "usage collection finished in %.1fs",
                self.monotonic() - started_at,
            )
            self._first_collection = False
        self.collection_count += 1

    async def run(self, cycles: int | None = None) -> None:
        """Run all device sessions with either bounded or continuous collection."""

        if cycles is not None and cycles < 1:
            raise ValueError("cycles must be at least one")
        self._running = True
        for session in self.sessions:
            session.on_link_ready = self._sessions_now.set
            self._start_supervisor(session)
        try:
            if cycles is None:
                await self._run_live()
                return
            for completed in range(cycles):
                await self.collect()
                offers = self._offer_current()
                await self._wait_for_available_deliveries(offers)
                if completed + 1 < cycles:
                    await asyncio.sleep(self.interval)
        finally:
            self._running = False
            await asyncio.gather(
                *(session.close() for session in self.sessions),
                return_exceptions=True,
            )
            if self.sessions_source is not None:
                self.sessions_source.close()
            tasks = tuple(self._supervisors.values())
            self._supervisors.clear()
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

    def _start_supervisor(self, session: DeviceSession) -> None:
        if session in self._supervisors:
            return
        self._supervisors[session] = asyncio.create_task(
            self._supervise_session(session)
        )

    def add_session(self, session: DeviceSession) -> None:
        """Attach a session to a service that may already be running.

        Adoption happens while the tray is live, so a newly owned board has to
        join the running graph rather than wait for a restart.
        """

        if session in self.sessions:
            return
        self.sessions = self.sessions + (session,)
        providers = self.store.providers
        if providers:
            session.offer(self._snapshot(providers))
        if self._running:
            # A board adopted while the bridge runs joins a loop that is already waiting for
            # a change, and its own first connection is not one of them.
            session.on_link_ready = self._sessions_now.set
            self._start_supervisor(session)

    async def remove_session(self, session: DeviceSession) -> None:
        """Detach and close one session, stopping its supervised restarts.

        The supervisor is cancelled before the session is closed: cancelling
        second would let it observe the close as an unexpected stop and
        schedule a reconnect for a board the user just removed.
        """

        self.sessions = tuple(
            existing for existing in self.sessions if existing is not session
        )
        task = self._supervisors.pop(session, None)
        if task is not None and not task.done():
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        await session.close()

    async def _supervise_session(self, session: DeviceSession) -> None:
        backoff = ReconnectBackoff(
            initial=_SESSION_RESTART_INITIAL_DELAY,
            maximum=_SESSION_RESTART_MAX_DELAY,
        )
        while True:
            try:
                await session.run()
            except asyncio.CancelledError:
                raise
            except TransportCleanupError as exc:
                LOGGER.error("%s: %s", session.label, exc)
                return
            except Exception:
                restart_delay = backoff.next_delay()
                LOGGER.exception(
                    "%s session stopped unexpectedly; restarting in %.1fs",
                    session.label,
                    restart_delay,
                )
            else:
                restart_delay = backoff.next_delay()
                LOGGER.error(
                    "%s session stopped unexpectedly; restarting in %.1fs",
                    session.label,
                    restart_delay,
                )
            await asyncio.sleep(restart_delay)

    async def _wait_for_available_deliveries(
        self,
        offers: tuple[tuple[DeviceSession, int], ...],
    ) -> None:
        # Sessions are paired with their own revision at offer time rather
        # than re-zipped against self.sessions here, because adoption and
        # removal can change the session set between the two points.
        if not offers:
            return
        waiters = tuple(
            (session, asyncio.create_task(session.wait_delivered(revision)))
            for session, revision in offers
        )
        try:
            await asyncio.wait(
                (waiter for _, waiter in waiters),
                return_when=asyncio.FIRST_COMPLETED,
            )
            connected_waiters = tuple(
                waiter
                for session, waiter in waiters
                if session.connected and not waiter.done()
            )
            if connected_waiters:
                await asyncio.gather(*connected_waiters)
        finally:
            pending = tuple(
                waiter
                for _, waiter in waiters
                if not waiter.done()
            )
            for waiter in pending:
                waiter.cancel()
            if pending:
                await asyncio.gather(*pending, return_exceptions=True)

    async def _run_live(self) -> None:
        async with asyncio.TaskGroup() as tasks:
            tasks.create_task(self._publish_periodically())
            tasks.create_task(self._publish_music_periodically())
            tasks.create_task(self._publish_sessions_periodically())
            while True:
                self._collect_now.clear()
                await self.collect()
                self._offer_current()
                try:
                    await asyncio.wait_for(self._collect_now.wait(), self.interval)
                except TimeoutError:
                    pass

    async def _publish_periodically(self) -> None:
        while True:
            await asyncio.sleep(self.publish_interval)
            self._offer_current()

    async def _publish_music_periodically(self) -> None:
        """Push the current song to every device that renders it.

        The music page follows the song, so this runs on its own short cadence
        instead of the usage cadence: a lyric line has to arrive while it is
        being sung. Ticks are aligned to a monotonic deadline so the collection
        and acknowledgement cost of one tick does not lengthen the cadence.
        """

        if self.music_source is None:
            return
        deadline = self.monotonic()
        while True:
            deadline += self.music_interval
            await self._wait_for_music_tick(deadline)
            if self.monotonic() >= deadline:
                # A slow lyric fetch overran the tick; resume from the current time.
                deadline = self.monotonic()
            try:
                publication = await self.music_source.collect()
            except Exception as exc:
                LOGGER.debug("now-playing collection failed: %s", type(exc).__name__)
                continue
            if publication is None:
                continue
            self.now_playing = publication
            for session in self.sessions:
                await session.publish_now_playing(publication)

    async def _publish_sessions_periodically(self) -> None:
        """Push the Claude Code roster to every device that renders it.

        Claude publishes its session state as files, so the source wakes this
        loop when one changes instead of being read on a timer: a session that
        starts or starts working reaches the panel in a fraction of a second.
        The interval is only the longest this loop stays blind, which covers the
        states no file reports, and the walk a reconnect needs.
        """

        source = self.sessions_source
        if source is None:
            return
        while True:
            await self._wait_for_sessions_event(source)
            try:
                publication = await source.collect()
            except Exception as exc:
                LOGGER.debug("sessions collection failed: %s", type(exc).__name__)
                continue
            for session in self.sessions:
                await session.publish_sessions(publication)

    async def _wait_for_sessions_event(self, source: SessionsSource) -> None:
        """Wait for a sessions change, a link that just came up, or the deadline."""

        change = asyncio.create_task(source.wait_for_change(self.sessions_interval))
        ready = asyncio.create_task(self._sessions_now.wait())
        try:
            await asyncio.wait((change, ready), return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in (change, ready):
                task.cancel()
            await asyncio.gather(change, ready, return_exceptions=True)
            self._sessions_now.clear()
        if change.done() and not change.cancelled() and change.exception() is not None:
            # A watcher that fails once would otherwise spin this loop at full
            # speed until the next change.
            LOGGER.debug("sessions watch failed: %s", type(change.exception()).__name__)
            await asyncio.sleep(1.0)

    async def _wait_for_music_tick(self, deadline: float) -> None:
        """Wait for the cadence, or until the player reports a new state.

        A seek, a pause or a track change reaches the mirror in a fraction of a
        second, but waiting for the cadence would hide it for up to a whole
        tick: seeking back and watching the panel would show the place the song
        has left until the next frame. The wait is therefore sliced, and the
        player's own report ends it early.
        """

        reported = self._player_revision()
        while True:
            delay = deadline - self.monotonic()
            if delay <= 0:
                return
            if self._player_revision() != reported:
                return
            await asyncio.sleep(min(delay, MUSIC_WATCH_SECONDS))

    def _player_revision(self) -> int:
        """What the music source last saw the player report, 0 when it cannot say."""

        revision = getattr(self.music_source, "revision", None)
        if revision is None:
            return 0
        try:
            return revision()
        except Exception:
            return 0

    @staticmethod
    def _snapshot(providers: Mapping[Provider, ProviderUsage]) -> UsageSnapshot:
        """Build a publication from the providers the active source manages."""

        return UsageSnapshot(
            deepseek=providers.get(Provider.DEEPSEEK),
            minimax=providers.get(Provider.MINIMAX),
        )

    def _offer_current(self) -> tuple[tuple[DeviceSession, int], ...]:
        providers = self.store.providers
        if not providers:
            return ()
        snapshot = self._snapshot(providers)
        return tuple(
            (session, session.offer(snapshot)) for session in self.sessions
        )
