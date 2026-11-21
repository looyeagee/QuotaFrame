"""Request routing for the control page, independent of the socket layer.

The page and the API are served from the same origin and authorised by one
token, which the page receives in its address on first load and then sends as a
header. No cookie is involved, so a request from another site carries nothing
that could authorise it.
"""

from __future__ import annotations

import hmac
import json
from collections.abc import Mapping
from dataclasses import dataclass
from urllib.parse import parse_qs, urlsplit

from quotaframe_bridge.control.backend import PanelControl
from quotaframe_bridge.control.page import render_page

TOKEN_HEADER = "x-quotaframe-token"
JSON_TYPE = "application/json; charset=utf-8"
HTML_TYPE = "text/html; charset=utf-8"
# Everything served here is live state; a cache would show a stale panel page.
STATIC_HEADERS = (
    ("Cache-Control", "no-store"),
    ("X-Content-Type-Options", "nosniff"),
    ("Referrer-Policy", "no-referrer"),
)
PAGE_PATH = "/"
STATE_PATH = "/api/state"
TOGGLE_PATH = "/api/screen/toggle"
PAGE_COMMAND_PATH = "/api/screen/page"
ALERT_PATH = "/api/alert"
REFRESH_PATH = "/api/refresh"
FAVICON_PATH = "/favicon.ico"
# The page is the only client; anything else is a typo or a probe.
ALLOWED_METHODS = {
    PAGE_PATH: ("GET",),
    STATE_PATH: ("GET",),
    TOGGLE_PATH: ("POST",),
    PAGE_COMMAND_PATH: ("POST",),
    ALERT_PATH: ("POST",),
    REFRESH_PATH: ("POST",),
    FAVICON_PATH: ("GET",),
}
DIRECTIONS = {"next": 1, "previous": -1}
LOOPBACK_HOSTNAMES = frozenset({"127.0.0.1", "localhost", "[::1]", "::1"})


@dataclass(frozen=True, slots=True)
class Request:
    """One parsed HTTP request. Header names are lower case."""

    method: str
    target: str
    headers: Mapping[str, str]
    body: bytes = b""

    def query(self) -> dict[str, list[str]]:
        return parse_qs(urlsplit(self.target).query)


@dataclass(frozen=True, slots=True)
class Response:
    status: int
    body: bytes = b""
    content_type: str = JSON_TYPE
    headers: tuple[tuple[str, str], ...] = ()


class ControlRoutes:
    """Map one request onto the control backend.

    `bound_host` decides whether the `Host` header is enforced: a loopback bind
    can only be reached by a name that resolves to loopback, so a foreign host
    name means DNS rebinding. A LAN bind is reached by whatever address that
    machine has, which cannot be enumerated, so the token is the whole defence
    there.
    """

    def __init__(
        self,
        control: PanelControl,
        *,
        token: str,
        bound_host: str = "127.0.0.1",
    ) -> None:
        if not token:
            raise ValueError("control token must not be empty")
        self._control = control
        self._token = token
        self._enforce_host = _is_loopback(bound_host)
        self._bound_hostname = bound_host.strip("[]").lower()

    def handle(self, request: Request) -> Response:
        if self._enforce_host and not self._host_allowed(request):
            return _error(421, "host_not_allowed")
        if not self._authorized(request):
            return _error(401, "token_required")
        path = urlsplit(request.target).path
        allowed = ALLOWED_METHODS.get(path)
        if allowed is not None and request.method not in allowed:
            return _error(405, "method_not_allowed", {"Allow": ", ".join(allowed)})
        if path == PAGE_PATH:
            return Response(200, render_page(token=self._token).encode("utf-8"), HTML_TYPE)
        if path == FAVICON_PATH:
            return Response(204, b"", "text/plain; charset=utf-8")
        if path == STATE_PATH:
            return _ok(self._control.state())
        if path == TOGGLE_PATH:
            return _accepted(self._control.toggle_screen())
        if path == PAGE_COMMAND_PATH:
            return self._turn_page(request)
        if path == ALERT_PATH:
            return self._set_alert(request)
        if path == REFRESH_PATH:
            return _accepted(self._control.refresh())
        return _error(404, "not_found")

    def _turn_page(self, request: Request) -> Response:
        try:
            direction = _direction(request)
        except ValueError as exc:
            return _error(400, str(exc))
        if direction is None:
            return _error(400, "direction_required")
        return _accepted(self._control.turn_page(direction))

    def _set_alert(self, request: Request) -> Response:
        try:
            mode = _alert_mode(request)
        except ValueError as exc:
            return _error(400, str(exc))
        if mode is None:
            return _error(400, "mode_required")
        return _accepted(self._control.set_alert(mode))

    def _authorized(self, request: Request) -> bool:
        supplied = request.headers.get(TOKEN_HEADER)
        if supplied is None:
            values = request.query().get("token")
            supplied = values[0] if values else None
        if supplied is None:
            return False
        # Byte comparison keeps a non-ASCII header value a rejection, not a crash.
        return hmac.compare_digest(
            supplied.encode("utf-8", "replace"), self._token.encode("utf-8")
        )

    def _host_allowed(self, request: Request) -> bool:
        host = request.headers.get("host")
        if not host:
            return False
        name = _hostname(host)
        return name in LOOPBACK_HOSTNAMES or name == self._bound_hostname


def _direction(request: Request) -> int | None:
    """Read `{"direction": "next"}` from the body or `?direction=` from the URL."""

    raw: object = None
    if request.body:
        try:
            payload = json.loads(request.body)
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            raise ValueError("invalid_json") from exc
        if not isinstance(payload, dict):
            raise ValueError("invalid_json")
        raw = payload.get("direction")
    if raw is None:
        values = request.query().get("direction")
        raw = values[0] if values else None
    if raw is None:
        return None
    if not isinstance(raw, str) or raw not in DIRECTIONS:
        raise ValueError("invalid_direction")
    return DIRECTIONS[raw]


# The mode numbering is shared with the firmware settings page and status.alert.
ALERT_MODES = {"0": 0, "1": 1, "2": 2}


def _alert_mode(request: Request) -> int | None:
    """Read `{"mode": 0|1|2}` from the body or `?mode=` from the URL."""

    raw: object = None
    if request.body:
        try:
            payload = json.loads(request.body)
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            raise ValueError("invalid_json") from exc
        if not isinstance(payload, dict):
            raise ValueError("invalid_json")
        raw = payload.get("mode")
    if raw is None:
        values = request.query().get("mode")
        raw = values[0] if values else None
    if raw is None:
        return None
    if isinstance(raw, bool):
        raise ValueError("invalid_mode")
    if isinstance(raw, int):
        text = str(raw)
    elif isinstance(raw, str):
        text = raw
    else:
        raise ValueError("invalid_mode")
    if text not in ALERT_MODES:
        raise ValueError("invalid_mode")
    return ALERT_MODES[text]


def _is_loopback(host: str) -> bool:
    name = host.strip("[]").lower()
    return name in {value.strip("[]") for value in LOOPBACK_HOSTNAMES}


def _hostname(host: str) -> str:
    if host.startswith("["):
        end = host.find("]")
        return host[: end + 1].lower() if end != -1 else host.lower()
    name, _, port = host.rpartition(":")
    return (name if port.isdigit() else host).lower()


def _ok(payload: dict[str, object], status: int = 200) -> Response:
    return Response(status, json.dumps(payload).encode("utf-8"))


def _accepted(payload: dict[str, object]) -> Response:
    # 202: the command is queued on the session, not yet acknowledged by a panel.
    return Response(202, json.dumps(payload).encode("utf-8"))


def _error(status: int, code: str, headers: Mapping[str, str] | None = None) -> Response:
    return Response(
        status,
        json.dumps({"error": code}).encode("utf-8"),
        JSON_TYPE,
        tuple((headers or {}).items()),
    )
