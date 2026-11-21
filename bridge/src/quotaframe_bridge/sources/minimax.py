"""MiniMax quota-window source.

MiniMax publishes the coding plan's rate windows through
``GET /v1/token_plan/remains``: a five-hour interval and a weekly interval per
model. Both arrive as *remaining* percentages, so the panel value is the
complement (``100 - remaining``). The weekly window is normalised to 100 % and no
longer reports the 150 % boost that the older console endpoint carried. The
endpoint authenticates with the plan's API key, so the Bridge forwards exactly
the key the user configured as a bearer token.
"""

from __future__ import annotations

import asyncio
import json
import time
import urllib.error
import urllib.request
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation

from quotaframe_bridge.domain.models import (
    Provider,
    ProviderUsage,
    SourceState,
    UsageWindow,
)

DEFAULT_API_BASE = "https://www.minimaxi.com"
DEFAULT_TIMEOUT_SECONDS = 15.0
MAX_RESPONSE_BYTES = 64 * 1024
# The endpoint reports one plan per model; "general" is the account-wide default.
PREFERRED_PLAN = "general"


class MinimaxFormatError(RuntimeError):
    """The quota response does not carry the two windows this panel needs."""


@dataclass(frozen=True, slots=True)
class MinimaxSettings:
    """Bridge-side configuration for the MiniMax source."""

    api_key: str
    api_base: str = DEFAULT_API_BASE
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS

    def __post_init__(self) -> None:
        if not self.api_key or not self.api_key.strip():
            raise ValueError("MiniMax API key must not be empty")


@dataclass(frozen=True, slots=True)
class QuotaReading:
    """Decoded five-hour and weekly windows for one plan."""

    plan: str
    short: UsageWindow
    week: UsageWindow


def _percent(value: object, field: str) -> int:
    """Decode a percentage field into a whole number clamped to 0-100."""

    if isinstance(value, bool):
        raise MinimaxFormatError(f"{field} must be a percentage")
    if isinstance(value, (int, float)):
        number = Decimal(str(value))
    elif isinstance(value, str):
        text = value.strip().rstrip("%").strip()
        if not text:
            raise MinimaxFormatError(f"{field} is empty")
        try:
            number = Decimal(text)
        except InvalidOperation as exc:
            raise MinimaxFormatError(f"{field} is not a number") from exc
    else:
        raise MinimaxFormatError(f"{field} must be a percentage")
    if not number.is_finite():
        raise MinimaxFormatError(f"{field} is not finite")
    # A plan can report slightly beyond the normal range; the protocol caps at 100.
    return max(0, min(100, int(number.to_integral_value(rounding="ROUND_HALF_UP"))))


def _used_percent(plan: Mapping[str, object], field: str) -> int:
    """Convert the endpoint's remaining percentage into the panel's used value."""

    return 100 - _percent(plan.get(field), field)


def _epoch_seconds(milliseconds: object, field: str) -> int:
    if isinstance(milliseconds, bool) or not isinstance(milliseconds, (int, float)):
        raise MinimaxFormatError(f"{field} must be epoch milliseconds")
    seconds = int(milliseconds) // 1000
    if not 0 <= seconds <= 0xFFFFFFFF:
        raise MinimaxFormatError(f"{field} is outside the supported range")
    return seconds


def _check_status(payload: object) -> None:
    """Reject a transport-level success that carries an API error code."""

    if not isinstance(payload, Mapping):
        return
    response = payload.get("base_resp")
    if not isinstance(response, Mapping):
        return
    code = response.get("status_code")
    if isinstance(code, bool) or not isinstance(code, int) or code == 0:
        return
    message = response.get("status_msg")
    detail = message.strip() if isinstance(message, str) and message.strip() else "unknown error"
    raise MinimaxFormatError(f"quota request failed: {code} {detail}")


def _pick_plan(payload: object) -> Mapping[str, object]:
    if isinstance(payload, Mapping):
        # The endpoint wraps every plan in a single ``model_remains`` array.
        plans = payload.get("model_remains")
        if isinstance(plans, list):
            return _pick_plan(plans)
        return payload
    if isinstance(payload, Sequence) and not isinstance(payload, (str, bytes)):
        candidates = [item for item in payload if isinstance(item, Mapping)]
        for item in candidates:
            if item.get("model_name") == PREFERRED_PLAN:
                return item
        if candidates:
            return candidates[0]
    raise MinimaxFormatError("quota response has no plan object")


def parse_quota(payload: object) -> QuotaReading:
    """Decode one ``token_plan/remains`` payload into the two panel windows."""

    _check_status(payload)
    plan = _pick_plan(payload)
    return QuotaReading(
        plan=str(plan.get("model_name") or PREFERRED_PLAN),
        short=UsageWindow(
            used_percent=_used_percent(plan, "current_interval_remaining_percent"),
            reset_at=_epoch_seconds(plan.get("end_time"), "end_time"),
        ),
        week=UsageWindow(
            used_percent=_used_percent(plan, "current_weekly_remaining_percent"),
            reset_at=_epoch_seconds(plan.get("weekly_end_time"), "weekly_end_time"),
        ),
    )


def fetch_quota(
    settings: MinimaxSettings,
    *,
    urlopen: Callable[..., object] = urllib.request.urlopen,
) -> QuotaReading:
    """Read the quota endpoint once; blocking, so callers run it in a thread."""

    request = urllib.request.Request(
        settings.api_base.rstrip("/") + "/v1/token_plan/remains",
        headers={
            "Accept": "application/json, text/plain, */*",
            "Authorization": f"Bearer {settings.api_key.strip()}",
            "Content-Type": "application/json",
            "User-Agent": "QuotaFrame-Bridge/1.0",
        },
        method="GET",
    )
    try:
        with urlopen(request, timeout=settings.timeout_seconds) as response:
            raw = response.read(MAX_RESPONSE_BYTES)
    except urllib.error.HTTPError as exc:
        raise MinimaxFormatError(f"quota request failed with HTTP {exc.code}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise MinimaxFormatError(f"quota request failed: {exc}") from exc

    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise MinimaxFormatError("quota response is not valid JSON") from exc
    return parse_quota(payload)


class MinimaxUsageSource:
    """Publish MiniMax's five-hour and weekly windows."""

    providers = (Provider.MINIMAX,)
    label = "the MiniMax API"

    def __init__(
        self,
        settings: MinimaxSettings,
        *,
        fetch: Callable[[], QuotaReading] | None = None,
    ) -> None:
        self.settings = settings
        self._fetch = fetch or (lambda: fetch_quota(settings))

    async def collect(
        self, attempted_at: int | None = None
    ) -> dict[Provider, ProviderUsage]:
        sampled_at = int(time.time()) if attempted_at is None else attempted_at
        try:
            reading = await asyncio.to_thread(self._fetch)
        except MinimaxFormatError:
            return {
                Provider.MINIMAX: ProviderUsage(
                    provider=Provider.MINIMAX,
                    state=SourceState.UNAVAILABLE,
                    sampled_at=sampled_at,
                )
            }
        return {
            Provider.MINIMAX: ProviderUsage(
                provider=Provider.MINIMAX,
                state=SourceState.OK,
                sampled_at=sampled_at,
                short=reading.short,
                week=reading.week,
            )
        }


__all__ = [
    "MinimaxFormatError",
    "MinimaxSettings",
    "MinimaxUsageSource",
    "QuotaReading",
    "fetch_quota",
    "parse_quota",
]
