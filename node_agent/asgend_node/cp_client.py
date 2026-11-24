"""HTTPS client for the Control Plane node channel (contract v1 section 7)."""

from __future__ import annotations

import http.client
import ipaddress
import json
import ssl
import time
import urllib.parse
from typing import Any

from . import __version__

MAX_JSON_RESPONSE = 4 * 1024 * 1024


class CPError(Exception):
    """Non-success HTTP reply from the Control Plane."""

    def __init__(self, status: int, code: str, message: str, retry_after: float | None = None,
                 body: dict | None = None):
        super().__init__(f"HTTP {status} {code}: {message}")
        self.status = status
        self.code = code
        self.message = message
        self.retry_after = retry_after
        self.body = body or {}


class Unauthorized(CPError):
    """401: the node token is invalid or the node was revoked. Fail closed."""


class RateLimited(CPError):
    """429 rate_limited (honour Retry-After)."""


class CPUnavailable(Exception):
    """Network failure, timeout, TLS failure or 5xx; retry with backoff."""

    def __init__(self, message: str, retry_after: float | None = None):
        super().__init__(message)
        self.retry_after = retry_after


def is_loopback_host(host: str) -> bool:
    if not host:
        return False
    if host.lower() in ("localhost", "localhost.") or host.lower().endswith(".localhost"):
        return True
    try:
        return ipaddress.ip_address(host.strip("[]")).is_loopback
    except ValueError:
        return False


def validate_server_url(url: str, allow_insecure_http: bool = False) -> urllib.parse.SplitResult:
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("https", "http") or not parts.hostname:
        raise ValueError("server URL must be an absolute https:// URL")
    if parts.username or parts.password:
        raise ValueError("server URL must not contain credentials")
    if parts.query or parts.fragment:
        raise ValueError("server URL must not contain a query or fragment")
    if parts.scheme == "http" and not allow_insecure_http and not is_loopback_host(parts.hostname):
        raise ValueError("plain http:// is only allowed for loopback hosts or with --allow-insecure-http")
    return parts


def _parse_retry_after(value: str | None) -> float | None:
    if not value:
        return None
    try:
        return max(0.0, float(value))
    except ValueError:
        pass
    try:
        from email.utils import parsedate_to_datetime

        when = parsedate_to_datetime(value)
        return max(0.0, when.timestamp() - time.time())
    except (TypeError, ValueError):
        return None


class CPClient:
    def __init__(self, server_url: str, token: str | None = None, ca_file: str | None = None,
                 allow_insecure_http: bool = False, timeout: float = 30.0):
        self.parts = validate_server_url(server_url, allow_insecure_http)
        self.base_path = self.parts.path.rstrip("/")
        self.token = token
        self.timeout = timeout
        self._ssl_context: ssl.SSLContext | None = None
        if self.parts.scheme == "https":
            ctx = ssl.create_default_context(cafile=ca_file) if ca_file else ssl.create_default_context()
            ctx.check_hostname = True
            ctx.verify_mode = ssl.CERT_REQUIRED
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2
            self._ssl_context = ctx

    # -- low level -----------------------------------------------------------
    def _connection(self, timeout: float) -> http.client.HTTPConnection:
        host = self.parts.hostname
        port = self.parts.port
        if self.parts.scheme == "https":
            return http.client.HTTPSConnection(host, port, timeout=timeout, context=self._ssl_context)
        return http.client.HTTPConnection(host, port, timeout=timeout)

    def _target(self, path: str) -> str:
        if path.startswith(("http://", "https://")):
            other = urllib.parse.urlsplit(path)
            if (other.scheme, other.hostname, other.port) != (self.parts.scheme, self.parts.hostname, self.parts.port):
                raise ValueError("refusing to follow a URL on a different origin")
            return other.path + (("?" + other.query) if other.query else "")
        if not path.startswith("/"):
            path = "/" + path
        if self.base_path and not path.startswith(self.base_path + "/"):
            path = self.base_path + path
        return path

    def request_raw(self, method: str, path: str, body: bytes | None = None,
                    headers: dict[str, str] | None = None, timeout: float | None = None,
                    auth: bool = True, max_response: int = MAX_JSON_RESPONSE) -> tuple[int, dict[str, str], bytes]:
        hdrs = {"User-Agent": f"asgend-node/{__version__}", "Accept": "application/json"}
        if auth and self.token:
            hdrs["Authorization"] = "Bearer " + self.token
        if headers:
            hdrs.update(headers)
        conn = self._connection(timeout if timeout is not None else self.timeout)
        try:
            conn.request(method, self._target(path), body=body, headers=hdrs)
            resp = conn.getresponse()
            data = resp.read(max_response + 1)
            if len(data) > max_response:
                raise CPUnavailable("control plane response too large")
            resp_headers = {k.lower(): v for k, v in resp.getheaders()}
            return resp.status, resp_headers, data
        except (OSError, http.client.HTTPException, ssl.SSLError) as exc:
            raise CPUnavailable(f"control plane request failed: {type(exc).__name__}: {exc}") from exc
        finally:
            conn.close()

    def request(self, method: str, path: str, payload: Any = None, timeout: float | None = None,
                auth: bool = True, raw_body: bytes | None = None,
                extra_headers: dict[str, str] | None = None) -> dict:
        headers = dict(extra_headers or {})
        body = raw_body
        if payload is not None:
            body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json"
        status, resp_headers, data = self.request_raw(method, path, body, headers, timeout, auth)
        parsed: dict = {}
        if data:
            try:
                value = json.loads(data.decode("utf-8"))
                if isinstance(value, dict):
                    parsed = value
            except (UnicodeDecodeError, json.JSONDecodeError):
                parsed = {}
        if 200 <= status < 300:
            return parsed
        err = parsed.get("error") if isinstance(parsed.get("error"), dict) else {}
        code = str(err.get("code") or "http_%d" % status)
        message = str(err.get("message") or "")
        retry_after = _parse_retry_after(resp_headers.get("retry-after"))
        if status == 401:
            raise Unauthorized(status, code, message, body=parsed)
        if status == 429 and code != "quota_exceeded":
            raise RateLimited(status, code, message, retry_after if retry_after is not None else 1.0, parsed)
        if status >= 500 or status == 408:
            raise CPUnavailable(f"control plane HTTP {status} {code}", retry_after)
        raise CPError(status, code, message, retry_after, parsed)

    # -- node channel ----------------------------------------------------------
    def enroll(self, enrollment_code: str, name: str, labels: list[str], protocol_version: int,
               core_version: str | None) -> dict:
        body = {"enrollment_code": enrollment_code, "name": name, "labels": labels,
                "protocol_version": protocol_version}
        if core_version:
            body["core_version"] = core_version
        return self.request("POST", "/v1/node/enroll", body, auth=False)

    def heartbeat(self, status: str, active_tasks: int, core_version: str | None) -> dict:
        body: dict[str, Any] = {"status": status, "active_tasks": int(active_tasks)}
        if core_version:
            body["core_version"] = core_version
        return self.request("POST", "/v1/node/heartbeat", body)

    def rotate_token(self) -> dict:
        return self.request("POST", "/v1/node/token/rotate", {})

    def lease_commands(self, max_commands: int, wait_ms: int) -> list[dict]:
        reply = self.request("POST", "/v1/node/commands/lease",
                             {"max": int(max_commands), "wait_ms": int(wait_ms)},
                             timeout=self.timeout + wait_ms / 1000.0)
        return [c for c in (reply.get("commands") or []) if isinstance(c, dict)]

    def ack_command(self, command_id: str, lease_id: str | None, outcome: str,
                    core_task_id: str | None = None, error: str | None = None) -> dict:
        body: dict[str, Any] = {"lease_id": lease_id, "outcome": outcome}
        if core_task_id:
            body["core_task_id"] = core_task_id
        if error:
            body["error"] = error[:1000]
        return self.request("POST", f"/v1/node/commands/{urllib.parse.quote(command_id, safe='')}/ack", body)

    def post_events(self, events: list[dict]) -> dict:
        return self.request("POST", "/v1/node/events", {"events": events})

    def snapshot(self, task_id: str, body: dict) -> dict:
        return self.request("POST", f"/v1/node/tasks/{urllib.parse.quote(task_id, safe='')}/snapshot", body)

    def artifact_init(self, body: dict) -> dict:
        return self.request("POST", "/v1/node/artifacts", body)

    def artifact_received(self, upload_url: str) -> int:
        reply = self.request("GET", upload_url)
        return int(reply.get("received") or 0)

    def artifact_put(self, upload_url: str, data: bytes, start: int, total: int) -> dict:
        end = start + len(data) - 1
        headers = {"Content-Type": "application/octet-stream",
                   "Content-Range": f"bytes {start}-{end}/{total}"}
        return self.request("PUT", upload_url, raw_body=data, extra_headers=headers,
                            timeout=self.timeout + 60)

    def artifact_complete(self, artifact_id: str) -> dict:
        return self.request("POST", f"/v1/node/artifacts/{urllib.parse.quote(artifact_id, safe='')}/complete", {})
