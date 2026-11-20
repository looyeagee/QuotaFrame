"""DeepSeek balance source tests."""

from __future__ import annotations

import unittest
from decimal import Decimal

from quotaframe_bridge.domain.models import Provider, SourceState
from quotaframe_bridge.sources.deepseek import (
    BalanceFormatError,
    BalanceReading,
    DeepSeekSettings,
    DeepSeekUsageSource,
    parse_balance,
)


class BalanceParsingTests(unittest.TestCase):
    def test_reads_configured_currency(self) -> None:
        payload = {
            "is_available": True,
            "balance_infos": [
                {"currency": "USD", "total_balance": "1.50"},
                {"currency": "CNY", "total_balance": "110.25"},
            ],
        }
        reading = parse_balance(payload, "CNY")
        self.assertEqual(reading.currency, "CNY")
        self.assertEqual(reading.balance, Decimal("110.25"))
        self.assertTrue(reading.available)

    def test_falls_back_to_first_entry(self) -> None:
        payload = {
            "is_available": False,
            "balance_infos": [{"currency": "USD", "total_balance": "0"}],
        }
        reading = parse_balance(payload, "CNY")
        self.assertEqual(reading.currency, "USD")
        self.assertFalse(reading.available)

    def test_rejects_payload_without_balance(self) -> None:
        for payload in (
            {},
            {"balance_infos": []},
            {"balance_infos": [{}]},
            {"balance_infos": [{"currency": "CNY"}]},
            {"balance_infos": [{"currency": "CNY", "total_balance": "many"}]},
            {"balance_infos": [{"currency": "CNY", "total_balance": "-1"}]},
            {"balance_infos": [{"currency": "CN", "total_balance": "1"}]},
        ):
            with self.subTest(payload=payload), self.assertRaises(BalanceFormatError):
                parse_balance(payload)


class SourceTests(unittest.IsolatedAsyncioTestCase):
    def _source(self, balance: str, currency: str = "CNY") -> DeepSeekUsageSource:
        settings = DeepSeekSettings(api_key="sk-test", currency=currency)
        return DeepSeekUsageSource(
            settings,
            fetch=lambda: BalanceReading(currency, Decimal(balance), True),
        )

    async def test_publishes_the_balance(self) -> None:
        usage = (await self._source("12.34").collect(attempted_at=1_000))[Provider.DEEPSEEK]
        self.assertEqual(usage.state, SourceState.OK)
        self.assertEqual(usage.balance, Decimal("12.34"))
        self.assertEqual(usage.currency, "CNY")
        self.assertIsNone(usage.short)
        self.assertIsNone(usage.week)
        self.assertEqual(usage.sampled_at, 1_000)

    async def test_fetch_failure_publishes_unavailable(self) -> None:
        def boom() -> BalanceReading:
            raise BalanceFormatError("offline")

        source = DeepSeekUsageSource(DeepSeekSettings(api_key="sk-test"), fetch=boom)
        usage = (await source.collect(attempted_at=42))[Provider.DEEPSEEK]
        self.assertEqual(usage.state, SourceState.UNAVAILABLE)
        self.assertEqual(usage.sampled_at, 42)
        self.assertIsNone(usage.balance)


if __name__ == "__main__":
    unittest.main()
