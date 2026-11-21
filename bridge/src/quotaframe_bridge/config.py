"""Single reader for the user-local Bridge configuration file."""

from __future__ import annotations

import os
import sys
import tomllib
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from decimal import Decimal, InvalidOperation

from quotaframe_bridge.paths import (
    CONFIG_DIRECTORY,
    config_directory,
    data_directory,
)

CONFIG_FILENAME = "config.toml"
VALID_LOG_LEVELS = ("DEBUG", "INFO", "WARNING", "ERROR")
DEFAULT_LOG_LEVEL = "INFO"

__all__ = [
    "CONFIG_DIRECTORY",
    "CONFIG_FILENAME",
    "DEFAULT_LOG_LEVEL",
    "MINIMAX_API_KEY_ENV",
    "VALID_LOG_LEVELS",
    "config_path",
    "load_config",
    "read_deepseek_settings",
    "read_minimax_settings",
    "read_log_level",
    "read_language",
]


def config_path(
    environ: Mapping[str, str] | None = None,
    *,
    platform: str = sys.platform,
) -> Path | None:
    """Return the configuration file path, or None when its root is unset."""

    source = os.environ if environ is None else environ
    directory = config_directory(source, platform=platform)
    if directory is None:
        return None
    return directory / CONFIG_FILENAME


def load_config(
    environ: Mapping[str, str] | None = None,
    *,
    platform: str = sys.platform,
) -> Mapping[str, Any]:
    """Parse the configuration file, returning {} for any unreadable file."""

    path = config_path(environ, platform=platform)
    if path is None:
        return {}
    try:
        return tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError):
        return {}


def read_log_level(
    environ: Mapping[str, str] | None = None,
    *,
    platform: str = sys.platform,
) -> str:
    """Return a validated log level name, defaulting to INFO."""

    value = load_config(environ, platform=platform).get("log_level")
    if not isinstance(value, str):
        return DEFAULT_LOG_LEVEL
    candidate = value.strip().upper()
    if candidate not in VALID_LOG_LEVELS:
        return DEFAULT_LOG_LEVEL
    return candidate


def read_language(
    environ: Mapping[str, str] | None = None,
    *,
    platform: str = sys.platform,
) -> str:
    """Return auto, zh or en; missing and invalid settings follow the system."""

    value = load_config(environ, platform=platform).get("language")
    if not isinstance(value, str):
        return "auto"
    candidate = value.strip().lower()
    return candidate if candidate in {"auto", "zh", "en"} else "auto"


DEEPSEEK_API_KEY_ENV = "DEEPSEEK_API_KEY"


def read_deepseek_settings(
    environ: Mapping[str, str] | None = None,
    *,
    platform: str = sys.platform,
):
    """Return DeepSeek settings, or None when no API key is configured.

    The key comes from the ``[deepseek] api_key`` entry or the
    ``DEEPSEEK_API_KEY`` environment variable, which wins so a shell can override
    the file.
    """

    from quotaframe_bridge.sources.deepseek import DeepSeekSettings

    source = os.environ if environ is None else environ
    section = load_config(environ, platform=platform).get("deepseek")
    section = section if isinstance(section, Mapping) else {}

    key = section.get("api_key")
    api_key = key.strip() if isinstance(key, str) else ""
    api_key = source.get(DEEPSEEK_API_KEY_ENV, "").strip() or api_key
    if not api_key:
        return None

    currency = section.get("currency")
    currency = (
        currency.strip().upper()
        if isinstance(currency, str) and len(currency.strip()) == 3
        else "CNY"
    )
    return DeepSeekSettings(api_key=api_key, currency=currency)


MINIMAX_API_KEY_ENV = "MINIMAX_API_KEY"


def read_minimax_settings(
    environ: Mapping[str, str] | None = None,
    *,
    platform: str = sys.platform,
):
    """Return MiniMax settings, or None when no plan API key is configured.

    ``GET /v1/token_plan/remains`` authenticates with the coding plan's API key,
    so the Bridge forwards the ``[minimax] key`` string verbatim; ``api_key`` is
    accepted as an alias and ``MINIMAX_API_KEY`` wins so a shell can override the
    file.
    """

    from quotaframe_bridge.sources.minimax import MinimaxSettings

    source = os.environ if environ is None else environ
    section = load_config(environ, platform=platform).get("minimax")
    section = section if isinstance(section, Mapping) else {}

    key = ""
    for name in ("key", "api_key"):
        value = section.get(name)
        if isinstance(value, str) and value.strip():
            key = value.strip()
            break
    key = source.get(MINIMAX_API_KEY_ENV, "").strip() or key
    if not key:
        return None
    return MinimaxSettings(api_key=key)
