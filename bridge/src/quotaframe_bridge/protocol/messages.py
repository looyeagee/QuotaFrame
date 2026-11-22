"""Privacy-allowlisted usage.v1 messages and device responses."""

from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import datetime
from typing import Any, Mapping

from quotaframe_bridge.versioning import SemVer, VersionError

from quotaframe_bridge.protocol.validation import validate_name, validate_identifier, validate_token
from quotaframe_bridge.domain.models import (
    LiveSession,
    NowPlayingPublication,
    Provider,
    ProviderUsage,
    SessionState,
    SessionsPublication,
    WaitingReason,
)
from quotaframe_bridge.protocol.ota_messages import OtaMessageError, OtaStatus, parse_ota_status

PROTOCOL_VERSION = 1
MAX_U32 = 0xFFFFFFFF
# The firmware enforces the same bound as `kMaxInverseClockSkewSeconds`
# in usage_core/usage_state.hpp; the two must stay equal.
MAX_INVERSE_CLOCK_SKEW_SECONDS = 300
# Capability a device declares when it renders the now-playing command.
NOW_PLAYING_CAPABILITY = "nowplaying.v1"
# # Capability a device declares when it renders the sessions command.
SESSIONS_CAPABILITY = "sessions.v1"
# Capability a device declares when it understands the extended session states:
# a session that waits for its user, the background shell of one that ended,
# and the reason a waiting session waits.
SESSIONS_STATES_CAPABILITY = "sessions.v2"
# Capability a device declares when the alert mode can be set over the link.
ALERT_SET_CAPABILITY = "alert.set.v1"
# Capability a device declares when the display brightness can be set over the link.
BRIGHTNESS_SET_CAPABILITY = "brightness.set.v1"
# Alert modes share one numbering with the firmware settings page.
ALERT_OFF = 0
ALERT_PAGE = 1
ALERT_BEEP = 2
ALERT_MODES = (ALERT_OFF, ALERT_PAGE, ALERT_BEEP)
# Display text bounds; the whole frame must stay inside the 4096 byte limit.
MAX_TITLE_BYTES = 192
MAX_LINES = 8
MAX_LINE_BYTES = 192
# Sessions page bounds; four live rows and three recent titles fit the panel
# and keep the frame far below the 4096 byte limit.
MAX_LIVE_SESSIONS = 4
MAX_RECENT_SESSIONS = 3
MAX_SESSION_TEXT_BYTES = 96



class ProtocolError(ValueError):
    """A local message or remote response violates usage protocol version 1."""


class AckError(ProtocolError):
    """The device rejected a command or acknowledged a different command."""


class CommandRejected(AckError):
    """A matching response explicitly rejected the command."""


def is_folder_rejection(message: Mapping[str, Any], command: str) -> bool:
    """Folder Push failures report zero or the number of bytes already written."""

    return (
        command in {"char_begin", "file", "chunk", "file_end", "char_end"}
        and message.get("ack") == command
        and message.get("ok") is False
        and type(message.get("n")) is int
        and 0 <= message["n"] <= 0xFFFFFFFF
    )


class IncompatibleProtocolError(ProtocolError):
    """A valid status declares an unsupported base protocol or required capability."""


@dataclass(frozen=True, slots=True)
class DeviceStatus:
    """Validated, non-sensitive device capabilities."""

    name: str
    secure: bool
    protocol: int
    page: str
    capabilities: frozenset[str]
    firmware_version: str = ""
    target: str = ""
    ota: OtaStatus | None = None
    boot_valid: bool = False
    firmware_project: str = ""
    # Persisted alert mode the panel reports, or None when it reports none.
    alert: int | None = None
    # Persisted display brightness percent the panel reports, or None when it reports none.
    brightness: int | None = None


class Sequence:
    """Allocate command sequence numbers modulo the unsigned 32-bit range."""

    def __init__(self, initial: int = 0) -> None:
        if isinstance(initial, bool) or not 0 <= initial <= MAX_U32:
            raise ValueError("initial sequence must fit unsigned 32-bit")
        self._value = initial

    def next(self) -> int:
        value = self._value
        self._value = (value + 1) & MAX_U32
        return value


def _u32(value: int, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= MAX_U32:
        raise ProtocolError(f"{field} must fit unsigned 32-bit")
    return value


def _u32_text(value: int, field: str) -> str:
    """Encode an exact integer through Buddy's public string getter."""

    return str(_u32(value, field))


def _json_line(payload: Mapping[str, object]) -> bytes:
    return (
        json.dumps(
            payload,
            ensure_ascii=True,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("utf-8")


def encode_time_sync(calendar: datetime, seq: int) -> bytes:
    """Encode host-local calendar fields for a time.sync.v1 capable device."""

    if calendar.tzinfo is None or calendar.utcoffset() is None:
        raise ValueError("time sync calendar must be timezone-aware")
    if not 2024 <= calendar.year <= 2099:
        raise ValueError("time sync year must be between 2024 and 2099")
    offset_seconds = calendar.utcoffset().total_seconds()
    if offset_seconds % 60 != 0:
        raise ValueError("time sync UTC offset must use whole minutes")
    offset_minutes = int(offset_seconds // 60)
    if not -840 <= offset_minutes <= 840:
        raise ValueError("time sync UTC offset is outside supported range")

    return _json_line(
        {
            "cmd": "time_sync",
            "v": "1",
            "seq": _u32_text(seq, "seq"),
            "year": str(calendar.year),
            "month": str(calendar.month),
            "day": str(calendar.day),
            "weekday": str((calendar.weekday() + 1) % 7),
            "hour": str(calendar.hour),
            "minute": str(calendar.minute),
            "second": str(calendar.second),
            "utc_offset_min": str(offset_minutes),
        }
    )


def encode_usage(usage: ProviderUsage, seq: int, sent_at: int) -> bytes:
    """Encode only the usage.v1 allowlist."""

    sequence = _u32(seq, "seq")
    sent = _u32(sent_at, "sent_at")
    if sent + MAX_INVERSE_CLOCK_SKEW_SECONDS < usage.sampled_at:
        raise ProtocolError("sent_at precedes sampled_at by more than 300 seconds")

    payload: dict[str, object] = {
        "cmd": "usage",
        "v": _u32_text(PROTOCOL_VERSION, "v"),
        "seq": _u32_text(sequence, "seq"),
        "provider": usage.provider.value,
        "state": usage.state.value,
        "sampled_at": _u32_text(usage.sampled_at, "sampled_at"),
        "sent_at": _u32_text(sent, "sent_at"),
    }
    if usage.short is not None:
        payload["short_used_pct"] = _u32_text(
            usage.short.used_percent, "short_used_pct"
        )
        if usage.short.reset_at is not None:
            payload["short_reset_at"] = _u32_text(
                usage.short.reset_at, "short_reset_at"
            )
    if usage.week is not None:
        payload["week_used_pct"] = _u32_text(
            usage.week.used_percent, "week_used_pct"
        )
        if usage.week.reset_at is not None:
            payload["week_reset_at"] = _u32_text(
                usage.week.reset_at, "week_reset_at"
            )
    if usage.balance is not None:
        # Cents keep the amount exact through the device's integer-only parser.
        payload["balance_cents"] = _u32_text(
            int(usage.balance * 100), "balance_cents"
        )
        payload["currency"] = usage.currency
    return _json_line(payload)


def encode_screen_toggle(seq: int) -> bytes:
    """Request one device-local screensaver state transition."""

    return _json_line({"cmd": "screen_toggle", "v": "1", "seq": _u32_text(seq, "seq")})


def encode_screen_page(seq: int, previous: bool) -> bytes:
    """Request one relative page change, waking the usage view if needed."""

    return _json_line({"cmd": "screen_page", "v": "1", "seq": _u32_text(seq, "seq"),
                       "previous": "1" if previous else "0"})


def encode_alert_set(seq: int, mode: int) -> bytes:
    """Set one panel's persisted alert mode (0 off, 1 page, 2 page and tone).

    The device writes the value to its own storage, so the setting survives a
    reboot and a reconnect without Bridge repeating it.
    """

    if type(mode) is not int or mode not in ALERT_MODES:
        raise ValueError("alert mode must be 0, 1 or 2")
    return _json_line({"cmd": "alert_set", "v": "1", "seq": _u32_text(seq, "seq"),
                       "mode": str(mode)})


def encode_brightness_set(seq: int, percent: int) -> bytes:
    """Set one panel's persisted display brightness in percent (0..100).

    The device writes the value to its own storage, so the setting survives a
    reboot and a reconnect without Bridge repeating it.
    """

    if type(percent) is not int or not 0 <= percent <= 100:
        raise ValueError("brightness percent must be an int from 0 to 100")
    return _json_line({"cmd": "brightness_set", "v": "1", "seq": _u32_text(seq, "seq"),
                       "percent": str(percent)})


def _truncate_utf8(text: str, limit: int) -> str:
    """Cut `text` to at most `limit` bytes without splitting a character."""

    encoded = text.encode("utf-8")
    if len(encoded) <= limit:
        return text
    return encoded[:limit].decode("utf-8", "ignore")


def _single_line(text: str) -> str:
    """Fold a display text into one line.

    The lists travel newline-separated, so a name that carries a line break of
    its own would split into two entries and make the roster unreadable.
    """

    return " ".join(text.split())


def encode_now_playing(publication: NowPlayingPublication, seq: int) -> bytes:
    """Encode one now-playing publication for a panel's music page.

    The lyric lines travel as one string with newline separators: the command
    view of the device reads strings, and a window of lines stays far below the
    4096 byte frame limit that a whole song would exceed.

    The playhead and the start of every line travel in milliseconds, so a panel
    can tell where the song is on its own timeline. `line_times` carries one
    value per line, in the order of `lines`; a panel that does not know the
    field keeps using the seconds.
    """

    sequence = _u32(seq, "seq")
    lines = publication.lines[:MAX_LINES]
    payload: dict[str, object] = {
        "cmd": "now_playing",
        "v": "1",
        "seq": _u32_text(sequence, "seq"),
        "title": _truncate_utf8(publication.title, MAX_TITLE_BYTES),
        "artist": _truncate_utf8(publication.artist, MAX_TITLE_BYTES),
        "album": _truncate_utf8(publication.album, MAX_TITLE_BYTES),
        "position": _u32_text(publication.position_seconds, "position"),
        "duration": _u32_text(publication.duration_seconds, "duration"),
        "state": "playing" if publication.playing else "paused",
    }
    if publication.line_end_seconds:
        # Optional upper bound for the panel's scroll: it may finish sooner, never later.
        payload["line_end"] = _u32_text(publication.line_end_seconds, "line_end")
    if lines:
        payload["current"] = _u32_text(publication.current_line, "current")
        payload["lines"] = "\n".join(
            _truncate_utf8(line, MAX_LINE_BYTES) for line in lines
        )
        times = publication.line_times_ms[:MAX_LINES]
        if len(times) == len(lines):
            payload["line_times"] = "\n".join(
                _u32_text(value, "line_times") for value in times
            )
    if publication.position_ms or publication.line_times_ms:
        # A song may start at zero, so the field travels whenever the source knows a
        # playhead; the device reads its presence as the position being known.
        payload["position_ms"] = _u32_text(publication.position_ms, "position_ms")
    return (
        json.dumps(
            payload,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("utf-8")


def _session_state_token(session: LiveSession, *, extended: bool) -> str:
    """The state word of one live session.

    A device that only knows the first vocabulary is told that a waiting
    session is busy: its turn has not ended, and two states cannot say more
    than that. A background shell produces nothing, so it reads as idle.
    """

    if not extended:
        if session.state is SessionState.WAITING:
            return "busy"
        return "busy" if session.state is SessionState.BUSY else "idle"
    if session.state is SessionState.WAITING:
        # The reason travels with the state, so the two cannot disagree about
        # which session waits for what.
        reason = session.waiting_for or WaitingReason.DIALOG
        return f"waiting/{reason.value}"
    return session.state.value


def encode_sessions(
    publication: SessionsPublication, seq: int, *, extended: bool = False
) -> bytes:
    """Encode one sessions publication for a panel's sessions page.

    The live names and their states travel as two parallel newline-joined
    strings, like the lyric lines and their start times do: the command view of
    the device reads strings, so a short list travels without adding an array
    type to the protocol. An empty list stays off the wire rather than sending
    a frame the device would count as one empty entry.

    `extended` sends the states a device declaring `sessions.v2` understands:
    a session that waits for its user, the background shell of one that ended,
    and the reason a waiting session waits.
    """

    sequence = _u32(seq, "seq")
    live = publication.live[:MAX_LIVE_SESSIONS]
    recent = publication.recent[:MAX_RECENT_SESSIONS]
    payload: dict[str, object] = {
        "cmd": "sessions",
        "v": "1",
        "seq": _u32_text(sequence, "seq"),
    }
    if live:
        payload["live"] = "\n".join(
            _truncate_utf8(_single_line(session.name), MAX_SESSION_TEXT_BYTES)
            for session in live
        )
        payload["states"] = "\n".join(
            _session_state_token(session, extended=extended) for session in live
        )
    if recent:
        payload["recent"] = "\n".join(
            _truncate_utf8(_single_line(title), MAX_SESSION_TEXT_BYTES)
            for title in recent
        )
    return (
        json.dumps(
            payload,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("utf-8")


def encode_status_query() -> bytes:
    """Build the core Buddy status query."""

    return b'{"cmd":"status"}\n'


def parse_ack(
    message: Mapping[str, Any],
    expected_command: str,
    expected_n: int,
) -> None:
    """Require a successful ACK for the command currently in flight."""

    if message.get("ack") != expected_command:
        raise AckError("ack command does not match")
    response_sequence = message.get("n")
    if (
        isinstance(response_sequence, bool)
        or not isinstance(response_sequence, int)
        or (response_sequence != expected_n and not is_folder_rejection(message, expected_command))
    ):
        raise AckError("ack value does not match")
    if message.get("ok") is False:
        error = message.get("error")
        detail = error if isinstance(error, str) and error else "rejected"
        raise CommandRejected(f"device rejected {expected_command}: {detail}")
    if message.get("ok") is not True:
        raise AckError("ack result is invalid")


def parse_status(message: Mapping[str, Any]) -> DeviceStatus:
    """Validate status negotiation before usage publication."""

    if message.get("ack") != "status" or message.get("ok") is not True:
        raise ProtocolError("device did not return successful status")
    data = message.get("data")
    if not isinstance(data, dict):
        raise ProtocolError("status data must be an object")

    name = data.get("name")
    secure = data.get("sec")
    protocol = data.get("protocol")
    page = data.get("page", "")
    caps = data.get("caps")
    try:
        validate_name(name)
    except ValueError as exc:
        raise ProtocolError(str(exc)) from None
    if type(message.get("n")) is not int or message["n"] != 0:
        raise ProtocolError("status ACK counter is invalid")
    if secure is not True:
        raise ProtocolError("device link is not reported secure")
    if (
        isinstance(protocol, bool)
        or not isinstance(protocol, int)
    ):
        raise ProtocolError("device protocol version is invalid")
    try:
        if "page" in data:
            validate_token(page, "page", 31)
        if not isinstance(caps, list) or not 1 <= len(caps) <= 32:
            raise ValueError("device capabilities are invalid")
        for token in caps:
            validate_token(token, "capability")
        capabilities = frozenset(caps)
        if len(capabilities) != len(caps) or sum(map(len, caps)) > 512:
            raise ValueError("device capabilities are duplicated or too large")
    except ValueError as exc:
        raise ProtocolError(str(exc)) from None
    firmware_version = data.get("fw", "")
    target = data.get("target", "")
    firmware_project = data.get("firmware_project", "")
    try:
        if "target" in data:
            validate_identifier(target, "target")
        if "firmware_project" in data:
            validate_identifier(firmware_project, "firmware_project", 64)
        if "fw" in data and (not isinstance(firmware_version, str)
                or not 1 <= len(firmware_version) <= 31
                or any(not 0x20 <= ord(c) <= 0x7E or c in '\"\\' for c in firmware_version)):
            raise ValueError("device firmware version is invalid")
    except ValueError as exc:
        raise ProtocolError(str(exc)) from None
    ota_value = data.get("ota")
    boot_valid = data.get("boot_valid", False)
    if not isinstance(boot_valid, bool):
        raise ProtocolError("device boot validation state is invalid")
    alert = data.get("alert")
    if "alert" in data:
        if isinstance(alert, bool) or not isinstance(alert, int) or alert not in ALERT_MODES:
            raise ProtocolError("device alert mode is invalid")
    else:
        alert = None
    brightness = data.get("brightness")
    if "brightness" in data:
        if isinstance(brightness, bool) or not isinstance(brightness, int) \
                or not 0 <= brightness <= 100:
            raise ProtocolError("device brightness is invalid")
    else:
        brightness = None
    try:
        ota = None if "ota" not in data else parse_ota_status(ota_value)
    except OtaMessageError as exc:
        raise ProtocolError(str(exc)) from None
    if "ota.folder.v1" in capabilities and (
        not firmware_version or not target or not firmware_project or ota is None
    ):
        raise ProtocolError("device OTA status is incomplete")
    if "ota.folder.v1" in capabilities:
        try:
            SemVer.parse(firmware_version.removeprefix("v"))
        except VersionError:
            raise ProtocolError("device OTA firmware version is invalid") from None
    if protocol != PROTOCOL_VERSION:
        raise IncompatibleProtocolError(
            "device protocol is incompatible; update Bridge or firmware, then restart Bridge"
        )
    if "usage.v1" not in capabilities:
        raise IncompatibleProtocolError(
            "device lacks usage.v1; install compatible firmware, then restart Bridge"
        )
    return DeviceStatus(
        name=name,
        secure=True,
        protocol=PROTOCOL_VERSION,
        page=page,
        boot_valid=boot_valid,
        capabilities=capabilities,
        firmware_version=firmware_version,
        target=target,
        firmware_project=firmware_project,
        ota=ota,
        alert=alert,
        brightness=brightness,
    )
