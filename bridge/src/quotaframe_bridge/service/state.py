"""In-memory sanitized state store shared by CLI and UI adapters."""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from types import MappingProxyType

from quotaframe_bridge.domain.models import Provider, ProviderUsage, SourceState


class UsageStateStore:
    """Retain latest publish state and last valid provider values separately."""

    def __init__(self) -> None:
        self._providers: dict[Provider, ProviderUsage] = {}
        self._last_valid: dict[Provider, ProviderUsage] = {}
        self.last_collection_attempt: int | None = None
        # Providers in scope for this deployment. A provider outside this set is
        # never published, so a device is not asked to render a service this host
        # cannot supply. The default matches the historical two-provider bridge;
        # a source that declares its own set calls restrict_to().
        self._managed: set[Provider] = {Provider.DEEPSEEK, Provider.MINIMAX}

    @property
    def providers(self) -> Mapping[Provider, ProviderUsage]:
        return MappingProxyType(self._providers)

    @property
    def last_valid(self) -> Mapping[Provider, ProviderUsage]:
        return MappingProxyType(self._last_valid)

    def restrict_to(self, providers: Iterable[Provider]) -> None:
        """Limit publication to the providers the active source can supply."""

        self._managed = set(providers)

    def merge_collection(
        self,
        collection: Mapping[Provider, ProviderUsage],
        *,
        attempted_at: int,
        partial: bool = False,
    ) -> Mapping[Provider, ProviderUsage]:
        """Merge a batch, preserving untouched providers during partial updates."""

        self.last_collection_attempt = attempted_at
        self._managed.update(collection)
        for provider in sorted(self._managed, key=lambda item: item.value):
            usage = collection.get(provider)
            if usage is None:
                if partial and provider in self._providers:
                    continue
                usage = ProviderUsage(
                    provider=provider,
                    state=SourceState.UNAVAILABLE,
                    sampled_at=attempted_at,
                )
            if usage.state is not SourceState.UNAVAILABLE:
                self._last_valid[provider] = usage
            else:
                previous = self._last_valid.get(provider)
                if previous is not None:
                    # Preserve source age so retries cannot extend data freshness.
                    usage = previous
            self._providers[provider] = usage
        return self.providers

    def mark_all_unavailable(
        self, *, attempted_at: int
    ) -> Mapping[Provider, ProviderUsage]:
        unavailable = {
            provider: ProviderUsage(
                provider=provider,
                state=SourceState.UNAVAILABLE,
                sampled_at=attempted_at,
            )
            for provider in sorted(self._managed, key=lambda item: item.value)
        }
        return self.merge_collection(unavailable, attempted_at=attempted_at)
