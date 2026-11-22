"""Device state and commands behind the local control page.

The tray and the page drive the same sessions through the same service entry
points; this module only translates between them and reports what happened per
device, because a session drops an ineligible command silently.
"""

from __future__ import annotations

import asyncio
import logging
import os
import time
from collections.abc import Callable, Mapping
from typing import Protocol

from quotaframe_bridge.domain.models import ProviderUsage, UsageWindow
from quotaframe_bridge.service.devices import (
    PairerT,
    TrayDevice,
    TrayServiceGraph,
    run_adoption,
)
from quotaframe_bridge.service.multi_device import (
    SCREEN_PAGE_CAPABILITY,
    SCREEN_TOGGLE_CAPABILITY,
    DeviceSession,
    MultiDeviceBridgeService,
)
from quotaframe_bridge.transports.base import (
    TransportCleanupError,
    device_status,
)

LOGGER = logging.getLogger(__name__)

# A command already on the wire clears itself in one round trip and only means
# "this click would have been dropped", which is exactly what the tray does.
# Every other reason stays true until something changes outside the page, so
# only those disable a control.
TRANSIENT_REASONS = frozenset({"busy"})


class PanelControl(Protocol):
    """What the HTTP layer needs; kept narrow so tests can stand in for it."""

    def state(self) -> dict[str, object]:
        """Return one JSON-ready snapshot of every device and provider."""

    def toggle_screen(self, address: str | None = None) -> dict[str, object]:
        """Ask the chosen panel, or every eligible panel, to switch between clock and usage."""

    def turn_page(self, direction: int, address: str | None = None) -> dict[str, object]:
        """Ask the chosen panel, or every eligible panel, to change usage page by one step."""

    def set_alert(self, mode: int, address: str | None = None) -> dict[str, object]:
        """Ask the chosen panel, or every eligible panel, to persist one alert mode."""

    def set_brightness(
        self, percent: int, address: str | None = None
    ) -> dict[str, object]:
        """Ask the chosen panel, or every eligible panel, to persist one brightness."""

    def refresh(self) -> dict[str, object]:
        """Ask the collection loop to read usage now."""

    def pair_new_device(self) -> dict[str, object]:
        """Scan for a panel nobody owns yet, pair with it, and adopt it."""

    def stop_pairing(self) -> dict[str, object]:
        """Cancel a running adoption scan and close its BLE link."""

    def repair_pairing(self, address: str) -> dict[str, object]:
        """Drop one panel's bond and pair again, or report why that cannot run."""

    def forget_device(self, address: str) -> dict[str, object]:
        """Remove one adopted panel from the Bridge, keeping the system bond."""


class ServiceControl:
    """Expose one running service to the control page.

    Every method must be called on the service's event loop: commands schedule
    tasks and the snapshot reads state the collection loop is mutating.
    """

    def __init__(
        self,
        service: MultiDeviceBridgeService,
        *,
        graph: TrayServiceGraph[PairerT] | None = None,
        version: str = "",
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._service = service
        self._graph = graph
        self._version = version
        self._clock = clock
        self._started_at = clock()
        # Pairing actions run as tasks on the service loop; the page watches
        # the flags and the last outcome through state() instead of a reply.
        self._adoption_task: asyncio.Task[None] | None = None
        self._last_adoption: dict[str, object] | None = None
        self._last_pairing_error: dict[str, str] | None = None

    def state(self) -> dict[str, object]:
        graph_devices = self._graph_devices()
        return {
            "bridge": {
                "version": self._version,
                "pid": os.getpid(),
                "uptime_seconds": int(self._clock() - self._started_at),
            },
            "collection": {
                "count": self._service.collection_count,
                "consecutive_failures": self._service.consecutive_failures,
                "last_attempt_at": self._service.store.last_collection_attempt,
            },
            "devices": [
                self._device(session, graph_devices.get(session))
                for session in self._service.sessions
            ],
            "providers": [
                _usage(usage) for usage in self._service.store.providers.values()
            ],
            "now_playing": _now_playing(self._service.now_playing),
            "pairing": {
                # Adopting, repairing and forgetting require the ownership
                # graph; single-device and dry-run modes have no store to act on.
                "supported": self._graph is not None,
                "adoption": {
                    "running": self._adoption_running(),
                    "last": self._last_adoption,
                },
                "last_error": self._last_pairing_error,
            },
        }

    def toggle_screen(self, address: str | None = None) -> dict[str, object]:
        return self._dispatch(SCREEN_TOGGLE_CAPABILITY, None, address)

    def turn_page(self, direction: int, address: str | None = None) -> dict[str, object]:
        if direction not in (-1, 1):
            raise ValueError("page direction must be -1 or 1")
        return self._dispatch(SCREEN_PAGE_CAPABILITY, direction, address)

    def set_alert(self, mode: int, address: str | None = None) -> dict[str, object]:
        if type(mode) is not int or mode not in (0, 1, 2):
            raise ValueError("alert mode must be 0, 1 or 2")
        return self._setting(
            DeviceSession.alert_command_unavailable,
            lambda session: session.schedule_alert_set(mode),
            address,
        )

    def set_brightness(self, percent: int, address: str | None = None) -> dict[str, object]:
        if type(percent) is not int or not 0 <= percent <= 100:
            raise ValueError("brightness percent must be an int from 0 to 100")
        return self._setting(
            DeviceSession.brightness_command_unavailable,
            lambda session: session.schedule_brightness_set(percent),
            address,
        )

    def refresh(self) -> dict[str, object]:
        self._service.request_collection()
        return {"requested": True}

    def _select(
        self, address: str | None
    ) -> tuple[list[DeviceSession], dict[str, object] | None]:
        """Resolve one device address to its session.

        No address selects every session; an unknown address, or any address in
        a mode without an ownership graph, produces the per-device rejection the
        page renders like any other.
        """

        if address is None:
            return list(self._service.sessions), None
        if self._graph is not None:
            matched = [
                device.session
                for device in self._graph.devices
                if device.address == address
            ]
            if matched:
                return matched, None
        return [], {
            "devices": [{"name": address, "accepted": False, "reason": "unknown_device"}]
        }

    def _dispatch(
        self, capability: str, direction: int | None, address: str | None = None
    ) -> dict[str, object]:
        # Eligibility is measured before dispatch so a device that would swallow
        # the command is reported instead of counted as success.
        sessions, rejection = self._select(address)
        if rejection is not None:
            return rejection
        results = []
        eligible = False
        for session in sessions:
            reason = session.screen_command_unavailable(capability)
            eligible = eligible or reason is None
            results.append(
                {
                    "name": session.label,
                    "accepted": reason is None,
                    "reason": reason,
                }
            )
        if eligible:
            # One scheduling call per session keeps the page and the tray on the
            # same path; ineligible sessions drop the command themselves.
            for session in sessions:
                if direction is None:
                    session.schedule_screen_toggle()
                else:
                    session.schedule_screen_page(direction < 0)
        return {"devices": results}

    def _setting(
        self,
        unavailable: Callable[[DeviceSession], str | None],
        schedule: Callable[[DeviceSession], None],
        address: str | None,
    ) -> dict[str, object]:
        """Apply one persisted setting to the chosen sessions, reporting each one."""

        sessions, rejection = self._select(address)
        if rejection is not None:
            return rejection
        results = []
        eligible = False
        for session in sessions:
            reason = unavailable(session)
            eligible = eligible or reason is None
            results.append(
                {
                    "name": session.label,
                    "accepted": reason is None,
                    "reason": reason,
                }
            )
        for session in sessions:
            schedule(session)
        return {"devices": results}

    def pair_new_device(self) -> dict[str, object]:
        """Start one bounded adoption scan, as the tray's add-device action does."""

        if self._graph is None:
            return {"started": False, "reason": "unsupported"}
        if self._adoption_running():
            return {"started": False, "reason": "busy"}
        self._last_adoption = None
        self._last_pairing_error = None
        self._adoption_task = asyncio.create_task(self._adopt())
        return {"started": True}

    def stop_pairing(self) -> dict[str, object]:
        """Cancel the running adoption scan; the page watches state for the result."""

        if not self._adoption_running():
            return {"stopped": False, "reason": "idle"}
        # Record the outcome here: a scan cancelled before its first step never
        # runs its own cleanup, so the page would otherwise see no result.
        self._last_adoption = {"stopped": True}
        self._adoption_task.cancel()
        return {"stopped": True}

    def repair_pairing(self, address: str) -> dict[str, object]:
        """Ask one panel to drop its bond and pair again on the next connect."""

        return self._pairing_command(address, self._start_repair)

    def forget_device(self, address: str) -> dict[str, object]:
        """Remove one adopted panel from the Bridge; the system bond stays."""

        return self._pairing_command(address, self._start_forget)

    def _pairing_command(
        self,
        address: str,
        start: Callable[[object], str | None],
    ) -> dict[str, object]:
        # The single-device response shape matches the screen commands, so the
        # page reports a rejection with the same per-device reason renderer.
        device = None if self._graph is None else self._graph.get(address)
        if device is None:
            reason = "unsupported" if self._graph is None else "unknown_device"
            return {"devices": [{"name": address, "accepted": False, "reason": reason}]}
        reason = start(device)
        return {
            "devices": [
                {
                    "name": device.name,
                    "accepted": reason is None,
                    "reason": reason,
                }
            ]
        }

    def _start_repair(self, device: TrayDevice[PairerT]) -> str | None:
        pairer = getattr(device, "pairer", None)
        if not callable(getattr(pairer, "request_repair", None)):
            # macOS owns the ceremony; the tray shows the same manual guidance.
            return "manual"
        if device.session.exclusive_active:
            return "exclusive"
        self._last_pairing_error = None
        asyncio.create_task(self._repair(device))
        return None

    def _start_forget(self, device: TrayDevice[PairerT]) -> str | None:
        if device.session.exclusive_active:
            return "exclusive"
        self._last_pairing_error = None
        asyncio.create_task(self._forget(device))
        return None

    async def _repair(self, device: TrayDevice[PairerT]) -> None:
        async def repair(transport):
            device.pairer.request_repair()
            await transport.reset()

        try:
            await device.session.run_exclusive(repair, interrupt_connect=True)
        except TransportCleanupError:
            self._record_pairing_error(device.name, "repair", "cleanup_failed")
        except RuntimeError:
            # An OTA session won the exclusive lock between the page's poll and
            # this task; the tray reports the same outcome as "updating".
            self._record_pairing_error(device.name, "repair", "busy")
        except Exception:
            LOGGER.exception("pairing repair failed: device=%s", device.name)
            self._record_pairing_error(device.name, "repair", "failed")
        else:
            self._service.request_collection()

    async def _forget(self, device: TrayDevice[PairerT]) -> None:
        graph = self._graph
        if graph is None:
            return
        try:
            await graph.forget(device.address, expected=device)
        except TransportCleanupError:
            # Removal already persisted; only the link cleanup failed.
            self._record_pairing_error(device.name, "forget", "cleanup_failed")
        except RuntimeError:
            self._record_pairing_error(device.name, "forget", "busy")
        except Exception:
            LOGGER.exception("forget device failed: device=%s", device.name)
            self._record_pairing_error(device.name, "forget", "failed")

    async def _adopt(self) -> None:
        graph = self._graph
        if graph is None:
            return
        try:
            if graph.store.read_error:
                self._last_adoption = {"error": "records"}
                return
            adopted = await run_adoption(graph)
        except asyncio.CancelledError:
            # discover_device's cleanup already closed whichever BLE link the
            # scan held when cancellation landed; the result was recorded by
            # stop_pairing, so re-raise without overwriting anything.
            raise
        except Exception:
            LOGGER.exception("adoption scan failed")
            self._last_adoption = {"error": "failed"}
            return
        self._last_adoption = {"adopted": [device.name for device in adopted]}

    def _record_pairing_error(self, name: str, action: str, error: str) -> None:
        self._last_pairing_error = {"device": name, "action": action, "error": error}

    def _adoption_running(self) -> bool:
        return self._adoption_task is not None and not self._adoption_task.done()

    def _graph_devices(self) -> Mapping[DeviceSession, TrayDevice[PairerT]]:
        if self._graph is None:
            return {}
        return {device.session: device for device in self._graph.devices}

    def _device(
        self,
        session: DeviceSession,
        device: TrayDevice[PairerT] | None,
    ) -> dict[str, object]:
        status = device_status(session.transport)
        capabilities = sorted(status.capabilities) if status is not None else []
        return {
            "name": session.label,
            "address": "" if device is None else device.address,
            "connected": session.connected,
            "secure": bool(status.secure) if status is not None else False,
            "capabilities": capabilities,
            "firmware": status.firmware_version if status is not None else "",
            "target": status.target if status is not None else "",
            "page": status.page if status is not None else "",
            "incompatible": bool(getattr(session.transport, "incompatible", False)),
            "screen": {
                "toggle": _availability(
                    session.screen_command_unavailable(SCREEN_TOGGLE_CAPABILITY)
                ),
                "page": _availability(
                    session.screen_command_unavailable(SCREEN_PAGE_CAPABILITY)
                ),
            },
            "alert": {
                # The session reports a written mode until a reconnect re-reads status.
                "mode": session.alert_mode,
                "set": _availability(session.alert_command_unavailable()),
            },
            "brightness": {
                # Same write-then-report contract as the alert mode.
                "percent": session.brightness,
                "set": _availability(session.brightness_command_unavailable()),
            },
            "pairing": {
                "repair": self._repair_availability(device, session),
                "forget": _availability(self._pairing_unavailable(session)),
            },
        }

    def _pairing_unavailable(self, session: DeviceSession) -> str | None:
        if self._graph is None:
            return "unsupported"
        if session.exclusive_active:
            return "exclusive"
        return None

    def _repair_availability(
        self,
        device: TrayDevice[PairerT] | None,
        session: DeviceSession,
    ) -> dict[str, object]:
        pairer = None if device is None else getattr(device, "pairer", None)
        # A pairer without request_repair (macOS) can only show manual steps;
        # the page uses the mode to decide between a request and the guidance.
        mode = "ceremony" if callable(getattr(pairer, "request_repair", None)) else "manual"
        return {**_availability(self._pairing_unavailable(session)), "mode": mode}


def _availability(reason: str | None) -> dict[str, object]:
    if reason in TRANSIENT_REASONS:
        return {"available": True, "pending": True, "reason": None}
    return {"available": reason is None, "pending": False, "reason": reason}


def _window(window: UsageWindow | None) -> dict[str, object] | None:
    if window is None:
        return None
    return {"used_percent": window.used_percent, "reset_at": window.reset_at}


def _usage(usage: ProviderUsage) -> dict[str, object]:
    return {
        "provider": usage.provider.value,
        "state": usage.state.value,
        "sampled_at": usage.sampled_at,
        # Balance stays a string so cents survive the trip through a browser float.
        "balance": None if usage.balance is None else f"{usage.balance:.2f}",
        "currency": usage.currency,
        "short": _window(usage.short),
        "week": _window(usage.week),
    }


def _now_playing(publication: object) -> dict[str, object] | None:
    if publication is None:
        return None
    return {
        "title": publication.title,
        "artist": publication.artist,
        "album": publication.album,
        "playing": publication.playing,
        "position_seconds": publication.position_seconds,
        "duration_seconds": publication.duration_seconds,
    }
