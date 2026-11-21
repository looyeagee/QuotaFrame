"""Reading the system now-playing mirror through the vendored adapter."""

from __future__ import annotations

import json
import tempfile
import time
import unittest
from pathlib import Path

from quotaframe_bridge.sources.macos_nowplaying import (
    MediaRemoteStream,
    NowPlayingState,
    parse_payload,
    parse_stream_line,
    stream_command,
)

BUNDLE_ID = "com.netease.163music"


def payload(**overrides: object) -> dict[str, object]:
    """A payload shaped like `media-control get --micros` output."""

    document: dict[str, object] = {
        "bundleIdentifier": BUNDLE_ID,
        "processIdentifier": 90106,
        "title": "只要平凡",
        "artist": "张杰",
        "album": "只要平凡",
        "durationMicros": 246_028_117,
        "elapsedTimeMicros": 0,
        "timestampEpochMicros": 1_795_253_218_000_000,
        "playbackRate": 1,
        "playing": True,
    }
    document.update(overrides)
    return document


def stream_line(*, diff: bool = False, **overrides: object) -> str:
    return json.dumps(
        {"type": "data", "diff": diff, "payload": payload(**overrides)},
        ensure_ascii=False,
    )


def payload_directory(root: Path) -> Path:
    """Lay out the vendored files the adapter resolves relative to itself."""

    adapter = root / "lib" / "media-control"
    adapter.mkdir(parents=True)
    (adapter / "mediaremote-adapter.pl").write_text("", encoding="utf-8")
    (root / "Frameworks" / "MediaRemoteAdapter.framework").mkdir(parents=True)
    return root


class FakeProcess:
    """A child stand-in that yields prepared lines and then exits."""

    def __init__(self, lines: list[str] | None = None) -> None:
        self.stdout = iter(lines or [])
        self.stderr = None
        self.terminated = False
        self.killed = False

    def terminate(self) -> None:
        self.terminated = True

    def kill(self) -> None:
        self.killed = True

    def wait(self, timeout: float | None = None) -> int:
        return 0


def wait_for(predicate, timeout: float = 2.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.005)
    return predicate()


class ParsePayloadTest(unittest.TestCase):
    def test_micros_payload_becomes_seconds(self) -> None:
        state = parse_payload(payload(), received_at=1_795_253_220.0)
        assert state is not None
        self.assertEqual(state.bundle_id, BUNDLE_ID)
        self.assertEqual(state.process_id, 90106)
        self.assertEqual(state.title, "只要平凡")
        self.assertEqual(state.artist, "张杰")
        self.assertAlmostEqual(state.duration_seconds, 246.028117, places=4)
        self.assertAlmostEqual(state.elapsed_seconds, 0.0)
        self.assertEqual(state.anchor_epoch, 1_795_253_218.0)
        self.assertTrue(state.playing)
        self.assertEqual(state.received_at, 1_795_253_220.0)

    def test_seconds_payload_is_accepted(self) -> None:
        document = payload(
            duration=246.0,
            elapsedTime=12.5,
            timestamp="2026-11-21T09:26:16Z",
        )
        for key in ("durationMicros", "elapsedTimeMicros", "timestampEpochMicros"):
            del document[key]
        state = parse_payload(document, received_at=0.0)
        assert state is not None
        self.assertEqual(state.duration_seconds, 246.0)
        self.assertEqual(state.elapsed_seconds, 12.5)
        self.assertAlmostEqual(state.anchor_epoch, 1_795_253_176.0, delta=1.0)

    def test_unparsable_anchor_names_nothing(self) -> None:
        document = payload()
        del document["timestampEpochMicros"]
        document["timestamp"] = "not a timestamp"
        self.assertIsNone(parse_payload(document, received_at=0.0))

    def test_payload_without_a_track_names_nothing(self) -> None:
        self.assertIsNone(parse_payload({}, received_at=0.0))
        self.assertIsNone(parse_payload(payload(title=""), received_at=0.0))
        self.assertIsNone(parse_payload(payload(bundleIdentifier=""), received_at=0.0))


class PositionTest(unittest.TestCase):
    """The playhead follows the anchor the player published, not the wall clock."""

    def _state(self, **overrides: object) -> NowPlayingState:
        state = parse_payload(payload(**overrides), received_at=0.0)
        assert state is not None
        return state

    def test_steady_playback_advances_with_time(self) -> None:
        state = self._state()
        self.assertAlmostEqual(state.position_seconds(1_795_253_218.0), 0.0, places=3)
        self.assertAlmostEqual(state.position_seconds(1_795_253_223.0), 5.0, places=3)

    def test_pause_freezes_the_anchor_position(self) -> None:
        state = self._state(
            playbackRate=0, playing=False, elapsedTimeMicros=120_650_000,
        )
        position = state.position_seconds(1_795_253_400.0)
        self.assertAlmostEqual(position, 120.65, places=2)

    def test_playback_rate_scales_the_drift(self) -> None:
        state = self._state(playbackRate=1.5)
        self.assertAlmostEqual(state.position_seconds(1_795_253_220.0), 3.0, places=3)

    def test_position_stops_at_the_duration(self) -> None:
        state = self._state()
        self.assertAlmostEqual(
            state.position_seconds(1_795_253_900.0), 246.028117, places=3
        )


class ParseStreamLineTest(unittest.TestCase):
    def test_a_data_line_carries_the_payload(self) -> None:
        state = parse_stream_line(stream_line(), received_at=0.0)
        assert state is not None
        self.assertEqual(state.title, "只要平凡")

    def test_an_empty_payload_is_not_a_state(self) -> None:
        empty = json.dumps({"type": "data", "diff": False, "payload": {}})
        self.assertIsNone(parse_stream_line(empty, received_at=0.0))

    def test_a_diff_is_ignored_because_full_payloads_are_requested(self) -> None:
        self.assertIsNone(parse_stream_line(stream_line(diff=True), received_at=0.0))

    def test_noise_is_ignored(self) -> None:
        for text in ("", "   ", "not json", "[]", json.dumps({"type": "error"})):
            self.assertIsNone(parse_stream_line(text, received_at=0.0))


class StreamCommandTest(unittest.TestCase):
    def test_command_runs_under_the_apple_signed_interpreter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            command = stream_command(payload_directory(Path(directory)))
        assert command is not None
        self.assertEqual(command[0], "/usr/bin/perl")
        self.assertIn("stream", command)
        self.assertIn("--no-diff", command)
        self.assertIn("--micros", command)
        self.assertTrue(any(part.startswith("--debounce=") for part in command))

    def test_missing_payload_yields_no_command(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsNone(stream_command(Path(directory)))


class MediaRemoteStreamTest(unittest.TestCase):
    def test_lines_update_the_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = payload_directory(Path(directory))
            stream = MediaRemoteStream(
                directory=root, spawn=lambda *a, **k: FakeProcess([stream_line()])
            )
            stream.state()
            self.assertTrue(wait_for(lambda: stream._state is not None))
            state = stream.state()
            stream.stop()
        assert state is not None
        self.assertEqual(state.title, "只要平凡")

    def test_other_platforms_never_spawn(self) -> None:
        spawned: list[int] = []
        stream = MediaRemoteStream(
            platform="win32", spawn=lambda *a, **k: spawned.append(1)
        )
        self.assertIsNone(stream.state())
        self.assertEqual(spawned, [])
        stream.stop()

    def test_incomplete_payload_never_spawns(self) -> None:
        spawned: list[int] = []
        with tempfile.TemporaryDirectory() as directory:
            stream = MediaRemoteStream(
                directory=Path(directory), spawn=lambda *a, **k: spawned.append(1)
            )
            self.assertIsNone(stream.state())
            stream.stop()
        self.assertEqual(spawned, [])

    def test_a_spawn_failure_keeps_the_reader_quiet(self) -> None:
        def spawn(*args, **kwargs):
            raise OSError("no interpreter")

        with tempfile.TemporaryDirectory() as directory:
            root = payload_directory(Path(directory))
            stream = MediaRemoteStream(directory=root, spawn=spawn)
            self.assertIsNone(stream.state())
            self.assertIsNone(stream.state())
            stream.stop()

    def test_a_dead_stream_is_replaced(self) -> None:
        spawnd: list[FakeProcess] = []
        now = [1_000.0]

        def spawn(*args, **kwargs):
            process = FakeProcess([stream_line()])
            spawnd.append(process)
            return process

        with tempfile.TemporaryDirectory() as directory:
            root = payload_directory(Path(directory))
            stream = MediaRemoteStream(
                directory=root, spawn=spawn, clock=lambda: now[0]
            )
            stream.state()
            self.assertTrue(wait_for(lambda: len(spawnd) == 1))
            self.assertTrue(wait_for(lambda: stream._process is None))
            now[0] += 60.0
            stream.state()
            self.assertTrue(wait_for(lambda: len(spawnd) == 2))
            stream.stop()
        self.assertEqual(len(spawnd), 2)

    def test_stop_terminates_a_running_child(self) -> None:
        process = FakeProcess()
        stream = MediaRemoteStream(platform="win32", spawn=lambda *a, **k: process)
        stream._process = process  # a stream that is running but has said nothing
        stream.stop()
        self.assertTrue(process.terminated or process.killed)

    def test_a_stopped_stream_is_not_restarted(self) -> None:
        spawned: list[int] = []
        with tempfile.TemporaryDirectory() as directory:
            root = payload_directory(Path(directory))
            stream = MediaRemoteStream(
                directory=root, spawn=lambda *a, **k: spawned.append(1)
            )
            stream.stop()
            stream.state()
        self.assertEqual(spawned, [])

    def test_state_from_a_stopped_stream_goes_stale(self) -> None:
        now = [1_000.0]
        stream = MediaRemoteStream(platform="win32", clock=lambda: now[0])
        stream._state = parse_payload(payload(), received_at=now[0])
        self.assertIsNotNone(stream.state())
        now[0] += 60.0
        self.assertIsNone(stream.state())
        stream.stop()


if __name__ == "__main__":
    unittest.main()
