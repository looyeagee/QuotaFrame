"""The sessions publication reaches only devices that render it."""

from __future__ import annotations

import asyncio
import json
import unittest

from quotaframe_bridge.domain.models import (
    LiveSession,
    Provider,
    ProviderUsage,
    SessionState,
    SessionsPublication,
    SourceState,
    WaitingReason,
)
from quotaframe_bridge.protocol.messages import (
    MAX_LIVE_SESSIONS,
    MAX_LIVE_SESSIONS_ROSTER,
    SESSIONS_CAPABILITY,
    SESSIONS_ROSTER_CAPABILITY,
    SESSIONS_STATES_CAPABILITY,
    DeviceStatus,
    encode_sessions,
)
from quotaframe_bridge.service.devices import DeviceSession
from quotaframe_bridge.service.multi_device import MultiDeviceBridgeService

PUBLICATION = SessionsPublication(
    live=(
        LiveSession("Claude Code Desktop 会话状态页面", SessionState.BUSY),
        LiveSession("修复列表滚动的问题", SessionState.IDLE),
    ),
)


def _roster(count: int) -> SessionsPublication:
    """A publication with `count` running sessions."""

    return SessionsPublication(
        live=tuple(
            LiveSession(f"会话 {index}", SessionState.BUSY) for index in range(count)
        )
    )


class _StubUsageSource:
    """Minimal usage source so the service loop has its usual publication."""

    providers = (Provider.DEEPSEEK,)

    async def collect(self, attempted_at: int | None = None):
        return {
            Provider.DEEPSEEK: ProviderUsage(
                provider=Provider.DEEPSEEK,
                state=SourceState.OK,
                sampled_at=1_785_398_400,
            )
        }


class _StubSessionsSource:
    """Publishes `publications` in order, then repeats the last one."""

    def __init__(self, *publications: SessionsPublication) -> None:
        self.publications = publications or (PUBLICATION,)
        self.reads = 0
        self.changes = asyncio.Event()

    async def collect(self) -> SessionsPublication:
        index = min(self.reads, len(self.publications) - 1)
        self.reads += 1
        return self.publications[index]

    async def wait_for_change(self, timeout: float) -> None:
        # The stub reports a change as soon as the test provides one; the loop
        # is event-driven, so the first read happens without waiting.
        self.changes.set()
        await asyncio.sleep(min(timeout, 0.05))

    def close(self) -> None:
        pass


class _EmptySessionsSource(_StubSessionsSource):
    def __init__(self) -> None:
        super().__init__(SessionsPublication())


class _QuietSessionsSource(_StubSessionsSource):
    """Reports no change until the deadline, so only a link that came up wakes the loop."""

    async def wait_for_change(self, timeout: float) -> None:
        await asyncio.sleep(timeout)


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
    """The sessions page is fed on change, only to capable devices."""

    def test_the_capability_token_is_stable(self) -> None:
        self.assertEqual(SESSIONS_CAPABILITY, "sessions.v1")
        self.assertEqual(SESSIONS_STATES_CAPABILITY, "sessions.v2")
        self.assertEqual(SESSIONS_ROSTER_CAPABILITY, "sessions.v3")

    async def _run(
        self,
        capabilities: frozenset[str],
        source: object,
        *,
        until: int = 1,
        seconds: float = 1.0,
    ) -> _RecordingTransport:
        """Run the service until `until` sessions frames were sent, or for `seconds`."""

        transport = _RecordingTransport(capabilities)
        service = MultiDeviceBridgeService(
            _StubUsageSource(),
            (DeviceSession("Panel", transport),),
            interval=10.0,
            publish_interval=10.0,
            sessions_source=source,  # type: ignore[arg-type]
            sessions_interval=30.0,
        )
        task = asyncio.create_task(service.run())
        deadline = asyncio.get_running_loop().time() + seconds
        try:
            while asyncio.get_running_loop().time() < deadline:
                frames = [payload for payload, command, _ in transport.sent
                          if command == "sessions"]
                if len(frames) >= until:
                    break
                await asyncio.sleep(0.01)
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        return transport

    @staticmethod
    def _frames(transport: _RecordingTransport) -> list[dict[str, object]]:
        return [json.loads(payload.decode("utf-8")) for payload, command, _ in transport.sent
                if command == "sessions"]

    async def test_capable_device_receives_the_sessions(self) -> None:
        transport = await self._run(
            frozenset({"usage.v1", SESSIONS_CAPABILITY}), _StubSessionsSource()
        )

        frames = self._frames(transport)
        self.assertTrue(frames)
        self.assertEqual(frames[0]["live"], "Claude Code Desktop 会话状态页面\n修复列表滚动的问题")
        self.assertEqual(frames[0]["states"], "busy\nidle")
        # The panels draw the roster and nothing else, so no transcript titles travel.
        self.assertNotIn("recent", frames[0])

    async def test_device_without_the_capability_is_left_alone(self) -> None:
        transport = await self._run(
            frozenset({"usage.v1"}), _StubSessionsSource(), until=99, seconds=0.3
        )
        self.assertEqual(self._frames(transport), [])

    async def test_an_empty_snapshot_still_publishes_the_summary_frame(self) -> None:
        transport = await self._run(
            frozenset({"usage.v1", SESSIONS_CAPABILITY}), _EmptySessionsSource()
        )

        frames = self._frames(transport)
        self.assertTrue(frames)
        self.assertEqual(set(frames[0]), {"cmd", "v", "seq"})

    async def test_an_unchanged_roster_is_not_sent_again(self) -> None:
        source = _StubSessionsSource()
        transport = await self._run(
            frozenset({"usage.v1", SESSIONS_CAPABILITY}), source, until=99, seconds=0.5
        )

        # The loop wakes many times over half a second; the roster changed never.
        self.assertGreater(source.reads, 2)
        self.assertEqual(len(self._frames(transport)), 1)

    async def test_a_change_reaches_the_device_without_the_cadence(self) -> None:
        # The safety interval is 30 s: anything that arrives inside the wait was
        # triggered by the change, not by the timer.
        changed = SessionsPublication(
            live=(LiveSession("修复列表滚动的问题", SessionState.BUSY),)
        )
        source = _StubSessionsSource(PUBLICATION, changed)
        transport = await self._run(
            frozenset({"usage.v1", SESSIONS_CAPABILITY}), source, until=2
        )

        frames = self._frames(transport)
        self.assertEqual(len(frames), 2)
        self.assertEqual(frames[1]["states"], "busy")

    async def test_a_device_with_the_extended_states_is_told_who_waits(self) -> None:
        source = _StubSessionsSource(
            SessionsPublication(
                live=(
                    LiveSession("等待回答", SessionState.WAITING, WaitingReason.ASK),
                    LiveSession("忙碌中", SessionState.BUSY),
                )
            )
        )
        transport = await self._run(
            frozenset({"usage.v1", SESSIONS_CAPABILITY, SESSIONS_STATES_CAPABILITY}),
            source,
        )

        frames = self._frames(transport)
        self.assertTrue(frames)
        self.assertEqual(frames[0]["states"], "waiting/ask\nbusy")

    async def test_a_device_without_them_gets_the_two_states(self) -> None:
        source = _StubSessionsSource(
            SessionsPublication(
                live=(
                    LiveSession("等待回答", SessionState.WAITING, WaitingReason.ASK),
                    LiveSession("空闲中", SessionState.IDLE),
                )
            )
        )
        transport = await self._run(
            frozenset({"usage.v1", SESSIONS_CAPABILITY}), source
        )

        frames = self._frames(transport)
        self.assertTrue(frames)
        self.assertEqual(frames[0]["states"], "busy\nidle")

    async def test_a_device_with_the_roster_receives_every_session(self) -> None:
        source = _StubSessionsSource(_roster(MAX_LIVE_SESSIONS_ROSTER))
        transport = await self._run(
            frozenset({"usage.v1", SESSIONS_CAPABILITY, SESSIONS_ROSTER_CAPABILITY}), source
        )

        frames = self._frames(transport)
        self.assertTrue(frames)
        self.assertEqual(len(frames[0]["live"].split("\n")), MAX_LIVE_SESSIONS_ROSTER)
        self.assertEqual(
            len(frames[0]["states"].split("\n")), MAX_LIVE_SESSIONS_ROSTER
        )

    async def test_a_device_without_the_roster_keeps_four_sessions(self) -> None:
        source = _StubSessionsSource(_roster(MAX_LIVE_SESSIONS_ROSTER))
        transport = await self._run(
            frozenset({"usage.v1", SESSIONS_CAPABILITY}), source
        )

        frames = self._frames(transport)
        self.assertTrue(frames)
        self.assertEqual(len(frames[0]["live"].split("\n")), MAX_LIVE_SESSIONS)

    def test_a_frame_that_does_not_fit_drops_its_oldest_sessions(self) -> None:
        # Names of quotes need escaping in the JSON, so a full roster does not
        # fit the frame the link carries.
        publication = SessionsPublication(
            live=tuple(
                LiveSession(f'"{index:02d}' + '"' * 94, SessionState.BUSY)
                for index in range(MAX_LIVE_SESSIONS_ROSTER)
            )
        )
        frame = encode_sessions(
            publication, 1, extended=True, limit=MAX_LIVE_SESSIONS_ROSTER
        )
        self.assertLessEqual(len(frame), 4096)
        payload = json.loads(frame.decode("utf-8"))
        names = payload["live"].split("\n")
        states = payload["states"].split("\n")
        self.assertEqual(len(names), len(states))
        self.assertLess(len(names), MAX_LIVE_SESSIONS_ROSTER)
        # The roster is ordered from the session touched last to the one touched
        # least, so the ones that leave the frame are the ones that moved least.
        self.assertTrue(names[0].startswith('"00'))
        self.assertTrue(names[-1].startswith(f'"{len(names) - 1:02d}'))

    async def test_the_roster_goes_out_again_on_a_new_link(self) -> None:
        transport = _RecordingTransport(frozenset({"usage.v1", SESSIONS_CAPABILITY}))
        session = DeviceSession("Panel", transport)
        service = MultiDeviceBridgeService(
            _StubUsageSource(),
            (session,),
            interval=10.0,
            publish_interval=10.0,
            sessions_source=_StubSessionsSource(),  # type: ignore[arg-type]
            sessions_interval=30.0,
        )
        task = asyncio.create_task(service.run())
        try:
            for _ in range(100):
                if self._frames(transport):
                    break
                await asyncio.sleep(0.01)
            # A board that rebooted has forgotten the roster, and its link came up again
            # with the sessions unchanged: the snapshot still has to go out.
            session._link_ready()
            for _ in range(100):
                if len(self._frames(transport)) == 2:
                    break
                await asyncio.sleep(0.01)
            self.assertEqual(len(self._frames(transport)), 2)
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)



    async def test_a_link_that_returns_with_a_usage_frame_gets_the_roster_again(self) -> None:
        # A panel that reboots drops the link and comes back on the snapshot the loop was
        # carrying, never through the idle second that declares a link ready. The roster did
        # not change meanwhile, so the new link still has to be given it.
        transport = _RecordingTransport(frozenset({"usage.v1", SESSIONS_CAPABILITY}))
        session = DeviceSession("Panel", transport)
        service = MultiDeviceBridgeService(
            _StubUsageSource(),
            (session,),
            interval=10.0,
            publish_interval=10.0,
            sessions_source=_QuietSessionsSource(),  # type: ignore[arg-type]
            sessions_interval=30.0,
        )
        task = asyncio.create_task(service.run())
        try:
            for _ in range(300):
                if self._frames(transport):
                    break
                await asyncio.sleep(0.01)
            self.assertEqual(len(self._frames(transport)), 1)

            transport.connected = False
            service._offer_current()
            for _ in range(300):
                if len(self._frames(transport)) == 2:
                    break
                await asyncio.sleep(0.01)
            self.assertEqual(len(self._frames(transport)), 2)
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)


if __name__ == "__main__":
    unittest.main()
