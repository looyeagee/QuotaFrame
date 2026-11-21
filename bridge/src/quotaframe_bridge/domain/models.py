"""Sanitized usage models shared by the Bridge's data sources."""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from enum import Enum


class Provider(str, Enum):
    """Services a panel renders, in display order."""

    DEEPSEEK = "deepseek"
    MINIMAX = "minimax"


class SourceState(str, Enum):
    """Whether a provider sample carries every value that provider publishes."""

    OK = "ok"
    PARTIAL = "partial"
    UNAVAILABLE = "unavailable"


@dataclass(frozen=True, slots=True)
class UsageWindow:
    """One normalized rate window."""

    used_percent: int
    reset_at: int | None = None

    def __post_init__(self) -> None:
        if not 0 <= self.used_percent <= 100:
            raise ValueError("used_percent must be between 0 and 100")
        if self.reset_at is not None and not 0 <= self.reset_at <= 0xFFFFFFFF:
            raise ValueError("reset_at must fit an unsigned 32-bit timestamp")


@dataclass(frozen=True, slots=True)
class NowPlayingPublication:
    """Sanitized now-playing state for a panel's music page.

    The panel shows the song title, its artists, the playhead and a window of
    lyric lines around the one being sung. Only display text is carried; the
    catalog identifier of the track stays on the computer.
    """

    title: str
    artist: str
    album: str
    position_seconds: int
    duration_seconds: int
    playing: bool
    lines: tuple[str, ...] = ()
    current_line: int = 0

    def __post_init__(self) -> None:
        if self.position_seconds < 0 or self.duration_seconds < 0:
            raise ValueError("now-playing times must not be negative")
        if self.lines and not 0 <= self.current_line < len(self.lines):
            raise ValueError("current_line must address one of the lines")


@dataclass(frozen=True, slots=True)
class ProviderUsage:
    """Sanitized provider state; deliberately contains no identity metadata.

    A provider publishes either quota windows (MiniMax) or an account balance
    (DeepSeek). Both may appear together; the panel decides what to draw.
    """

    provider: Provider
    state: SourceState
    sampled_at: int
    short: UsageWindow | None = None
    week: UsageWindow | None = None
    balance: Decimal | None = None
    currency: str = ""

    def __post_init__(self) -> None:
        if not 0 <= self.sampled_at <= 0xFFFFFFFF:
            raise ValueError("sampled_at must fit an unsigned 32-bit timestamp")
        if self.balance is not None:
            if self.balance < 0:
                raise ValueError("balance must not be negative")
            if not (len(self.currency) == 3 and self.currency.isalpha()):
                raise ValueError("currency must be a three-letter code")
            if self.balance * 100 != (self.balance * 100).to_integral_value():
                raise ValueError("balance must be expressible in whole cents")
        elif self.currency:
            raise ValueError("currency without a balance describes nothing")

        present = (
            int(self.short is not None)
            + int(self.week is not None)
            + int(self.balance is not None)
        )
        if self.state is SourceState.OK and present == 0:
            raise ValueError("ok requires at least one value")
        if self.state is SourceState.UNAVAILABLE and present != 0:
            raise ValueError("unavailable must not carry values")
        if self.state is SourceState.PARTIAL and present == 0:
            raise ValueError("partial requires at least one value")
