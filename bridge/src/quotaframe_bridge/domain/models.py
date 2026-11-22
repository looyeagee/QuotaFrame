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

    The playhead and the start of every line in the window are also carried in
    milliseconds, so a panel can place the song on its own timeline instead of
    counting from the moment a publication arrived. The seconds are the same
    instants rounded down, for a panel that only draws a clock.
    """

    title: str
    artist: str
    album: str
    position_seconds: int
    duration_seconds: int
    playing: bool
    lines: tuple[str, ...] = ()
    current_line: int = 0
    # Position at which the sung line stops and the next one starts, or 0 when the
    # source cannot say. A panel only uses it as an upper bound for its scroll.
    line_end_seconds: int = 0
    # Playhead in milliseconds, 0 when the source cannot say.
    position_ms: int = 0
    # Start of every line in `lines`, in milliseconds, in the same order.
    line_times_ms: tuple[int, ...] = ()

    def __post_init__(self) -> None:
        if self.position_seconds < 0 or self.duration_seconds < 0:
            raise ValueError("now-playing times must not be negative")
        if self.line_end_seconds < 0:
            raise ValueError("line end must not be negative")
        if self.position_ms < 0:
            raise ValueError("now-playing position must not be negative")
        if self.lines and not 0 <= self.current_line < len(self.lines):
            raise ValueError("current_line must address one of the lines")
        if self.line_times_ms and len(self.line_times_ms) != len(self.lines):
            raise ValueError("line_times_ms must address every line")


@dataclass(frozen=True, slots=True)
class SessionsPublication:
    """Sanitized Claude Code session state for a panel's sessions page.

    A panel shows which Claude Code sessions are live and what they are working
    on. Only display text travels: the session name or its first prompt stands
    in for the session, and session ids, working directories and transcripts
    stay on the computer. `live` pairs each running session's name with whether
    it is busy; `recent` lists the most recently active ended sessions.
    """

    # (display name, busy) for every running session, newest first.
    live: tuple[tuple[str, bool], ...] = ()
    recent: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if len(self.live) > 4:
            raise ValueError("a publication carries at most four live sessions")
        if len(self.recent) > 3:
            raise ValueError("a publication carries at most three recent sessions")
        for name, busy in self.live:
            if not name:
                raise ValueError("a live session needs a display name")
            if not isinstance(busy, bool):
                raise ValueError("a live session's busy flag must be a bool")
        for title in self.recent:
            if not title:
                raise ValueError("a recent session needs a title")


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
