"""MiniMax quota source tests."""

from __future__ import annotations

import unittest

from quotaframe_bridge.domain.models import Provider, SourceState, UsageWindow
from quotaframe_bridge.sources.combined import CombinedUsageSource
from quotaframe_bridge.sources.deepseek import DeepSeekSettings, DeepSeekUsageSource
from quotaframe_bridge.sources.minimax import (
    MinimaxFormatError,
    MinimaxSettings,
    MinimaxUsageSource,
    parse_quota,
)

# Shape of /backend/account/token_plan/remains_percent for one plan.
PLAN = {
    "model_name": "general",
    "start_time": 1795190400000,
    "end_time": 1795208400000,
    "remains_time": 14168776,
    "current_interval_used_percent": "54%",
    "current_interval_total_percent": "100%",
    "current_interval_status": 1,
    "weekly_start_time": 1794758400000,
    "weekly_end_time": 1795363200000,
    "weekly_remains_time": 168968776,
    "current_weekly_used_percent": "47%",
    "current_weekly_total_percent": "150%",
    "current_weekly_status": 1,
}


class QuotaParsingTests(unittest.TestCase):
    def test_reads_both_windows(self) -> None:
        reading = parse_quota(PLAN)
        self.assertEqual(reading.plan, "general")
        self.assertEqual(reading.short, UsageWindow(54, 1795208400))
        self.assertEqual(reading.week, UsageWindow(47, 1795363200))

    def test_unwraps_the_console_response(self) -> None:
        other = dict(PLAN, model_name="video", current_interval_used_percent="10%")
        reading = parse_quota({"model_remains": [other, PLAN]})
        self.assertEqual(reading.plan, "general")
        self.assertEqual(reading.short.used_percent, 54)

    def test_accepts_a_plan_list(self) -> None:
        other = dict(PLAN, model_name="abab", current_interval_used_percent="10%")
        reading = parse_quota([other, PLAN])
        self.assertEqual(reading.plan, "general")
        self.assertEqual(reading.short.used_percent, 54)

    def test_percentages_clamp_and_require_numbers(self) -> None:
        self.assertEqual(parse_quota(dict(PLAN, current_interval_used_percent="120%")).short.used_percent, 100)
        self.assertEqual(parse_quota(dict(PLAN, current_interval_used_percent=54)).short.used_percent, 54)
        for bad in (None, "", "abc", True, {}):
            with self.subTest(bad=bad), self.assertRaises(MinimaxFormatError):
                parse_quota(dict(PLAN, current_interval_used_percent=bad))

    def test_missing_timestamps_are_rejected(self) -> None:
        for field in ("end_time", "weekly_end_time"):
            payload = dict(PLAN)
            payload.pop(field)
            with self.subTest(field=field), self.assertRaises(MinimaxFormatError):
                parse_quota(payload)

    def test_payload_without_a_plan_is_rejected(self) -> None:
        for bad in ({}, [], "text", {"model_remains": []}):
            with self.subTest(bad=bad), self.assertRaises(MinimaxFormatError):
                parse_quota(bad)


class SourceTests(unittest.IsolatedAsyncioTestCase):
    async def test_publishes_windows(self) -> None:
        source = MinimaxUsageSource(
            MinimaxSettings(cookie="_token=x"), fetch=lambda: parse_quota(PLAN)
        )
        usage = (await source.collect(attempted_at=1_000))[Provider.MINIMAX]
        self.assertEqual(usage.state, SourceState.OK)
        self.assertEqual(usage.short, UsageWindow(54, 1795208400))
        self.assertEqual(usage.week, UsageWindow(47, 1795363200))
        self.assertIsNone(usage.balance)

    async def test_fetch_failure_publishes_unavailable(self) -> None:
        def boom():
            raise MinimaxFormatError("HTTP 401")

        source = MinimaxUsageSource(MinimaxSettings(cookie="_token=x"), fetch=boom)
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
            MinimaxSettings(cookie="_token=x"), fetch=lambda: parse_quota(PLAN)
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
