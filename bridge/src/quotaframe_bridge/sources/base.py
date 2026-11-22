"""Source adapter contract."""

from __future__ import annotations

from collections.abc import Callable
from typing import Protocol, runtime_checkable

from quotaframe_bridge.domain.models import (
    NowPlayingPublication,
    Provider,
    ProviderUsage,
    SessionsPublication,
)

UsageUpdate = Callable[[ProviderUsage], None]


class UsageSource(Protocol):
    """Collects one sanitized snapshot for each supported provider."""

    async def collect(self, attempted_at: int | None = None) -> dict[Provider, ProviderUsage]:
        """Return current sanitized usage."""


@runtime_checkable
class ProgressiveUsageSource(UsageSource, Protocol):
    """A source that can report a provider while the collection is pending."""

    async def collect_progressively(
        self, attempted_at: int | None, *, on_update: UsageUpdate,
    ) -> dict[Provider, ProviderUsage]:
        """Report early provider results and return the complete collection."""


@runtime_checkable
class NowPlayingSource(Protocol):
    """Collects the track a music client is playing."""

    async def collect(self) -> NowPlayingPublication | None:
        """Return the current song, or None when nothing is playing."""


@runtime_checkable
class SessionsSource(Protocol):
    """Collects the Claude Code session state of this computer."""

    async def collect(self) -> SessionsPublication:
        """Return the current sessions snapshot; the roster may be empty."""

    async def wait_for_change(self, timeout: float) -> None:
        """Return once the sessions may have changed, or after `timeout` seconds.

        A source that cannot observe the sessions blocks for `timeout`, so the
        caller always gets a bounded wait.
        """

    def close(self) -> None:
        """Release anything held for watching the sessions."""
