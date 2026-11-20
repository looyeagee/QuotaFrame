"""DeepSeek account balance source.

DeepSeek publishes no usage or rate-window API; the only billing endpoint is
``GET /user/balance`` (https://api-docs.deepseek.com/api/get-user-balance), which
returns the remaining credit per currency. The panel therefore shows the balance
itself instead of a percentage.
"""

from __future__ import annotations

import asyncio
import json
import time
import urllib.error
import urllib.request
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation

from quotaframe_bridge.domain.models import Provider, ProviderUsage, SourceState

DEFAULT_API_BASE = "https://api.deepseek.com"
DEFAULT_CURRENCY = "CNY"
DEFAULT_TIMEOUT_SECONDS = 10.0
MAX_RESPONSE_BYTES = 64 * 1024


class BalanceFormatError(RuntimeError):
    """The balance response does not carry a usable amount for this account."""


@dataclass(frozen=True, slots=True)
class DeepSeekSettings:
    """Bridge-side configuration for the DeepSeek source."""

    api_key: str
    currency: str = DEFAULT_CURRENCY
    api_base: str = DEFAULT_API_BASE
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS

    def __post_init__(self) -> None:
        if not self.api_key or not self.api_key.strip():
            raise ValueError("DeepSeek API key must not be empty")


@dataclass(frozen=True, slots=True)
class BalanceReading:
    """Decoded ``/user/balance`` payload for the configured currency."""

    currency: str
    balance: Decimal
    available: bool


def parse_balance(payload: object, currency: str = DEFAULT_CURRENCY) -> BalanceReading:
    """Decode the balance payload, preferring the configured currency entry."""

    if not isinstance(payload, Mapping):
        raise BalanceFormatError("balance response is not an object")
    infos = payload.get("balance_infos")
    if not isinstance(infos, list):
        raise BalanceFormatError("balance response has no balance_infos array")

    chosen: Mapping[str, object] | None = None
    fallback: Mapping[str, object] | None = None
    for item in infos:
        if not isinstance(item, Mapping):
            continue
        entry_currency = item.get("currency")
        if not isinstance(entry_currency, str):
            continue
        if fallback is None:
            fallback = item
        if entry_currency.upper() == currency.upper():
            chosen = item
            break
    if chosen is None:
        chosen = fallback
    if chosen is None:
        raise BalanceFormatError("balance response has no usable currency entry")

    raw_total = chosen.get("total_balance")
    if not isinstance(raw_total, (str, int)):
        raise BalanceFormatError("balance entry has no total_balance")
    try:
        total = Decimal(str(raw_total))
    except InvalidOperation as exc:
        raise BalanceFormatError("total_balance is not a decimal") from exc
    if not total.is_finite() or total < 0:
        raise BalanceFormatError("total_balance is outside the supported range")

    code = str(chosen.get("currency") or currency).upper()
    if len(code) != 3 or not code.isalpha():
        raise BalanceFormatError("currency is not a three-letter code")
    return BalanceReading(currency=code, balance=total, available=bool(payload.get("is_available")))


def fetch_balance(
    settings: DeepSeekSettings,
    *,
    urlopen: Callable[..., object] = urllib.request.urlopen,
) -> BalanceReading:
    """Read ``/user/balance`` once; blocking, so callers run it in a thread."""

    request = urllib.request.Request(
        settings.api_base.rstrip("/") + "/user/balance",
        headers={
            "Accept": "application/json",
            "Authorization": f"Bearer {settings.api_key.strip()}",
        },
        method="GET",
    )
    try:
        with urlopen(request, timeout=settings.timeout_seconds) as response:
            raw = response.read(MAX_RESPONSE_BYTES)
    except urllib.error.HTTPError as exc:
        raise BalanceFormatError(f"balance request failed with HTTP {exc.code}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise BalanceFormatError(f"balance request failed: {exc}") from exc

    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BalanceFormatError("balance response is not valid JSON") from exc
    return parse_balance(payload, settings.currency)


class DeepSeekUsageSource:
    """Publish the DeepSeek balance as the panel's primary service."""

    providers = (Provider.DEEPSEEK,)
    label = "the DeepSeek API"

    def __init__(
        self,
        settings: DeepSeekSettings,
        *,
        fetch: Callable[[], BalanceReading] | None = None,
    ) -> None:
        self.settings = settings
        self._fetch = fetch or (lambda: fetch_balance(settings))

    async def collect(
        self, attempted_at: int | None = None
    ) -> dict[Provider, ProviderUsage]:
        sampled_at = int(time.time()) if attempted_at is None else attempted_at
        try:
            reading = await asyncio.to_thread(self._fetch)
        except BalanceFormatError:
            return {
                Provider.DEEPSEEK: ProviderUsage(
                    provider=Provider.DEEPSEEK,
                    state=SourceState.UNAVAILABLE,
                    sampled_at=sampled_at,
                )
            }
        return {
            Provider.DEEPSEEK: ProviderUsage(
                provider=Provider.DEEPSEEK,
                state=SourceState.OK,
                sampled_at=sampled_at,
                balance=reading.balance,
                currency=reading.currency,
            )
        }


__all__ = [
    "BalanceFormatError",
    "BalanceReading",
    "DeepSeekSettings",
    "DeepSeekUsageSource",
    "fetch_balance",
    "parse_balance",
]
