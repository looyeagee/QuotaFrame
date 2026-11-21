"""Device state and commands behind the local control page.

The tray and the page drive the same sessions through the same service entry
points; this module only translates between them and reports what happened per
device, because a session drops an ineligible command silently.
"""

from __future__ import annotations

import os
import time
from collections.abc import Callable, Mapping
from typing import Protocol

from quotaframe_bridge.domain.models import ProviderUsage, UsageWindow
from quotaframe_bridge.service.devices import PairerT, TrayServiceGraph
from quotaframe_bridge.service.multi_device import (
    SCREEN_PAGE_CAPABILITY,
    SCREEN_TOGGLE_CAPABILITY,
    DeviceSession,
    MultiDeviceBridgeService,
)
from quotaframe_bridge.transports.base import device_status

# A command already on the wire clears itself in one round trip and only means
# "this click would have been dropped", which is exactly what the tray does.
# Every other reason stays true until something changes outside the page, so
# only those disable a control.
TRANSIENT_REASONS = frozenset({"busy"})


class PanelControl(Protocol):
    """What the HTTP layer needs; kept narrow so tests can stand in for it."""

    def state(self) -> dict[str, object]:
        """Return one JSON-ready snapshot of every device and provider."""

    def toggle_screen(self) -> dict[str, object]:
        """Ask every eligible panel to switch between clock and usage."""

    def turn_page(self, direction: int) -> dict[str, object]:
        """Ask every eligible panel to change usage page by one step."""

    def refresh(self) -> dict[str, object]:
        """Ask the collection loop to read usage now."""


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

    def state(self) -> dict[str, object]:
        addresses = self._addresses()
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
                self._device(session, addresses.get(session))
                for session in self._service.sessions
            ],
            "providers": [
                _usage(usage) for usage in self._service.store.providers.values()
            ],
            "now_playing": _now_playing(self._service.now_playing),
        }

    def toggle_screen(self) -> dict[str, object]:
        return self._dispatch(SCREEN_TOGGLE_CAPABILITY, None)

    def turn_page(self, direction: int) -> dict[str, object]:
        if direction not in (-1, 1):
            raise ValueError("page direction must be -1 or 1")
        return self._dispatch(SCREEN_PAGE_CAPABILITY, direction)

    def refresh(self) -> dict[str, object]:
        self._service.request_collection()
        return {"requested": True}

    def _dispatch(self, capability: str, direction: int | None) -> dict[str, object]:
        # Eligibility is measured before dispatch so a device that would swallow
        # the command is reported instead of counted as success.
        results = []
        eligible = False
        for session in self._service.sessions:
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
            # One service entry point per command keeps the page and the tray on
            # the same scheduling path; ineligible sessions drop it themselves.
            if direction is None:
                self._service.toggle_screensavers()
            else:
                self._service.turn_pages(direction)
        return {"devices": results}

    def _addresses(self) -> Mapping[DeviceSession, str]:
        if self._graph is None:
            return {}
        return {device.session: device.address for device in self._graph.devices}

    def _device(self, session: DeviceSession, address: str | None) -> dict[str, object]:
        status = device_status(session.transport)
        capabilities = sorted(status.capabilities) if status is not None else []
        return {
            "name": session.label,
            "address": address or "",
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
        }


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
