"""MiniMax quota source tests."""

from __future__ import annotations

import json
import unittest

from quotaframe_bridge.domain.models import Provider, SourceState, UsageWindow
from quotaframe_bridge.sources.combined import CombinedUsageSource
from quotaframe_bridge.sources.deepseek import DeepSeekSettings, DeepSeekUsageSource
from quotaframe_bridge.sources.minimax import (
    MinimaxFormatError,
    MinimaxSettings,
    MinimaxUsageSource,
    fetch_quota,
    parse_quota,
)

# Shape of /v1/token_plan/remains for one plan: the endpoint reports remaining
# percentages, so the panel's used value is the complement.
PLAN = {
    "model_name": "general",
    "start_time": 1795190400000,
    "end_time": 1795208400000,
    "remains_time": 14168776,
    "current_interval_status": 1,
    "current_interval_remaining_percent": 46,
    "weekly_start_time": 1794758400000,
    "weekly_end_time": 1795363200000,
    "weekly_remains_time": 168968776,
    "current_weekly_status": 1,
    "current_weekly_remaining_percent": 53,
    "weekly_boost_permille": 1500,
}
SUCCESS = {"base_resp": {"status_code": 0, "status_msg": "success"}}


def payload(*plans: object) -> dict[str, object]:
    return {"model_remains": list(plans), "base_resp": SUCCESS["base_resp"]}


class QuotaParsingTests(unittest.TestCase):
    def test_reads_both_windows_as_used_percentages(self) -> None:
        reading = parse_quota(payload(PLAN))
        self.assertEqual(reading.plan, "general")
        self.assertEqual(reading.short, UsageWindow(54, 1795208400))
        self.assertEqual(reading.week, UsageWindow(47, 1795363200))

    def test_weekly_window_is_not_scaled_by_the_boost(self) -> None:
        reading = parse_quota(payload(dict(PLAN, current_weekly_remaining_percent=59)))
        self.assertEqual(reading.week.used_percent, 41)

    def test_exhausted_interval_reads_as_fully_used(self) -> None:
        plan = dict(PLAN, current_interval_status=2, current_interval_remaining_percent=0)
        reading = parse_quota(payload(plan))
        self.assertEqual(reading.short.used_percent, 100)

    def test_prefers_the_general_plan(self) -> None:
        other = dict(PLAN, model_name="video", current_interval_remaining_percent=90)
        reading = parse_quota(payload(other, PLAN))
        self.assertEqual(reading.plan, "general")
        self.assertEqual(reading.short.used_percent, 54)

    def test_accepts_a_plan_list(self) -> None:
        other = dict(PLAN, model_name="abab", current_interval_remaining_percent=90)
        reading = parse_quota([other, PLAN])
        self.assertEqual(reading.plan, "general")
        self.assertEqual(reading.short.used_percent, 54)

    def test_percentages_clamp_and_require_numbers(self) -> None:
        reading = parse_quota(payload(dict(PLAN, current_interval_remaining_percent=-20)))
        self.assertEqual(reading.short.used_percent, 100)
        reading = parse_quota(payload(dict(PLAN, current_interval_remaining_percent=140)))
        self.assertEqual(reading.short.used_percent, 0)
        reading = parse_quota(payload(dict(PLAN, current_interval_remaining_percent="54%")))
        self.assertEqual(reading.short.used_percent, 46)
        for bad in (None, "", "abc", True, {}):
            with self.subTest(bad=bad), self.assertRaises(MinimaxFormatError):
                parse_quota(payload(dict(PLAN, current_interval_remaining_percent=bad)))

    def test_missing_timestamps_are_rejected(self) -> None:
        for field in ("end_time", "weekly_end_time"):
            plan = dict(PLAN)
            plan.pop(field)
            with self.subTest(field=field), self.assertRaises(MinimaxFormatError):
                parse_quota(payload(plan))

    def test_payload_without_a_plan_is_rejected(self) -> None:
        for bad in ({}, [], "text", payload()):
            with self.subTest(bad=bad), self.assertRaises(MinimaxFormatError):
                parse_quota(bad)

    def test_api_error_code_is_rejected(self) -> None:
        failed = {
            "model_remains": [],
            "base_resp": {"status_code": 1004, "status_msg": "invalid api key"},
        }
        with self.assertRaises(MinimaxFormatError) as caught:
            parse_quota(failed)
        self.assertIn("1004", str(caught.exception))


class FakeResponse:
    def __init__(self, body: bytes) -> None:
        self._body = body

    def read(self, _size: int) -> bytes:
        return self._body

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *exc_info: object) -> bool:
        return False


class FetchTests(unittest.TestCase):
    def test_sends_the_key_as_a_bearer_token(self) -> None:
        seen: dict[str, object] = {}

        def urlopen(request, timeout=None):
            seen["url"] = request.full_url
            seen["headers"] = dict(request.header_items())
            seen["method"] = request.get_method()
            return FakeResponse(json.dumps(payload(PLAN)).encode())

        reading = fetch_quota(MinimaxSettings(api_key="sk-cp-test"), urlopen=urlopen)
        self.assertEqual(reading.short.used_percent, 54)
        self.assertEqual(seen["url"], "https://www.minimaxi.com/v1/token_plan/remains")
        self.assertEqual(seen["method"], "GET")
        headers = {name.lower(): value for name, value in seen["headers"].items()}
        self.assertEqual(headers["authorization"], "Bearer sk-cp-test")

    def test_empty_key_is_rejected(self) -> None:
        for bad in ("", "   "):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                MinimaxSettings(api_key=bad)


class SourceTests(unittest.IsolatedAsyncioTestCase):
    async def test_publishes_windows(self) -> None:
        source = MinimaxUsageSource(
            MinimaxSettings(api_key="sk-cp-test"), fetch=lambda: parse_quota(payload(PLAN))
        )
        usage = (await source.collect(attempted_at=1_000))[Provider.MINIMAX]
        self.assertEqual(usage.state, SourceState.OK)
        self.assertEqual(usage.short, UsageWindow(54, 1795208400))
        self.assertEqual(usage.week, UsageWindow(47, 1795363200))
        self.assertIsNone(usage.balance)

    async def test_fetch_failure_publishes_unavailable(self) -> None:
        def boom():
            raise MinimaxFormatError("HTTP 401")

        source = MinimaxUsageSource(MinimaxSettings(api_key="sk-cp-test"), fetch=boom)
        usage = (await source.collect(attempted_at=7))[Provider.MINIMAX]
        self.assertEqual(usage.state, SourceState.UNAVAILABLE)
        self.assertEqual(usage.sampled_at, 7)


class CombinedTests(unittest.IsolatedAsyncioTestCase):
    async def test_merges_both_services(self) -> None:
        from decimal import Decimal

        from quotaframe_bridge.sources.deepseek import BalanceReading

        deepseek = DeepSeekUsageSource(
            DeepSeekSettings(api_key="sk-test"),
            fetch=lambda: BalanceReading("CNY", Decimal("1.00"), True),
        )
        minimax = MinimaxUsageSource(
            MinimaxSettings(api_key="sk-cp-test"), fetch=lambda: parse_quota(payload(PLAN))
        )
        source = CombinedUsageSource([deepseek, minimax])
        self.assertEqual(source.providers, (Provider.DEEPSEEK, Provider.MINIMAX))

        merged = await source.collect(attempted_at=5)
        self.assertEqual(set(merged), {Provider.DEEPSEEK, Provider.MINIMAX})
        self.assertEqual(merged[Provider.DEEPSEEK].balance, Decimal("1.00"))
        self.assertEqual(merged[Provider.MINIMAX].short.used_percent, 54)

    async def test_one_failing_source_does_not_drop_the_other(self) -> None:
        from decimal import Decimal

        from quotaframe_bridge.sources.deepseek import BalanceReading

        deepseek = DeepSeekUsageSource(
            DeepSeekSettings(api_key="sk-test"),
            fetch=lambda: BalanceReading("CNY", Decimal("2.00"), True),
        )

        class Broken:
            providers = (Provider.MINIMAX,)
            label = "broken"

            async def collect(self, attempted_at=None):
                raise RuntimeError("boom")

        merged = await CombinedUsageSource([deepseek, Broken()]).collect(attempted_at=5)
        self.assertEqual(set(merged), {Provider.DEEPSEEK})


if __name__ == "__main__":
    unittest.main()
