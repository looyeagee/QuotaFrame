"""Deterministic account-free usage source."""

from __future__ import annotations

import time
from decimal import Decimal

from quotaframe_bridge.domain.models import (
    Provider,
    ProviderUsage,
    SourceState,
    UsageWindow,
)


class MockUsageSource:
    """Emit changing but repeatable values and future reset times."""

    providers = (Provider.DEEPSEEK, Provider.MINIMAX)
    label = "the mock source"

    def __init__(self) -> None:
        self._cycle = 0

    async def collect(
        self, attempted_at: int | None = None
    ) -> dict[Provider, ProviderUsage]:
        now = int(time.time()) if attempted_at is None else attempted_at
        cycle = self._cycle
        self._cycle += 1
        return {
            Provider.DEEPSEEK: ProviderUsage(
                provider=Provider.DEEPSEEK,
                state=SourceState.OK,
                sampled_at=now,
                balance=Decimal("12.34") + Decimal(cycle % 7),
                currency="CNY",
            ),
            Provider.MINIMAX: ProviderUsage(
                provider=Provider.MINIMAX,
                state=SourceState.OK,
                sampled_at=now,
                short=UsageWindow((12 + 7 * cycle) % 101, now + 2 * 60 * 60),
                week=UsageWindow((34 + 3 * cycle) % 101, now + 4 * 24 * 60 * 60),
            ),
        }
