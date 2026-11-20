"""Build the usage source a host is configured for."""

from __future__ import annotations

import sys
from collections.abc import Callable
from pathlib import Path

from quotaframe_bridge.config import read_deepseek_settings, read_minimax_settings
from quotaframe_bridge.sources.base import UsageSource
from quotaframe_bridge.sources.combined import CombinedUsageSource


class NoSourceConfiguredError(RuntimeError):
    """Neither a DeepSeek key nor a MiniMax cookie is configured."""

    def __init__(self) -> None:
        super().__init__(
            "no usage source is configured; set [deepseek] api_key or "
            "[minimax] cookie in config.toml"
        )


def create_usage_source(
    executable: Path | None = None,
    *,
    timeout: float | None = None,
    platform: str | None = None,
    dependency_status: Callable[[str], None] | None = None,
    deepseek_settings: object | None = None,
    minimax_settings: object | None = None,
) -> UsageSource:
    """Return a source covering every service this host can read.

    DeepSeek needs an API key and MiniMax needs a console cookie; whichever is
    configured takes part, and both are collected as one panel publication.
    """

    del executable, timeout, dependency_status  # The sources talk HTTP directly.

    if deepseek_settings is None:
        deepseek_settings = read_deepseek_settings()
    if minimax_settings is None:
        minimax_settings = read_minimax_settings()

    sources: list[UsageSource] = []
    if deepseek_settings is not None:
        from quotaframe_bridge.sources.deepseek import DeepSeekUsageSource

        sources.append(DeepSeekUsageSource(deepseek_settings))  # type: ignore[arg-type]
    if minimax_settings is not None:
        from quotaframe_bridge.sources.minimax import MinimaxUsageSource

        sources.append(MinimaxUsageSource(minimax_settings))  # type: ignore[arg-type]
    if not sources:
        raise NoSourceConfiguredError()
    if len(sources) == 1:
        return sources[0]
    return CombinedUsageSource(sources)
