from __future__ import annotations

import json
import unittest

from quotaframe_bridge.domain.models import (
    LiveSession,
    Provider,
    ProviderUsage,
    SessionState,
    SessionsPublication,
    SourceState,
    UsageWindow,
    WaitingReason,
)
from quotaframe_bridge.protocol.lines import LineDecodeError, LineDecoder
from quotaframe_bridge.protocol.messages import (
    AckError,
    IncompatibleProtocolError,
    ProtocolError,
    Sequence,
    encode_alert_set,
    encode_brightness_set,
    encode_sessions,
    encode_status_query,
    encode_usage,
    parse_ack,
    parse_status,
)


def sample_usage(state: SourceState = SourceState.OK) -> ProviderUsage:
    if state is SourceState.UNAVAILABLE:
        return ProviderUsage(Provider.DEEPSEEK, state, 1_785_398_400)
    short = UsageWindow(36, 1_785_409_200)
    week = UsageWindow(61, 1_785_798_000) if state is SourceState.OK else None
    return ProviderUsage(
        Provider.DEEPSEEK,
        state,
        1_785_398_400,
        short=short,
        week=week,
    )


class UsageEncodingTests(unittest.TestCase):
    def test_usage_encoder_has_exact_allowlisted_keys(self) -> None:
        line = encode_usage(sample_usage(), seq=42, sent_at=1_785_398_402)

        payload = json.loads(line)
        self.assertEqual(
            set(payload),
            {
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
            },
        )
        self.assertTrue(line.endswith(b"\n"))
        self.assertNotIn(b" ", line)
        for key in (
            "v",
            "seq",
            "sampled_at",
            "sent_at",
            "short_used_pct",
            "short_reset_at",
            "week_used_pct",
            "week_reset_at",
        ):
            with self.subTest(key=key):
                self.assertIsInstance(payload[key], str)
                self.assertTrue(payload[key].isdigit())

    def test_partial_and_unavailable_omit_absent_fields(self) -> None:
        partial = json.loads(
            encode_usage(sample_usage(SourceState.PARTIAL), 1, 1_785_398_402)
        )
        unavailable = json.loads(
            encode_usage(sample_usage(SourceState.UNAVAILABLE), 2, 1_785_398_402)
        )

        self.assertIn("short_used_pct", partial)
        self.assertNotIn("week_used_pct", partial)
        self.assertEqual(
            set(unavailable),
            {"cmd", "v", "seq", "provider", "state", "sampled_at", "sent_at"},
        )

    def test_encoder_rejects_invalid_sequence_or_clock_inversion(self) -> None:
        with self.assertRaises(ProtocolError):
            encode_usage(sample_usage(), seq=-1, sent_at=1_785_398_402)
        with self.assertRaises(ProtocolError):
            encode_usage(sample_usage(), seq=1, sent_at=1_785_398_099)

    def test_sequence_wraps_as_unsigned_32_bit(self) -> None:
        sequence = Sequence(initial=0xFFFFFFFF)

        self.assertEqual(sequence.next(), 0xFFFFFFFF)
        self.assertEqual(sequence.next(), 0)

    def test_status_query_uses_buddy_command_shape(self) -> None:
        self.assertEqual(encode_status_query(), b'{"cmd":"status"}\n')


class LineDecoderTests(unittest.TestCase):
    def test_buffers_fragmented_notification(self) -> None:
        decoder = LineDecoder(max_line_bytes=1024)

        self.assertEqual(decoder.feed(b'{"ack":"usage",'), [])
        self.assertEqual(
            decoder.feed(b'"ok":true,"n":42}\n'),
            [{"ack": "usage", "ok": True, "n": 42}],
        )

    def test_decodes_multiple_lines_and_crlf(self) -> None:
        decoder = LineDecoder(max_line_bytes=1024)
        messages = decoder.feed(b'{"a":1}\r\n{"b":2}\n')

        self.assertEqual(messages, [{"a": 1}, {"b": 2}])

    def test_rejects_oversized_invalid_utf8_or_non_object_lines(self) -> None:
        with self.subTest("oversized"):
            decoder = LineDecoder(max_line_bytes=8)
            with self.assertRaises(LineDecodeError):
                decoder.feed(b"123456789")
        with self.subTest("utf8"):
            with self.assertRaises(LineDecodeError):
                LineDecoder(32).feed(b"\xff\n")
        with self.subTest("array"):
            with self.assertRaises(LineDecodeError):
                LineDecoder(32).feed(b"[]\n")


class ResponseValidationTests(unittest.TestCase):
    def test_capability_extensions_do_not_require_a_new_protocol_version(self) -> None:
        message = {"ack": "status", "n": 0, "ok": True, "data": {
            "name": "Panel", "sec": True, "protocol": 1,
            "page": "overview", "caps": ['usage.v1', 'screen.toggle.v1', 'future.v1'],
        }}
        self.assertIn("future.v1", parse_status(message).capabilities)
        for field, value in (("protocol", 2), ("caps", ["screen.toggle.v1"])):
            changed = json.loads(json.dumps(message))
            changed["data"][field] = value
            with self.subTest(field=field), self.assertRaises(IncompatibleProtocolError):
                parse_status(changed)
        message["data"]["protocol"] = True
        with self.assertRaises(ProtocolError) as caught:
            parse_status(message)
        self.assertNotIsInstance(caught.exception, IncompatibleProtocolError)

    def test_ack_requires_matching_command_sequence_and_success(self) -> None:
        parse_ack({"ack": "usage", "ok": True, "n": 42}, "usage", 42)

        for message in (
            {"ack": "other", "ok": True, "n": 42},
            {"ack": "usage", "ok": True, "n": 43},
            {"ack": "usage", "ok": True, "n": True},
            {"ack": "usage", "ok": False, "n": 42, "error": "queue_full"},
        ):
            with self.subTest(message=message), self.assertRaises(AckError):
                parse_ack(message, "usage", 42)

    def test_status_requires_secure_protocol_v1_and_parses_caps(self) -> None:
        status = parse_status(
            {
                "ack": "status", "n": 0,
                "ok": True,
                "data": {
                    "name": "M5StickS3",
                    "sec": True,
                    "protocol": 1,
                    "page": "overview",
                    "caps": ['usage.v1'],
                },
            }
        )

        self.assertEqual(status.name, "M5StickS3")
        self.assertEqual(status.page, "overview")
        self.assertEqual(status.capabilities, frozenset({"usage.v1"}))

    def test_status_rejects_insecure_or_incompatible_device(self) -> None:
        base = {
            "ack": "status", "n": 0,
            "ok": True,
            "data": {
                "name": "M5StickS3",
                "sec": True,
                "protocol": 1,
                "page": "overview",
                "caps": ['usage.v1'],
            },
        }
        for field, value in (
            ("sec", False),
            ("protocol", 2),
            ("protocol", True),
            ("page", "bad page"),
        ):
            message = json.loads(json.dumps(base))
            message["data"][field] = value
            with self.subTest(field=field), self.assertRaises(ProtocolError):
                parse_status(message)


class AlertCommandTests(unittest.TestCase):
    def test_alert_encoder_carries_the_mode_as_canonical_text(self) -> None:
        self.assertEqual(
            encode_alert_set(47, 2),
            b'{"cmd":"alert_set","mode":"2","seq":"47","v":"1"}\n',
        )
        for mode in (True, -1, 3, 2.0, "2"):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                encode_alert_set(47, mode)


class BrightnessCommandTests(unittest.TestCase):
    def test_brightness_encoder_carries_the_percent_as_canonical_text(self) -> None:
        self.assertEqual(
            encode_brightness_set(47, 80),
            b'{"cmd":"brightness_set","percent":"80","seq":"47","v":"1"}\n',
        )
        for percent in (True, -1, 101, 2.0, "80"):
            with self.subTest(percent=percent), self.assertRaises(ValueError):
                encode_brightness_set(47, percent)


class OpenStatusTests(unittest.TestCase):
    def parse(self, **fields):
        data = {"name": '书桌 "额度" \\ 屏', "sec": True, "protocol": 1, "caps": ["usage.v1"]}
        data.update(fields)
        return parse_status({"ack": "status", "ok": True, "n": 0, "data": data})

    def test_minimal_and_custom_page_status(self):
        self.assertEqual(self.parse().target, "")
        self.assertEqual(self.parse().page, "")
        self.assertEqual(self.parse(page="custom.dashboard").page, "custom.dashboard")
        self.assertIn("screen.page.v1", self.parse(caps=["usage.v1", "screen.page.v1"]).capabilities)

    def test_identity_and_capability_bounds(self):
        for fields in ({"name": " trailing "}, {"name": "x" * 65}, {"name": "中" * 43},
                       {"name": "x\u202ey"}, {"name": "x\u0085y"}, {"name": "\ud800"},
                       {"caps": ["usage.v1", "usage.v1"]}, {"caps": "usage.v1"},
                       {"target": ""}, {"firmware_project": "Invalid"}, {"page": None}):
            with self.subTest(fields=fields), self.assertRaises(ProtocolError):
                self.parse(**fields)

    def test_alert_mode_is_optional_and_bounded(self):
        self.assertIsNone(self.parse().alert)
        self.assertEqual(self.parse(caps=["usage.v1", "alert.set.v1"], alert=2).alert, 2)
        for value in (3, -1, True, "2", 2.0):
            with self.subTest(value=value), self.assertRaises(ProtocolError):
                self.parse(caps=["usage.v1", "alert.set.v1"], alert=value)

    def test_brightness_is_optional_and_bounded(self):
        self.assertIsNone(self.parse().brightness)
        self.assertEqual(self.parse(caps=["usage.v1", "brightness.set.v1"], brightness=80).brightness, 80)
        self.assertEqual(self.parse(caps=["usage.v1", "brightness.set.v1"], brightness=0).brightness, 0)
        self.assertEqual(self.parse(caps=["usage.v1", "brightness.set.v1"], brightness=100).brightness, 100)
        for value in (101, -1, True, "80", 80.0):
            with self.subTest(value=value), self.assertRaises(ProtocolError):
                self.parse(caps=["usage.v1", "brightness.set.v1"], brightness=value)

    def test_duplicate_decoded_keys_are_rejected_at_every_depth(self):
        for raw in (b'{"n":0,"n":1}\n', b'{"data":{"a":1,"\\u0061":2}}\n'):
            with self.assertRaises(LineDecodeError):
                LineDecoder(4096).feed(raw)

    def test_maximum_frame_with_fragmented_crlf(self):
        raw = b'{"x":"' + b'a' * (4096 - 8) + b'"}'
        decoder = LineDecoder(4096)
        self.assertEqual(decoder.feed(raw + b'\r'), [])
        self.assertEqual(len(decoder.feed(b'\n')), 1)


class SessionsEncodingTests(unittest.TestCase):
    def test_sessions_encoder_has_exact_allowlisted_keys(self) -> None:
        publication = SessionsPublication(
            live=(
                LiveSession("Panel session", SessionState.BUSY),
                LiveSession("Idle session", SessionState.IDLE),
            ),
            recent=("修复列表滚动的问题",),
        )
        line = encode_sessions(publication, seq=46)

        payload = json.loads(line)
        self.assertEqual(
            set(payload), {"cmd", "v", "seq", "live", "states", "recent"}
        )
        self.assertTrue(line.endswith(b"\n"))
        self.assertEqual(payload["live"], "Panel session\nIdle session")
        self.assertEqual(payload["states"], "busy\nidle")
        self.assertEqual(payload["recent"], "修复列表滚动的问题")

    def test_empty_lists_stay_off_the_wire(self) -> None:
        payload = json.loads(encode_sessions(SessionsPublication(), 7))

        self.assertEqual(set(payload), {"cmd", "v", "seq"})

    def test_long_names_are_cut_at_a_whole_character(self) -> None:
        name = "字" * 60
        publication = SessionsPublication(live=(LiveSession(name, SessionState.BUSY),))
        line = encode_sessions(publication, seq=1)

        payload = json.loads(line)
        self.assertLessEqual(len(payload["live"].encode("utf-8")), 96)

    def test_a_name_with_a_line_break_stays_one_entry(self) -> None:
        publication = SessionsPublication(
            live=(LiveSession("第一行\n第二行", SessionState.BUSY),),
            recent=("标题\n也是一行",),
        )
        line = encode_sessions(publication, seq=2)

        payload = json.loads(line)
        self.assertEqual(payload["live"], "第一行 第二行")
        self.assertEqual(payload["recent"], "标题 也是一行")
        self.assertEqual(len(payload["states"].split("\n")), 1)

    def test_the_extended_states_carry_the_waiting_reason(self) -> None:
        publication = SessionsPublication(
            live=(
                LiveSession("Waiting session", SessionState.WAITING, WaitingReason.ASK),
                LiveSession("Working session", SessionState.BUSY),
                LiveSession("Background session", SessionState.SHELL),
                LiveSession("Idle session", SessionState.IDLE),
            )
        )
        line = encode_sessions(publication, seq=48, extended=True)

        payload = json.loads(line)
        self.assertEqual(
            payload["states"], "waiting/ask\nbusy\nshell\nidle"
        )

    def test_a_waiting_session_without_a_reason_reads_as_a_dialog(self) -> None:
        publication = SessionsPublication(
            live=(LiveSession("Waiting session", SessionState.WAITING),)
        )
        line = encode_sessions(publication, seq=49, extended=True)

        payload = json.loads(line)
        self.assertEqual(payload["states"], "waiting/dialog")

    def test_a_device_without_the_extended_states_gets_two_states(self) -> None:
        publication = SessionsPublication(
            live=(
                LiveSession("Waiting session", SessionState.WAITING, WaitingReason.PERMISSION),
                LiveSession("Background session", SessionState.SHELL),
                LiveSession("Idle session", SessionState.IDLE),
            )
        )
        line = encode_sessions(publication, seq=50)

        payload = json.loads(line)
        # A turn that waits for its user has not ended, and a background shell
        # produces nothing: the two states a first panel knows say as much.
        self.assertEqual(payload["states"], "busy\nidle\nidle")


if __name__ == "__main__":
    unittest.main()


class StrictFrameTests(unittest.TestCase):
    def test_decoded_duplicate_keys_surrogates_and_negative_zero_fail(self):
        for line in (b'{"a":1,"\\u0061":2}', b'{"a":{"x":1,"x":2}}',
                     b'{"ignored":["\\ud800"]}', b'{"ack":"status","n":-0}'):
            with self.subTest(line=line), self.assertRaises(LineDecodeError):
                LineDecoder(4096).feed(line + b"\n")

    def test_full_frame_crlf_can_be_split_after_carriage_return(self):
        line = b'{"a":"' + b'x' * (4096 - 8) + b'"}'
        self.assertEqual(len(line), 4096)
        decoder = LineDecoder(4096)
        self.assertEqual(decoder.feed(line + b"\r"), [])
        self.assertEqual(len(decoder.feed(b"\n")[0]["a"]), 4088)
