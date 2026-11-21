"""The shared secret that authorises the control page.

One token per installation, stored beside the other Bridge state so a restart
does not invalidate a bookmarked page. The file is created readable only by its
owner, and the token is also logged at startup for the case where the file is
easier to read than the URL.
"""

from __future__ import annotations

import logging
import os
import secrets
from collections.abc import Mapping
from pathlib import Path

from quotaframe_bridge import paths

LOGGER = logging.getLogger(__name__)

TOKEN_BYTES = 24
TOKEN_FILE = "control.token"
# Anything shorter than this is a truncated or hand-edited file, not a token.
MINIMUM_TOKEN_LENGTH = 16


def token_path(
    environ: Mapping[str, str] | None = None,
    *,
    platform: str | None = None,
) -> Path:
    kwargs = {} if platform is None else {"platform": platform}
    current = os.environ if environ is None else environ
    return paths.data_directory(current, **kwargs) / TOKEN_FILE


def load_or_create_token(path: Path | None = None) -> str:
    """Return the stored token, generating and persisting one when absent.

    A file that cannot be written is not fatal: the token still works for this
    run, and the next start simply issues a new one.
    """

    target = token_path() if path is None else path
    try:
        existing = target.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        existing = ""
    except OSError as exc:
        LOGGER.warning("control token unreadable (%s); issuing a new one", type(exc).__name__)
        existing = ""
    if len(existing) >= MINIMUM_TOKEN_LENGTH:
        return existing
    token = secrets.token_urlsafe(TOKEN_BYTES)
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        handle = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            stream.write(token + "\n")
    except OSError as exc:
        LOGGER.warning(
            "control token not persisted to %s (%s)", target, type(exc).__name__
        )
    return token
