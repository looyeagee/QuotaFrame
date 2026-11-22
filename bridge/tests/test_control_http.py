"""The local control page: routing, eligibility reporting, and the socket."""

from __future__ import annotations

import asyncio
import json
import os
import stat
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from quotaframe_bridge.control import ServiceControl, load_or_create_token, token_path
from quotaframe_bridge.control.routes import ControlRoutes, Request
from quotaframe_bridge.control.server import MAX_BODY_BYTES, ControlServer
from quotaframe_bridge.domain.models import (
    NowPlayingPublication,
    Provider,
    ProviderUsage,
    SourceState,
    UsageWindow,
)
from quotaframe_bridge.protocol.messages import ALERT_SET_CAPABILITY, DeviceStatus
from quotaframe_bridge.service.multi_device import (
    SCREEN_PAGE_CAPABILITY,
    SCREEN_TOGGLE_CAPABILITY,
    DeviceSession,
    MultiDeviceBridgeService,
)
from quotaframe_bridge.transports.base import TransportCleanupError
from quotaframe_bridge.cli.main import _control_server
from quotaframe_bridge.service.state import UsageStateStore
from quotaframe_bridge.sources.mock import MockUsageSource

TOKEN = "test-token-0123456789"


class ScreenTransport:
    """A transport that advertises whatever capabilities a test needs."""

    def __init__(
        self,
        capabilities: frozenset[str] = frozenset(),
        *,
        connected: bool = True,
        secure: bool = True,
        alert: int | None = None,
    ) -> None:
        self.connected = connected
        self.sent: list[tuple[bytes, str, int]] = []
        self.resets = 0
        self.status = DeviceStatus(
            name="Panel",
            secure=secure,
            protocol=1,
            page="music",
            capabilities=capabilities,
            firmware_version="v1.2.1",
            target="waveshare_rlcd_42",
            alert=alert,
        )

    async def send_command(self, payload: bytes, command: str, sequence: int) -> None:
        self.sent.append((payload, command, sequence))

    async def reset(self) -> None:
        self.resets += 1

    async def close(self) -> None:
        self.connected = False


class FakeSession:
    """A session as the control page sees it, without a transport lifecycle."""

    def __init__(
        self,
        label: str,
        *,
        capabilities=frozenset({SCREEN_TOGGLE_CAPABILITY, SCREEN_PAGE_CAPABILITY}),
        connected: bool = True,
        secure: bool = True,
        reason: str | None = None,
        alert: int | None = None,
    ) -> None:
        self.label = label
        self.connected = connected
        self.transport = ScreenTransport(
            capabilities, connected=connected, secure=secure, alert=alert
        )
        self._reason = reason
        self.exclusive_active = False
        self._exclusive_failure: Exception | None = None

    def screen_command_unavailable(self, capability: str) -> str | None:
        if self._reason is not None:
            return self._reason
        if not self.connected:
            return "disconnected"
        if not self.transport.status.secure:
            return "unsecured"
        if capability not in self.transport.status.capabilities:
            return "missing_capability"
        return None

    def alert_command_unavailable(self) -> str | None:
        return self.screen_command_unavailable(ALERT_SET_CAPABILITY)

    @property
    def alert_mode(self) -> int | None:
        return self.transport.status.alert

    async def run_exclusive(self, operation, *, interrupt_connect: bool = False) -> None:
        if self._exclusive_failure is not None:
            raise self._exclusive_failure
        await operation(self.transport)


class FakePairer:
    """The pairer surface the page's repair action drives."""

    def __init__(self) -> None:
        self.repairs = 0

    def request_repair(self) -> None:
        self.repairs += 1

    async def ensure_paired(self, device) -> None:
        return None


class FakeGraphDevice:
    """One adopted device as the pairing actions see it."""

    def __init__(self, session: FakeSession, *, pairer=object()) -> None:
        self.session = session
        self.address = "AA:BB:CC:DD:EE:FF"
        self.pairer = pairer
        self.name = session.label


class FakeGraph:
    """The ownership surface the page's pairing actions need."""

    def __init__(self, *devices: FakeGraphDevice, forget_error: Exception | None = None) -> None:
        self.devices = tuple(devices)
        self.store = SimpleNamespace(read_error=False)
        self.forget_error = forget_error
        self.forgotten: list[str] = []

    def get(self, address: str) -> FakeGraphDevice | None:
        wanted = address.strip().upper()
        return next(
            (device for device in self.devices if device.address.upper() == wanted), None
        )

    async def forget(self, address: str, *, expected) -> bool:
        if self.forget_error is not None:
            raise self.forget_error
        device = self.get(address)
        if device is None or device is not expected:
            return False
        self.forgotten.append(device.address)
        return True


class FakeService:
    """The subset of the service the control page reads and drives."""

    def __init__(self, sessions=(), *, usage=None, now_playing=None) -> None:
        self.sessions = tuple(sessions)
        self.collection_count = 3
        self.consecutive_failures = 1
        self.now_playing = now_playing
        self.store = UsageStateStore()
        if usage is not None:
            self.store.merge_collection(usage, attempted_at=1_700_000_100)
        self.toggles = 0
        self.pages: list[int] = []
        self.alerts: list[int] = []
        self.refreshes = 0

    def toggle_screensavers(self) -> None:
        self.toggles += 1

    def turn_pages(self, direction: int) -> None:
        self.pages.append(direction)

    def set_alert(self, mode: int) -> None:
        self.alerts.append(mode)

    def request_collection(self) -> None:
        self.refreshes += 1


def _deepseek() -> ProviderUsage:
    return ProviderUsage(
        provider=Provider.DEEPSEEK,
        state=SourceState.OK,
        sampled_at=1_700_000_000,
        balance=Decimal("12.34"),
        currency="CNY",
    )


def _minimax() -> ProviderUsage:
    return ProviderUsage(
        provider=Provider.MINIMAX,
        state=SourceState.OK,
        sampled_at=1_700_000_000,
        short=UsageWindow(40, 1_700_003_600),
        week=UsageWindow(85),
    )


class ServiceControlTests(unittest.TestCase):
    def test_state_reports_devices_providers_and_now_playing(self) -> None:
        session = FakeSession("QF-WS-S3-R42-C092")
        service = FakeService(
            [session],
            usage={Provider.DEEPSEEK: _deepseek(), Provider.MINIMAX: _minimax()},
            now_playing=NowPlayingPublication(
                title="Song",
                artist="Artist",
                album="Album",
                position_seconds=42,
                duration_seconds=200,
                playing=True,
            ),
        )
        device = type("Device", (), {"session": session, "address": "AA:BB:CC:DD:EE:FF"})
        graph = type("Graph", (), {"devices": (device,)})()

        control = ServiceControl(service, graph=graph, version="1.2.1", clock=lambda: 100.0)
        state = control.state()

        self.assertEqual(state["bridge"]["version"], "1.2.1")
        self.assertEqual(state["bridge"]["pid"], os.getpid())
        self.assertEqual(state["collection"]["count"], 3)
        self.assertEqual(state["collection"]["consecutive_failures"], 1)
        self.assertEqual(state["collection"]["last_attempt_at"], 1_700_000_100)
        reported = state["devices"][0]
        self.assertEqual(reported["name"], "QF-WS-S3-R42-C092")
        self.assertEqual(reported["address"], "AA:BB:CC:DD:EE:FF")
        self.assertEqual(reported["firmware"], "v1.2.1")
        self.assertEqual(
            reported["capabilities"], [SCREEN_PAGE_CAPABILITY, SCREEN_TOGGLE_CAPABILITY]
        )
        self.assertTrue(reported["screen"]["toggle"]["available"])
        self.assertTrue(reported["screen"]["page"]["available"])

        providers = {item["provider"]: item for item in state["providers"]}
        self.assertEqual(providers["deepseek"]["balance"], "12.34")
        self.assertEqual(providers["deepseek"]["currency"], "CNY")
        self.assertIsNone(providers["deepseek"]["short"])
        self.assertEqual(providers["minimax"]["short"]["used_percent"], 40)
        self.assertEqual(providers["minimax"]["week"]["used_percent"], 85)
        self.assertEqual(state["now_playing"]["title"], "Song")
        self.assertEqual(state["now_playing"]["duration_seconds"], 200)

    def test_a_missing_capability_is_reported_per_command(self) -> None:
        session = FakeSession("old", capabilities=frozenset({SCREEN_TOGGLE_CAPABILITY}))
        control = ServiceControl(FakeService([session]), version="1.2.1")

        reported = control.state()["devices"][0]

        self.assertTrue(reported["screen"]["toggle"]["available"])
        self.assertEqual(
            reported["screen"]["page"],
            {"available": False, "pending": False, "reason": "missing_capability"},
        )

    def test_an_in_flight_command_stays_available(self) -> None:
        session = FakeSession("ready", reason="busy")
        reported = ServiceControl(FakeService([session]), version="1.2.1").state()["devices"][0]

        self.assertEqual(
            reported["screen"]["toggle"],
            {"available": True, "pending": True, "reason": None},
        )
        self.assertEqual(
            reported["screen"]["page"],
            {"available": True, "pending": True, "reason": None},
        )

    def test_state_omits_now_playing_before_the_first_track(self) -> None:
        state = ServiceControl(FakeService([]), version="1.2.1").state()
        self.assertIsNone(state["now_playing"])
        self.assertEqual(state["devices"], [])
        self.assertEqual(state["providers"], [])

    def test_state_reports_the_alert_mode_and_setting_availability(self) -> None:
        capable = FakeSession(
            "reporting", capabilities=frozenset({ALERT_SET_CAPABILITY}), alert=1
        )
        silent = FakeSession("silent")
        control = ServiceControl(FakeService([capable, silent]), version="1.2.1")

        devices = control.state()["devices"]

        self.assertEqual(devices[0]["alert"]["mode"], 1)
        self.assertTrue(devices[0]["alert"]["set"]["available"])
        self.assertIsNone(devices[1]["alert"]["mode"])
        self.assertFalse(devices[1]["alert"]["set"]["available"])
        self.assertEqual(devices[1]["alert"]["set"]["reason"], "missing_capability")

    def test_dispatch_reports_why_a_device_is_skipped(self) -> None:
        ready = FakeSession("ready")
        gone = FakeSession("gone", connected=False)
        old = FakeSession("old", capabilities=frozenset())
        service = FakeService([ready, gone, old])
        control = ServiceControl(service, version="1.2.1")

        result = control.toggle_screen()

        self.assertEqual(service.toggles, 1)
        self.assertEqual(
            [(item["name"], item["accepted"], item["reason"]) for item in result["devices"]],
            [
                ("ready", True, None),
                ("gone", False, "disconnected"),
                ("old", False, "missing_capability"),
            ],
        )

    def test_dispatch_sends_nothing_when_every_device_is_ineligible(self) -> None:
        service = FakeService([FakeSession("gone", reason="exclusive")])

        result = ServiceControl(service, version="1.2.1").turn_page(-1)

        self.assertEqual(service.pages, [])
        self.assertFalse(result["devices"][0]["accepted"])

    def test_turn_page_rejects_other_directions(self) -> None:
        control = ServiceControl(FakeService([]), version="1.2.1")
        for direction in (0, 2, -2):
            with self.subTest(direction=direction):
                with self.assertRaises(ValueError):
                    control.turn_page(direction)

    def test_refresh_asks_the_collection_loop(self) -> None:
        service = FakeService([])
        self.assertEqual(ServiceControl(service, version="1").refresh(), {"requested": True})
        self.assertEqual(service.refreshes, 1)


class PairingStateTests(unittest.TestCase):
    """The state snapshot must tell the page which pairing actions exist."""

    def test_state_without_a_graph_reports_pairing_unsupported(self) -> None:
        reported = ServiceControl(FakeService([FakeSession("solo")]), version="1").state()

        self.assertFalse(reported["pairing"]["supported"])
        self.assertIsNone(reported["pairing"]["adoption"]["last"])
        pairing = reported["devices"][0]["pairing"]
        self.assertFalse(pairing["repair"]["available"])
        self.assertEqual(pairing["repair"]["reason"], "unsupported")
        self.assertFalse(pairing["forget"]["available"])

    def test_an_owned_device_reports_ceremony_repair_and_forget(self) -> None:
        session = FakeSession("QF-1")
        device = FakeGraphDevice(session, pairer=FakePairer())
        state = ServiceControl(
            FakeService([session]), graph=FakeGraph(device), version="1"
        ).state()

        self.assertTrue(state["pairing"]["supported"])
        self.assertEqual(state["devices"][0]["address"], device.address)
        pairing = state["devices"][0]["pairing"]
        self.assertTrue(pairing["repair"]["available"])
        self.assertEqual(pairing["repair"]["mode"], "ceremony")
        self.assertTrue(pairing["forget"]["available"])

    def test_a_pairer_without_a_ceremony_reports_manual_repair(self) -> None:
        session = FakeSession("mac")
        device = FakeGraphDevice(session, pairer=object())
        pairing = ServiceControl(
            FakeService([session]), graph=FakeGraph(device), version="1"
        ).state()["devices"][0]["pairing"]

        self.assertTrue(pairing["repair"]["available"])
        self.assertEqual(pairing["repair"]["mode"], "manual")

    def test_an_exclusive_session_blocks_repair_and_forget(self) -> None:
        session = FakeSession("QF-1")
        session.exclusive_active = True
        device = FakeGraphDevice(session, pairer=FakePairer())
        pairing = ServiceControl(
            FakeService([session]), graph=FakeGraph(device), version="1"
        ).state()["devices"][0]["pairing"]

        self.assertEqual(pairing["repair"]["reason"], "exclusive")
        self.assertEqual(pairing["forget"]["reason"], "exclusive")


class PairingCommandTests(unittest.IsolatedAsyncioTestCase):
    """Pairing actions run as tasks on the loop and report through state."""

    async def test_pair_new_device_adopts_and_reports_the_result(self) -> None:
        session = FakeSession("QF-1")
        device = FakeGraphDevice(session, pairer=FakePairer())
        graph = FakeGraph(device)
        control = ServiceControl(FakeService([session]), graph=graph, version="1")

        async def adoption(graph_arg, **kwargs):
            self.assertIs(graph_arg, graph)
            return [device]

        with patch("quotaframe_bridge.control.backend.run_adoption", adoption):
            self.assertEqual(control.pair_new_device(), {"started": True})
            await asyncio.sleep(0)

        state = control.state()
        self.assertFalse(state["pairing"]["adoption"]["running"])
        self.assertEqual(state["pairing"]["adoption"]["last"], {"adopted": ["QF-1"]})

    async def test_a_second_adoption_is_reported_as_busy(self) -> None:
        session = FakeSession("QF-1")
        device = FakeGraphDevice(session, pairer=FakePairer())
        control = ServiceControl(
            FakeService([session]), graph=FakeGraph(device), version="1"
        )
        release = asyncio.Event()

        async def adoption(graph, **kwargs):
            await release.wait()
            return []

        with patch("quotaframe_bridge.control.backend.run_adoption", adoption):
            self.assertEqual(control.pair_new_device(), {"started": True})
            self.assertEqual(
                control.pair_new_device(), {"started": False, "reason": "busy"}
            )
            release.set()
            # The task is the observation point the page watches through state.
            await asyncio.gather(control._adoption_task)  # noqa: SLF001

        self.assertEqual(control.state()["pairing"]["adoption"]["last"], {"adopted": []})

    async def test_pairing_is_reported_unsupported_without_a_graph(self) -> None:
        control = ServiceControl(FakeService([]), version="1")

        self.assertEqual(
            control.pair_new_device(), {"started": False, "reason": "unsupported"}
        )
        self.assertIsNone(control.state()["pairing"]["adoption"]["last"])

    async def test_stop_pairing_cancels_the_running_scan(self) -> None:
        session = FakeSession("QF-1")
        device = FakeGraphDevice(session, pairer=FakePairer())
        control = ServiceControl(
            FakeService([session]), graph=FakeGraph(device), version="1"
        )
        release = asyncio.Event()

        async def adoption(graph, **kwargs):
            await release.wait()
            return []

        with patch("quotaframe_bridge.control.backend.run_adoption", adoption):
            self.assertEqual(control.pair_new_device(), {"started": True})
            self.assertEqual(control.stop_pairing(), {"stopped": True})
            with self.assertRaises(asyncio.CancelledError):
                await asyncio.gather(control._adoption_task)  # noqa: SLF001

        state = control.state()
        self.assertFalse(state["pairing"]["adoption"]["running"])
        self.assertEqual(state["pairing"]["adoption"]["last"], {"stopped": True})

    async def test_stop_pairing_without_a_scan_reports_idle(self) -> None:
        control = ServiceControl(FakeService([]), version="1")

        self.assertEqual(
            control.stop_pairing(), {"stopped": False, "reason": "idle"}
        )
        self.assertIsNone(control.state()["pairing"]["adoption"]["last"])

    async def test_unreadable_records_stop_the_scan_before_it_starts(self) -> None:
        session = FakeSession("QF-1")
        graph = FakeGraph(FakeGraphDevice(session, pairer=FakePairer()))
        graph.store.read_error = True
        control = ServiceControl(FakeService([session]), graph=graph, version="1")

        self.assertEqual(control.pair_new_device(), {"started": True})
        await asyncio.sleep(0)

        self.assertEqual(
            control.state()["pairing"]["adoption"]["last"], {"error": "records"}
        )

    async def test_repair_drops_the_bond_and_resets_the_link(self) -> None:
        session = FakeSession("QF-1")
        device = FakeGraphDevice(session, pairer=FakePairer())
        service = FakeService([session])
        control = ServiceControl(service, graph=FakeGraph(device), version="1")

        result = control.repair_pairing("aa:bb:cc:dd:ee:ff")
        await asyncio.sleep(0)

        self.assertTrue(result["devices"][0]["accepted"])
        self.assertEqual(device.pairer.repairs, 1)
        self.assertEqual(session.transport.resets, 1)
        # The tray asks for a usage collection after a successful repair.
        self.assertEqual(service.refreshes, 1)
        self.assertIsNone(control.state()["pairing"]["last_error"])

    async def test_repair_reports_an_unknown_address(self) -> None:
        session = FakeSession("QF-1")
        device = FakeGraphDevice(session, pairer=FakePairer())
        control = ServiceControl(
            FakeService([session]), graph=FakeGraph(device), version="1"
        )

        result = control.repair_pairing("11:22:33:44:55:66")

        self.assertEqual(
            result["devices"],
            [{"name": "11:22:33:44:55:66", "accepted": False, "reason": "unknown_device"}],
        )
        self.assertEqual(device.pairer.repairs, 0)

    async def test_repair_rejects_while_an_exclusive_operation_owns_the_link(self) -> None:
        session = FakeSession("QF-1")
        session.exclusive_active = True
        device = FakeGraphDevice(session, pairer=FakePairer())
        control = ServiceControl(
            FakeService([session]), graph=FakeGraph(device), version="1"
        )

        result = control.repair_pairing(device.address)

        self.assertEqual(result["devices"][0]["reason"], "exclusive")
        self.assertEqual(device.pairer.repairs, 0)

    async def test_a_lost_cleanup_is_recorded_for_the_page(self) -> None:
        session = FakeSession("QF-1")
        session._exclusive_failure = TransportCleanupError("client leaked")  # noqa: SLF001
        device = FakeGraphDevice(session, pairer=FakePairer())
        service = FakeService([session])
        control = ServiceControl(service, graph=FakeGraph(device), version="1")

        control.repair_pairing(device.address)
        await asyncio.sleep(0)

        self.assertEqual(
            control.state()["pairing"]["last_error"],
            {"device": "QF-1", "action": "repair", "error": "cleanup_failed"},
        )
        self.assertEqual(service.refreshes, 0)

    async def test_a_busy_session_is_recorded_for_the_page(self) -> None:
        session = FakeSession("QF-1")
        session._exclusive_failure = RuntimeError(  # noqa: SLF001
            "device operation is already active or device is closing"
        )
        device = FakeGraphDevice(session, pairer=FakePairer())
        control = ServiceControl(
            FakeService([session]), graph=FakeGraph(device), version="1"
        )

        control.repair_pairing(device.address)
        await asyncio.sleep(0)

        self.assertEqual(
            control.state()["pairing"]["last_error"],
            {"device": "QF-1", "action": "repair", "error": "busy"},
        )

    async def test_forget_removes_the_device_from_the_ownership_graph(self) -> None:
        session = FakeSession("QF-1")
        device = FakeGraphDevice(session, pairer=FakePairer())
        graph = FakeGraph(device)
        control = ServiceControl(FakeService([session]), graph=graph, version="1")

        result = control.forget_device(device.address)
        await asyncio.sleep(0)

        self.assertTrue(result["devices"][0]["accepted"])
        self.assertEqual(graph.forgotten, [device.address])

    async def test_a_forget_during_ota_is_recorded_for_the_page(self) -> None:
        session = FakeSession("QF-1")
        device = FakeGraphDevice(session, pairer=FakePairer())
        graph = FakeGraph(device, forget_error=RuntimeError("device is busy with OTA"))
        control = ServiceControl(FakeService([session]), graph=graph, version="1")

        control.forget_device(device.address)
        await asyncio.sleep(0)

        self.assertEqual(graph.forgotten, [])
        self.assertEqual(
            control.state()["pairing"]["last_error"],
            {"device": "QF-1", "action": "forget", "error": "busy"},
        )

    async def test_pairing_commands_without_a_graph_report_unsupported(self) -> None:
        control = ServiceControl(FakeService([]), version="1")

        for action in (control.repair_pairing, control.forget_device):
            with self.subTest(action=action):
                result = action("AA:BB:CC:DD:EE:FF")
                self.assertEqual(
                    result["devices"],
                    [
                        {
                            "name": "AA:BB:CC:DD:EE:FF",
                            "accepted": False,
                            "reason": "unsupported",
                        }
                    ],
                )


class RealSessionTests(unittest.TestCase):
    """The page must drive the same session scheduling path the tray uses."""

    def service(self, transport: ScreenTransport) -> tuple[MultiDeviceBridgeService, DeviceSession]:
        session = DeviceSession("Panel", transport)
        service = MultiDeviceBridgeService(MockUsageSource(), (session,))
        return service, session

    def test_toggle_reaches_the_transport_of_a_connected_session(self) -> None:
        transport = ScreenTransport(frozenset({SCREEN_TOGGLE_CAPABILITY}))
        service, session = self.service(transport)

        async def scenario() -> dict[str, object]:
            session._connected = True  # noqa: SLF001 - a live link needs a connect cycle
            result = ServiceControl(service, version="1.2.1").toggle_screen()
            await asyncio.sleep(0)
            return result

        result = asyncio.run(scenario())

        self.assertTrue(result["devices"][0]["accepted"])
        self.assertEqual([command for _, command, _ in transport.sent], ["screen_toggle"])

    def test_a_command_on_the_wire_reads_as_pending_not_unavailable(self) -> None:
        started = asyncio.Event()
        release = asyncio.Event()

        class SlowTransport(ScreenTransport):
            async def send_command(self, payload, command, sequence) -> None:
                started.set()
                await release.wait()
                await super().send_command(payload, command, sequence)

        transport = SlowTransport(frozenset({SCREEN_TOGGLE_CAPABILITY}))
        service, session = self.service(transport)

        async def scenario() -> tuple[dict, dict]:
            session._connected = True  # noqa: SLF001 - a live link needs a connect cycle
            control = ServiceControl(service, version="1.2.1")
            control.toggle_screen()
            await started.wait()
            during = control.state()["devices"][0]["screen"]["toggle"]
            release.set()
            await asyncio.sleep(0)
            await asyncio.sleep(0)
            after = control.state()["devices"][0]["screen"]["toggle"]
            return during, after

        during, after = asyncio.run(scenario())

        self.assertEqual(during, {"available": True, "pending": True, "reason": None})
        self.assertEqual(after, {"available": True, "pending": False, "reason": None})

    def test_an_unconnected_session_reports_instead_of_sending(self) -> None:
        transport = ScreenTransport(frozenset({SCREEN_TOGGLE_CAPABILITY}), connected=False)
        service, _session = self.service(transport)

        async def scenario() -> dict[str, object]:
            return ServiceControl(service, version="1.2.1").toggle_screen()

        result = asyncio.run(scenario())

        self.assertEqual(result["devices"][0]["reason"], "disconnected")
        self.assertEqual(transport.sent, [])

    def test_an_unsupported_capability_reports_instead_of_sending(self) -> None:
        transport = ScreenTransport(frozenset())
        service, session = self.service(transport)

        async def scenario() -> dict[str, object]:
            session._connected = True  # noqa: SLF001 - a live link needs a connect cycle
            return ServiceControl(service, version="1.2.1").turn_page(1)

        result = asyncio.run(scenario())

        self.assertEqual(result["devices"][0]["reason"], "missing_capability")
        self.assertEqual(transport.sent, [])


class RouteTests(unittest.TestCase):
    def routes(self, *, bound_host: str = "127.0.0.1", service=None) -> ControlRoutes:
        control = ServiceControl(service or FakeService([]), version="1.2.1")
        return ControlRoutes(control, token=TOKEN, bound_host=bound_host)

    def get(
        self,
        target: str = "/api/state",
        *,
        token: str | None = TOKEN,
        host: str = "127.0.0.1:8788",
        method: str = "GET",
        body: bytes = b"",
    ) -> Request:
        headers = {"host": host}
        if token is not None:
            headers["x-quotaframe-token"] = token
        return Request(method=method, target=target, headers=headers, body=body)

    def test_missing_or_wrong_token_is_rejected(self) -> None:
        routes = self.routes()
        self.assertEqual(routes.handle(self.get(token=None)).status, 401)
        self.assertEqual(routes.handle(self.get(token="wrong")).status, 401)
        self.assertEqual(routes.handle(self.get()).status, 200)

    def test_query_token_is_accepted_for_the_first_load(self) -> None:
        response = self.routes().handle(self.get(f"/api/state?token={TOKEN}", token=None))
        self.assertEqual(response.status, 200)

    def test_a_foreign_host_name_is_refused(self) -> None:
        routes = self.routes()
        self.assertEqual(routes.handle(self.get(host="evil.example:8788")).status, 421)
        self.assertEqual(routes.handle(self.get(host="localhost:8788")).status, 200)
        self.assertEqual(routes.handle(self.get(host="[::1]:8788")).status, 200)

    def test_a_lan_bind_does_not_check_the_host_name(self) -> None:
        routes = self.routes(bound_host="0.0.0.0")
        self.assertEqual(routes.handle(self.get(host="192.168.1.9:8788")).status, 200)

    def test_page_carries_the_token_and_localized_copy(self) -> None:
        response = self.routes().handle(self.get("/"))
        body = response.body.decode("utf-8")
        self.assertEqual(response.status, 200)
        self.assertIn(f'const TOKEN = "{TOKEN}";', body)
        self.assertIn("X-QuotaFrame-Token", body)
        self.assertIn('id="toggle"', body)
        self.assertNotIn("{{", body)
        self.assertTrue(response.content_type.startswith("text/html"))

    def test_rendered_script_has_no_string_literal_broken_by_a_newline(self) -> None:
        # A template escape survives as JS source: a real newline inside quotes
        # would reach the browser as "Invalid or unexpected token".
        body = self.routes().handle(self.get("/")).body.decode("utf-8")
        script = body.split("<script>", 1)[1].split("</script>", 1)[0]
        self.assertNotIn('"\n"', script)

    def test_page_requires_the_token(self) -> None:
        self.assertEqual(self.routes().handle(self.get("/", token=None)).status, 401)

    def test_page_renders_the_pairing_controls(self) -> None:
        body = self.routes().handle(self.get("/")).body.decode("utf-8")
        self.assertIn('id="pair"', body)
        self.assertIn('id="pair-stop"', body)
        self.assertIn('id="pairing-status"', body)
        self.assertIn("/api/pair/repair", body)
        self.assertIn("/api/pair/forget", body)
        self.assertIn("renderPairing", body)

    def test_wrong_method_is_reported_with_allow(self) -> None:
        response = self.routes().handle(self.get("/api/state", method="POST"))
        self.assertEqual(response.status, 405)
        self.assertIn(("Allow", "GET"), response.headers)

    def test_unknown_path_is_not_found(self) -> None:
        self.assertEqual(self.routes().handle(self.get("/api/unknown")).status, 404)

    def test_toggle_and_refresh_are_accepted(self) -> None:
        service = FakeService([FakeSession("ready")])
        routes = self.routes(service=service)
        toggle = routes.handle(self.get("/api/screen/toggle", method="POST"))
        refresh = routes.handle(self.get("/api/refresh", method="POST"))
        self.assertEqual(toggle.status, 202)
        self.assertEqual(refresh.status, 202)
        self.assertEqual(service.toggles, 1)
        self.assertEqual(service.refreshes, 1)

    def test_page_command_reads_direction_from_body_or_query(self) -> None:
        service = FakeService([FakeSession("ready")])
        routes = self.routes(service=service)
        body = json.dumps({"direction": "next"}).encode()
        accepted = routes.handle(self.get("/api/screen/page", method="POST", body=body))
        previous = routes.handle(
            self.get("/api/screen/page?direction=previous", method="POST")
        )
        self.assertEqual((accepted.status, previous.status), (202, 202))
        self.assertEqual(service.pages, [1, -1])

    def test_alert_command_reads_mode_from_body_or_query(self) -> None:
        service = FakeService([
            FakeSession("ready", capabilities=frozenset({ALERT_SET_CAPABILITY}))
        ])
        routes = self.routes(service=service)
        body = json.dumps({"mode": 2}).encode()
        accepted = routes.handle(self.get("/api/alert", method="POST", body=body))
        from_query = routes.handle(self.get("/api/alert?mode=0", method="POST"))
        self.assertEqual((accepted.status, from_query.status), (202, 202))
        self.assertEqual(json.loads(accepted.body)["alert"], 2)
        self.assertEqual(service.alerts, [2, 0])

    def test_alert_command_rejects_a_missing_or_invalid_mode(self) -> None:
        routes = self.routes()
        self.assertEqual(routes.handle(self.get("/api/alert", method="POST")).status, 400)
        for payload in ({"mode": 3}, {"mode": -1}, {"mode": "9"}, {"mode": True},
                        {"mode": 2.5}, {"mode": []}):
            with self.subTest(payload=payload):
                response = routes.handle(
                    self.get("/api/alert", method="POST", body=json.dumps(payload).encode())
                )
                self.assertEqual((response.status, json.loads(response.body)),
                                 (400, {"error": "invalid_mode"}))
        self.assertEqual(
            self.routes().handle(self.get("/api/alert", method="GET")).status, 405
        )

    def test_pair_start_is_post_only_and_accepted(self) -> None:
        routes = self.routes()
        self.assertEqual(routes.handle(self.get("/api/pair", method="POST")).status, 202)
        refused = routes.handle(self.get("/api/pair"))
        self.assertEqual(refused.status, 405)
        self.assertIn(("Allow", "POST"), refused.headers)

    def test_pair_stop_is_post_only_and_accepted(self) -> None:
        routes = self.routes()
        self.assertEqual(
            routes.handle(self.get("/api/pair/stop", method="POST")).status, 202
        )
        self.assertEqual(
            json.loads(routes.handle(self.get("/api/pair/stop", method="POST")).body),
            {"stopped": False, "reason": "idle"},
        )
        refused = routes.handle(self.get("/api/pair/stop"))
        self.assertEqual(refused.status, 405)
        self.assertIn(("Allow", "POST"), refused.headers)

    def test_repair_and_forget_read_the_address_from_body_or_query(self) -> None:
        routes = self.routes()
        body = json.dumps({"address": "AA:BB"}).encode()
        repair = routes.handle(self.get("/api/pair/repair", method="POST", body=body))
        forget = routes.handle(self.get("/api/pair/forget?address=AA:BB", method="POST"))
        self.assertEqual((repair.status, forget.status), (202, 202))
        for response in (repair, forget):
            self.assertEqual(
                json.loads(response.body)["devices"][0]["reason"], "unsupported"
            )

    def test_a_pairing_target_rejects_a_missing_or_invalid_address(self) -> None:
        routes = self.routes()
        missing = routes.handle(self.get("/api/pair/repair", method="POST"))
        self.assertEqual(
            (missing.status, json.loads(missing.body)["error"]),
            (400, "address_required"),
        )
        for payload in ({"address": ""}, {"address": 42}, {"address": " "},
                        {"address": "A" * 65}, {"address": ["AA:BB"]}):
            with self.subTest(payload=payload):
                response = routes.handle(
                    self.get(
                        "/api/pair/forget", method="POST", body=json.dumps(payload).encode()
                    )
                )
                self.assertEqual(
                    (response.status, json.loads(response.body)["error"]),
                    (400, "invalid_address"),
                )
        broken = routes.handle(self.get("/api/pair/forget", method="POST", body=b"{"))
        self.assertEqual((broken.status, json.loads(broken.body)["error"]),
                         (400, "invalid_json"))

    def test_page_command_rejects_a_missing_or_unknown_direction(self) -> None:
        routes = self.routes()
        missing = routes.handle(self.get("/api/screen/page", method="POST"))
        unknown = routes.handle(
            self.get("/api/screen/page", method="POST", body=b'{"direction":"sideways"}')
        )
        broken = routes.handle(self.get("/api/screen/page", method="POST", body=b"{"))
        self.assertEqual((missing.status, unknown.status, broken.status), (400, 400, 400))
        self.assertEqual(json.loads(broken.body)["error"], "invalid_json")
        self.assertEqual(json.loads(missing.body)["error"], "direction_required")

    def test_favicon_answers_without_a_body(self) -> None:
        response = self.routes().handle(self.get("/favicon.ico"))
        self.assertEqual((response.status, response.body), (204, b""))


class ServerTests(unittest.TestCase):
    def serve(self, control=None) -> ControlServer:
        backend = control or ServiceControl(FakeService([FakeSession("ready")]), version="1.2.1")
        return ControlServer(backend, token=TOKEN, host="127.0.0.1", port=0)

    async def exchange(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        raw: bytes,
    ) -> tuple[int, dict[str, str], bytes]:
        writer.write(raw)
        await writer.drain()
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 5)
        status = int(head.split(b" ", 2)[1])
        headers: dict[str, str] = {}
        for line in head.decode("iso-8859-1").split("\r\n")[1:]:
            if not line:
                continue
            name, _, value = line.partition(":")
            headers[name.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        body = await reader.readexactly(length) if length else b""
        return status, headers, body

    async def one(self, server: ControlServer, raw: bytes) -> tuple[int, dict[str, str], bytes]:
        reader, writer = await asyncio.open_connection("127.0.0.1", server.port)
        try:
            return await self.exchange(reader, writer, raw)
        finally:
            writer.close()
            await writer.wait_closed()

    def request(self, server: ControlServer, path: str, extra: str = "") -> bytes:
        return (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{server.port}\r\n"
            f"X-QuotaFrame-Token: {TOKEN}\r\n"
            f"{extra}\r\n"
        ).encode()

    def run_server(self, body):
        async def scenario():
            server = self.serve()
            await server.start()
            try:
                return await body(server)
            finally:
                await server.close()

        return asyncio.run(scenario())

    def test_state_round_trip(self) -> None:
        async def body(server: ControlServer):
            return await self.one(server, self.request(server, "/api/state"))

        status, headers, payload = self.run_server(body)

        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "application/json; charset=utf-8")
        self.assertEqual(headers["cache-control"], "no-store")
        self.assertEqual(json.loads(payload)["bridge"]["version"], "1.2.1")

    def test_two_requests_share_one_connection(self) -> None:
        async def body(server: ControlServer):
            reader, writer = await asyncio.open_connection("127.0.0.1", server.port)
            try:
                first = await self.exchange(reader, writer, self.request(server, "/api/state"))
                second = await self.exchange(reader, writer, self.request(server, "/api/refresh"))
                return first, second
            finally:
                writer.close()
                await writer.wait_closed()

        first, second = self.run_server(body)

        self.assertEqual(first[0], 200)
        self.assertIn("keep-alive", first[1]["connection"])
        # A GET on a POST-only route is a method error, not a second connection.
        self.assertEqual(second[0], 405)

    def test_connection_close_is_honoured(self) -> None:
        async def body(server: ControlServer):
            return await self.one(
                server, self.request(server, "/api/state", "Connection: close\r\n")
            )

        _status, headers, _payload = self.run_server(body)

        self.assertEqual(headers["connection"], "close")

    def test_page_is_served_with_the_token(self) -> None:
        async def body(server: ControlServer):
            return await self.one(server, self.request(server, "/"))

        status, headers, payload = self.run_server(body)

        self.assertEqual(status, 200)
        self.assertTrue(headers["content-type"].startswith("text/html"))
        self.assertIn(TOKEN.encode(), payload)

    def test_malformed_request_line_is_rejected(self) -> None:
        async def body(server: ControlServer):
            return await self.one(server, b"NONSENSE\r\n\r\n")

        status, _headers, payload = self.run_server(body)

        self.assertEqual(status, 400)
        self.assertEqual(json.loads(payload)["error"], "malformed_request_line")

    def test_oversized_body_is_rejected_before_it_is_read(self) -> None:
        async def body(server: ControlServer):
            return await self.one(
                server,
                (
                    "POST /api/refresh HTTP/1.1\r\n"
                    f"Host: 127.0.0.1:{server.port}\r\n"
                    f"X-QuotaFrame-Token: {TOKEN}\r\n"
                    f"Content-Length: {MAX_BODY_BYTES + 1}\r\n\r\n"
                ).encode(),
            )

        status, _headers, payload = self.run_server(body)

        self.assertEqual(status, 413)
        self.assertEqual(json.loads(payload)["error"], "body_too_large")

    def test_chunked_bodies_are_refused(self) -> None:
        async def body(server: ControlServer):
            return await self.one(
                server,
                (
                    "POST /api/refresh HTTP/1.1\r\n"
                    f"Host: 127.0.0.1:{server.port}\r\n"
                    f"X-QuotaFrame-Token: {TOKEN}\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n"
                ).encode(),
            )

        status, _headers, payload = self.run_server(body)

        self.assertEqual(status, 400)
        self.assertEqual(json.loads(payload)["error"], "chunked_body_unsupported")

    def test_a_request_without_a_token_is_unauthorised(self) -> None:
        async def body(server: ControlServer):
            return await self.one(
                server,
                (
                    "GET /api/state HTTP/1.1\r\n"
                    f"Host: 127.0.0.1:{server.port}\r\n\r\n"
                ).encode(),
            )

        status, _headers, payload = self.run_server(body)

        self.assertEqual(status, 401)
        self.assertEqual(json.loads(payload)["error"], "token_required")

    def test_start_reports_the_bound_port_and_url(self) -> None:
        async def body(server: ControlServer):
            return server.port, server.url

        port, url = self.run_server(body)

        self.assertGreater(port, 0)
        self.assertEqual(url, f"http://127.0.0.1:{port}/?token={TOKEN}")

    def test_a_busy_port_leaves_the_bridge_running(self) -> None:
        async def body(server: ControlServer):
            second = ControlServer(ServiceControl(FakeService([]), version="1"), token=TOKEN, port=server.port)
            await second.__aenter__()
            try:
                return second.running
            finally:
                await second.close()

        self.assertFalse(self.run_server(body))


class CliWiringTests(unittest.TestCase):
    """`--http-port` decides whether the page exists at all."""

    def test_port_zero_disables_the_page(self) -> None:
        args = SimpleNamespace(http_port=0, http_host="127.0.0.1")
        self.assertIsNone(_control_server(args, FakeService([]), None))

    def test_a_port_builds_a_server_on_the_configured_address(self) -> None:
        args = SimpleNamespace(http_port=8790, http_host="127.0.0.1")
        with patch(
            "quotaframe_bridge.cli.main.load_or_create_token", return_value="t" * 24
        ):
            server = _control_server(args, FakeService([]), None)
        self.assertEqual(server.url, "http://127.0.0.1:8790/?token=" + "t" * 24)
        self.assertFalse(server.running)


class TokenTests(unittest.TestCase):
    def test_token_is_persisted_once_and_reused(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "control.token"
            first = load_or_create_token(path)
            second = load_or_create_token(path)
            self.assertEqual(first, second)
            self.assertGreaterEqual(len(first), 16)
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_truncated_file_is_replaced(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "control.token"
            path.write_text("short\n", encoding="utf-8")
            token = load_or_create_token(path)
            self.assertGreaterEqual(len(token), 16)
            self.assertNotEqual(token, "short")

    def test_unwritable_directory_still_yields_a_usable_token(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            # A regular file where the directory should be makes the write fail
            # the way a read-only or missing location would.
            blocker = Path(root) / "blocker"
            blocker.write_text("x", encoding="utf-8")
            token = load_or_create_token(blocker / "control.token")
            self.assertGreaterEqual(len(token), 16)
            self.assertFalse((blocker / "control.token").exists())

    def test_token_path_follows_the_platform_data_directory(self) -> None:
        self.assertEqual(
            token_path({"HOME": "/home/user"}, platform="darwin"),
            Path("/home/user/Library/Application Support/quotaframe/control.token"),
        )
        self.assertEqual(
            token_path({"LOCALAPPDATA": "C:/local"}, platform="win32"),
            Path("C:/local/quotaframe/control.token"),
        )


if __name__ == "__main__":
    unittest.main()
