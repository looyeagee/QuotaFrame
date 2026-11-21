"""A minimal HTTP/1.1 server for the control page.

The Bridge already owns an event loop, so this uses the standard library's
stream API rather than a framework: one dependency less in a frozen build, and
no third-party parser between a local browser and a BLE command.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging

from quotaframe_bridge.control.backend import PanelControl
from quotaframe_bridge.control.routes import STATIC_HEADERS, ControlRoutes, Request, Response

LOGGER = logging.getLogger(__name__)

# Headers arrive in one small block; bodies carry a direction word at most.
MAX_HEADER_BYTES = 8192
MAX_BODY_BYTES = 4096
# A browser keeps the connection alive between polls; an idle one is dropped
# instead of pinning a task for the life of the process.
REQUEST_TIMEOUT_S = 10.0
IDLE_TIMEOUT_S = 30.0
KEEP_ALIVE_REQUESTS = 64

_STATUS_TEXT = {
    200: "OK",
    202: "Accepted",
    204: "No Content",
    400: "Bad Request",
    401: "Unauthorized",
    404: "Not Found",
    405: "Method Not Allowed",
    413: "Content Too Large",
    421: "Misdirected Request",
    500: "Internal Server Error",
    501: "Not Implemented",
}


class ControlServer:
    """Serve the page and its API on one loopback port.

    `start()` raises when the port cannot be bound, while `__aenter__` logs and
    continues without a page: publishing usage to a panel does not depend on it.
    """

    def __init__(
        self,
        control: PanelControl,
        *,
        token: str,
        host: str = "127.0.0.1",
        port: int = 8788,
    ) -> None:
        self._routes = ControlRoutes(control, token=token, bound_host=host)
        self._token = token
        self._host = host
        self._requested_port = port
        self._server: asyncio.Server | None = None
        self.port = port

    @property
    def url(self) -> str:
        return f"http://{_url_host(self._host)}:{self.port}/?token={self._token}"

    @property
    def running(self) -> bool:
        return self._server is not None and self._server.is_serving()

    async def start(self) -> ControlServer:
        self._server = await asyncio.start_server(
            self._serve,
            self._host,
            self._requested_port,
            limit=MAX_HEADER_BYTES,
            reuse_address=True,
        )
        sockets = self._server.sockets or ()
        if sockets:
            self.port = sockets[0].getsockname()[1]
        LOGGER.info("control page on %s", self.url)
        return self

    async def close(self) -> None:
        server = self._server
        self._server = None
        if server is None:
            return
        server.close()
        with contextlib.suppress(Exception):
            await server.wait_closed()

    async def __aenter__(self) -> ControlServer:
        try:
            return await self.start()
        except OSError as exc:
            LOGGER.warning(
                "control page not started on %s:%s: %s", self._host, self._requested_port, exc
            )
            return self

    async def __aexit__(self, *exc_info: object) -> None:
        await self.close()

    async def _serve(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            for _ in range(KEEP_ALIVE_REQUESTS):
                request = await _read_request(reader)
                if request is None:
                    return
                response = self._respond(request)
                keep_alive = _keep_alive(request)
                writer.write(_encode(response, keep_alive))
                await writer.drain()
                if not keep_alive:
                    return
        except (TimeoutError, ConnectionError, asyncio.IncompleteReadError):
            return
        except _BadRequest as exc:
            writer.write(_encode(_failure(exc.status, exc.code), False))
            await _drain_quietly(writer)
        except asyncio.LimitOverrunError:
            writer.write(_encode(_failure(413, "headers_too_large"), False))
            await _drain_quietly(writer)
        except Exception:
            LOGGER.exception("control request failed")
            writer.write(_encode(_failure(500, "internal_error"), False))
            await _drain_quietly(writer)
        finally:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()

    def _respond(self, request: Request) -> Response:
        try:
            return self._routes.handle(request)
        except Exception:
            LOGGER.exception("control route failed")
            return _failure(500, "internal_error")


def _url_host(host: str) -> str:
    return f"[{host}]" if ":" in host and not host.startswith("[") else host


async def _read_request(reader: asyncio.StreamReader) -> Request | None:
    """Read one request head and its body, or None once the peer is done."""

    try:
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), IDLE_TIMEOUT_S)
    except asyncio.IncompleteReadError as exc:
        if not exc.partial:
            return None
        raise
    lines = head.decode("iso-8859-1").split("\r\n")
    try:
        method, target, version = lines[0].split(" ")
    except ValueError as exc:
        raise _BadRequest("malformed_request_line") from exc
    if not version.startswith("HTTP/"):
        raise _BadRequest("unsupported_protocol")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if not line:
            continue
        name, separator, value = line.partition(":")
        if not separator:
            raise _BadRequest("malformed_header")
        headers[name.strip().lower()] = value.strip()
    if "transfer-encoding" in headers:
        raise _BadRequest("chunked_body_unsupported")
    body = await _read_body(reader, headers.get("content-length", "0"))
    return Request(method=method.upper(), target=target, headers=headers, body=body)


async def _read_body(reader: asyncio.StreamReader, length_field: str) -> bytes:
    if not length_field.isdigit():
        raise _BadRequest("invalid_content_length")
    length = int(length_field)
    if length > MAX_BODY_BYTES:
        raise _BadRequest("body_too_large", status=413)
    if length == 0:
        return b""
    return await asyncio.wait_for(reader.readexactly(length), REQUEST_TIMEOUT_S)


class _BadRequest(Exception):
    def __init__(self, code: str, *, status: int = 400) -> None:
        super().__init__(code)
        self.code = code
        self.status = status


def _keep_alive(request: Request) -> bool:
    connection = request.headers.get("connection", "").lower()
    if "close" in connection:
        return False
    if request.headers.get("host") is None:
        # HTTP/1.0 without a Host header is a one-shot request.
        return False
    return True


def _encode(response: Response, keep_alive: bool) -> bytes:
    text = _STATUS_TEXT.get(response.status, "OK")
    lines = [
        f"HTTP/1.1 {response.status} {text}",
        f"Content-Type: {response.content_type}",
        f"Content-Length: {len(response.body)}",
        f"Connection: {'keep-alive' if keep_alive else 'close'}",
    ]
    lines.extend(f"{name}: {value}" for name, value in STATIC_HEADERS)
    lines.extend(f"{name}: {value}" for name, value in response.headers)
    head = "\r\n".join(lines) + "\r\n\r\n"
    return head.encode("iso-8859-1") + response.body


def _failure(status: int, code: str) -> Response:
    return Response(status, json.dumps({"error": code}).encode("utf-8"))


async def _drain_quietly(writer: asyncio.StreamWriter) -> None:
    with contextlib.suppress(Exception):
        await writer.drain()
