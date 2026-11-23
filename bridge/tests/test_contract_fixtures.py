from __future__ import annotations

import json
import unittest
from pathlib import Path

from quotaframe_bridge.protocol.lines import LineDecoder

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "protocol" / "examples"
ALLOWED_KEYS = {
    "cmd",
    "v",
    "seq",
    "provider",
    "state",
    "sampled_at",
    "sent_at",
    "short_used_pct",
    "short_reset_at",
    "week_used_pct",
    "week_reset_at",
}
SESSIONS_ALLOWED_KEYS = {"cmd", "v", "seq", "live", "states", "recent"}
SESSIONS_STATES = {
    "busy",
    "idle",
    "shell",
    "waiting/ask",
    "waiting/permission",
    "waiting/sandbox",
    "waiting/worker",
    "waiting/goal",
    "waiting/dialog",
}


class ContractFixtureTests(unittest.TestCase):
    def test_valid_usage_fixtures_decode_and_use_only_allowlisted_keys(self) -> None:
        paths = (
            EXAMPLES / "usage-ok.jsonl",
            EXAMPLES / "usage-partial.jsonl",
            EXAMPLES / "usage-unavailable.jsonl",
        )
        for path in paths:
            with self.subTest(path=path.name):
                messages = LineDecoder(1024).feed(path.read_bytes())
                self.assertEqual(len(messages), 1)
                payload = messages[0]
                self.assertEqual(payload["cmd"], "usage")
                self.assertEqual(payload["v"], "1")
                self.assertLessEqual(set(payload), ALLOWED_KEYS)
                for key, value in payload.items():
                    if key not in {"cmd", "provider", "state"}:
                        with self.subTest(path=path.name, key=key):
                            self.assertIsInstance(value, str)
                            self.assertTrue(value.isdigit())

    def test_invalid_fixtures_cover_range_type_and_orphan_reset(self) -> None:
        percent = json.loads((EXAMPLES / "usage-invalid.jsonl").read_text())
        numeric = json.loads(
            (EXAMPLES / "usage-invalid-numeric.jsonl").read_text()
        )
        orphan = json.loads(
            (EXAMPLES / "usage-invalid-orphan-reset.jsonl").read_text()
        )

        self.assertGreater(int(percent["short_used_pct"]), 100)
        self.assertIsInstance(numeric["short_used_pct"], int)
        self.assertIn("short_reset_at", orphan)
        self.assertNotIn("short_used_pct", orphan)


    def test_valid_sessions_fixture_decodes_and_uses_only_allowlisted_keys(self) -> None:
        messages = LineDecoder(1024).feed((EXAMPLES / "sessions.jsonl").read_bytes())
        self.assertEqual(len(messages), 1)
        payload = messages[0]
        self.assertEqual(payload["cmd"], "sessions")
        self.assertEqual(payload["v"], "1")
        self.assertLessEqual(set(payload), SESSIONS_ALLOWED_KEYS)
        self.assertEqual(len(payload["states"].split("\n")), len(payload["live"].split("\n")))
        for state in payload["states"].split("\n"):
            self.assertIn(state, SESSIONS_STATES)

    def test_invalid_sessions_fixture_carries_mismatched_lists(self) -> None:
        payload = json.loads((EXAMPLES / "sessions-invalid.jsonl").read_text())

        self.assertNotEqual(
            len(payload["states"].split("\n")), len(payload["live"].split("\n"))
        )

    def test_the_extended_states_fixture_uses_only_known_words(self) -> None:
        messages = LineDecoder(1024).feed((EXAMPLES / "sessions-v2.jsonl").read_bytes())
        self.assertEqual(len(messages), 1)
        payload = messages[0]
        self.assertEqual(payload["cmd"], "sessions")
        self.assertLessEqual(set(payload), SESSIONS_ALLOWED_KEYS)
        self.assertEqual(len(payload["states"].split("\n")), len(payload["live"].split("\n")))
        for state in payload["states"].split("\n"):
            self.assertIn(state, SESSIONS_STATES)

    def test_an_extended_state_fixture_with_an_unknown_reason_is_invalid(self) -> None:
        payload = json.loads((EXAMPLES / "sessions-invalid-v2.jsonl").read_text())

        states = payload["states"].split("\n")
        # One row is a state the device knows and one names a reason it does not, so
        # the frame is rejected as a whole rather than drawn with a state missing.
        self.assertEqual(len(states), len(payload["live"].split("\n")))
        self.assertTrue(any(state not in SESSIONS_STATES for state in states))

    def test_the_roster_fixture_carries_more_sessions_than_the_four_of_v1(self) -> None:
        messages = LineDecoder(1024).feed((EXAMPLES / "sessions-roster.jsonl").read_bytes())
        self.assertEqual(len(messages), 1)
        payload = messages[0]
        self.assertEqual(payload["cmd"], "sessions")
        self.assertLessEqual(set(payload), SESSIONS_ALLOWED_KEYS)
        live = payload["live"].split("\n")
        states = payload["states"].split("\n")
        self.assertEqual(len(live), len(states))
        self.assertGreater(len(live), 4)
        for state in states:
            self.assertIn(state, SESSIONS_STATES)


if __name__ == "__main__":
    unittest.main()
