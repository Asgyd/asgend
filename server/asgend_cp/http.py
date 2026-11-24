"""Minimal HTTP layer on top of http.server (standard library only).

* ThreadingHTTPServer: one thread per connection, so long-polls do not block
  other requests.
* Optional direct TLS (handshake in the worker thread, TLS >= 1.2).
* Graceful shutdown: stop accepting, wake long-polls, wait for in-flight
  requests (bounded), then close idle keep-alive connections.
"""

from __future__ import annotations

import json
import re
import socket
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlsplit

from . import API_VERSION


class ApiError(Exception):
    STATUS = {
        "invalid_request": 400, "unauthorized": 401, "forbidden": 403, "not_found": 404,
        "conflict": 409, "gone": 410, "payload_too_large": 413, "checksum_mismatch": 422,
        "unprocessable": 422, "rate_limited": 429, "quota_exceeded": 429, "internal": 500,
        "unavailable": 503,
    }

    def __init__(self, code: str, message: str = "", details: dict | None = None,
                 status: int | None = None, headers: dict | None = None):
        super().__init__(message or code)
        self.code = code
        self.message = message or code.replace("_", " ")
        self.details = details or {}
        self.status = status or self.STATUS.get(code, 500)
        self.headers = headers or {}


class Response:
    def __init__(self, status: int = 200, body: bytes = b"", headers: dict | None = None,
                 stream=None, length: int | None = None):
        self.status = status
        self.body = body
        self.headers = headers or {}
        self.stream = stream          # file object for downloads
        self.length = length          # bytes to copy from stream

    @classmethod
    def json(cls, obj: dict, status: int = 200, headers: dict | None = None) -> "Response":
        data = {"api_version": API_VERSION}
        data.update(obj)
        body = json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        h = {"Content-Type": "application/json; charset=utf-8"}
        if headers:
            h.update(headers)
        return cls(status, body, h)


class Request:
    def __init__(self, handler: "Handler"):
        self.handler = handler
        self.method = handler.command.upper()
        parts = urlsplit(handler.path)
        self.raw_path = parts.path
        self.path = unquote(parts.path)
        self.query = {k: v[0] for k, v in parse_qs(parts.query, keep_blank_values=True).items()}
        self.headers = handler.headers
        self.peer_ip = handler.client_address[0] if handler.client_address else ""
        self.client_ip = self.peer_ip
        self.request_id = ""
        self.route = None
        self.params: dict = {}
        self.principal = None
        self._body: bytes | None = None
        self.body_consumed = False

    def header(self, name: str, default: str = "") -> str:
        v = self.headers.get(name)
        return v if v is not None else default

    # -- body ----------------------------------------------------------------
    def content_length(self) -> int | None:
        v = self.headers.get("Content-Length")
        if v is None:
            return None
        if not v.strip().isdigit():
            raise ApiError("invalid_request", "invalid Content-Length")
        return int(v)

    def read_body(self, limit: int) -> bytes:
        if self._body is not None:
            return self._body
        te = self.header("Transfer-Encoding").lower()
        rfile = self.handler.rfile
        if "chunked" in te:
            out = bytearray()
            while True:
                line = rfile.readline(1024)
                if not line:
                    raise ApiError("invalid_request", "truncated chunked body")
                try:
                    size = int(line.split(b";", 1)[0].strip(), 16)
                except ValueError:
                    raise ApiError("invalid_request", "bad chunk size") from None
                if size == 0:
                    while rfile.readline(1024) not in (b"\r\n", b"\n", b""):
                        pass
                    break
                if len(out) + size > limit:
                    self.handler.close_connection = True
                    raise ApiError("payload_too_large", "request body too large",
                                   {"limit": limit})
                out += rfile.read(size)
                rfile.readline(16)
            self._body = bytes(out)
        else:
            n = self.content_length() or 0
            if n > limit:
                self.handler.close_connection = True
                raise ApiError("payload_too_large", "request body too large", {"limit": limit})
            data = rfile.read(n) if n else b""
            if len(data) != n:
                self.handler.close_connection = True
                raise ApiError("invalid_request", "truncated body")
            self._body = data
        self.body_consumed = True
        return self._body

    def json(self, limit: int) -> dict:
        raw = self.read_body(limit)
        if not raw.strip():
            return {}
        try:
            obj = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            raise ApiError("invalid_request", "body is not valid UTF-8 JSON") from None
        if not isinstance(obj, dict):
            raise ApiError("invalid_request", "JSON body must be an object")
        return obj


class Route:
    def __init__(self, method: str, pattern: str, fn, auth: str, name: str, body_limit: str = "json"):
        self.method = method
        self.pattern = pattern
        self.regex = re.compile("^" + pattern + "$")
        self.fn = fn
        self.auth = auth           # none | user | node | metrics | ticket
        self.name = name           # stable label for logs/metrics (no IDs)
        self.body_limit = body_limit


class Router:
    def __init__(self):
        self.routes: list[Route] = []

    def add(self, method, pattern, fn, auth, name=None, body_limit="json"):
        self.routes.append(Route(method, pattern, fn, auth, name or pattern, body_limit))

    def match(self, method: str, path: str):
        allowed = []
        for r in self.routes:
            m = r.regex.match(path)
            if m:
                if r.method == method or (method == "HEAD" and r.method == "GET"):
                    return r, m.groupdict(), None
                allowed.append(r.method)
        return None, None, allowed


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "asgend-control-plane"
    sys_version = ""

    def setup(self):
        super().setup()
        self.idle = True
        self.connection.settimeout(self.server.app.config.idle_timeout_seconds)
        with self.server.conn_lock:
            self.server.connections.add(self)

    def finish(self):
        with self.server.conn_lock:
            self.server.connections.discard(self)
        try:
            super().finish()
        except OSError:
            pass

    def handle(self):
        self.close_connection = True
        try:
            self.handle_one_request()
            while not self.close_connection and not self.server.stopping:
                self.handle_one_request()
        except (OSError, ssl.SSLError):
            self.close_connection = True

    def handle_one_request(self):
        self.idle = True
        try:
            self.raw_requestline = self.rfile.readline(65537)
        except (TimeoutError, socket.timeout, OSError, ssl.SSLError):
            self.close_connection = True
            return
        if self.server.stopping and not self.raw_requestline:
            self.close_connection = True
            return
        self.idle = False
        try:
            if len(self.raw_requestline) > 65536:
                self.send_error(414)
                return
            if not self.raw_requestline:
                self.close_connection = True
                return
            if not self.parse_request():
                return
            self.server.begin_request()
            try:
                self.server.app.dispatch(self)
            finally:
                self.server.end_request()
            try:
                self.wfile.flush()
            except OSError:
                self.close_connection = True
        finally:
            self.idle = True

    def log_message(self, fmt, *args):  # access logs are emitted by the app
        pass

    def log_error(self, fmt, *args):
        pass


class ControlPlaneHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False
    request_queue_size = 128

    def __init__(self, app, host: str, port: int, ssl_context: ssl.SSLContext | None = None):
        self.app = app
        self.ssl_context = ssl_context
        self.stopping = False
        self.connections: set = set()
        self.conn_lock = threading.Lock()
        self._inflight = 0
        self._inflight_cv = threading.Condition()
        if ":" in host:
            self.address_family = socket.AF_INET6
        super().__init__((host, port), Handler)

    def finish_request(self, request, client_address):
        if self.ssl_context is not None:
            try:
                request.settimeout(15)
                request = self.ssl_context.wrap_socket(request, server_side=True)
            except (OSError, ssl.SSLError):
                try:
                    request.close()
                except OSError:
                    pass
                return
        self.RequestHandlerClass(request, client_address, self)

    def begin_request(self):
        with self._inflight_cv:
            self._inflight += 1

    def end_request(self):
        with self._inflight_cv:
            self._inflight -= 1
            self._inflight_cv.notify_all()

    @property
    def inflight(self) -> int:
        return self._inflight

    def graceful_stop(self, grace_seconds: float) -> None:
        """Stop accepting, release long-polls, wait for in-flight requests."""
        self.stopping = True
        self.app.begin_shutdown()
        self.shutdown()  # stops serve_forever accept loop
        deadline = time.monotonic() + grace_seconds
        with self._inflight_cv:
            while self._inflight > 0 and time.monotonic() < deadline:
                self._inflight_cv.wait(0.1)
        with self.conn_lock:
            conns = list(self.connections)
        for h in conns:
            try:
                h.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.server_close()


def make_ssl_context(cert: str, key: str) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(cert, key)
    ctx.set_alpn_protocols(["http/1.1"])
    return ctx
