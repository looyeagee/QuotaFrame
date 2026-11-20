"""Collect several usage sources as one panel publication."""

from __future__ import annotations

import asyncio
from collections.abc import Sequence

from quotaframe_bridge.domain.models import Provider, ProviderUsage
from quotaframe_bridge.sources.base import UsageSource


class CombinedUsageSource:
    """Run every configured source and merge their provider results.

    A source that raises contributes nothing, so the remaining services still
    reach the panel; the service layer turns the missing providers into
    ``unavailable`` publications.
    """

    def __init__(self, sources: Sequence[UsageSource]) -> None:
        if not sources:
            raise ValueError("at least one usage source is required")
        self._sources = tuple(sources)
        self.providers = tuple(
            provider
            for source in self._sources
            for provider in getattr(source, "providers", ())
        )
        self.label = " and ".join(getattr(source, "label", "a source") for source in self._sources)

    async def collect_progressively(
        self,
        attempted_at: int | None,
        *,
        on_update,
    ) -> dict[Provider, ProviderUsage]:
        """Report each provider as soon as its own source answers."""

        merged: dict[Provider, ProviderUsage] = {}
        lock = asyncio.Lock()

        async def run(source) -> None:
            try:
                collected = await source.collect(attempted_at)
            except Exception:
                return
            for usage in collected.values():
                async with lock:
                    merged[usage.provider] = usage
                on_update(usage)

        await asyncio.gather(*(run(source) for source in self._sources))
        return dict(merged)

    async def collect(
        self, attempted_at: int | None = None
    ) -> dict[Provider, ProviderUsage]:
        results = await asyncio.gather(
            *(source.collect(attempted_at) for source in self._sources),
            return_exceptions=True,
        )
        merged: dict[Provider, ProviderUsage] = {}
        for result in results:
            if isinstance(result, BaseException):
                continue
            merged.update(result)
        return merged


__all__ = ["CombinedUsageSource"]
