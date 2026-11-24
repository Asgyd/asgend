"""Client for the local ASGEND core JSON API (docs/PROTOCOL.md).

One JSON object + LF per connection; the core answers with one JSON object + LF and
closes the connection. Supports TCP and (where available) AF_UNIX sockets.
"""

from __future__ import annotations

import json
import socket
from dataclasses import dataclass
from typing import Any


class CoreError(Exception):
    """The core answered with ok=false (validation/business error)."""

    def __init__(self, message: str, reply: dict | None = None):
        super().__init__(message)
        self.reply = reply or {}

    @property
    def retryable(self) -> bool:
        return bool(self.reply.get("retryable"))


class CoreUnavailable(Exception):
    """Transport-level failure: connection refused, timeout, oversized or invalid reply."""


@dataclass
class CoreEndpoint:
    host: str = "127.0.0.1"
    port: int = 8765
    unix_socket: str | None = None
    auth_token: str | None = None
    timeout: float = 30.0
    max_response_bytes: int = 8 * 1024 * 1024


class CoreClient:
    def __init__(self, endpoint: CoreEndpoint):
        self.endpoint = endpoint

    # -- transport ---------------------------------------------------------
    def _connect(self) -> socket.socket:
        ep = self.endpoint
        if ep.unix_socket:
            if not hasattr(socket, "AF_UNIX"):
                raise CoreUnavailable("AF_UNIX sockets are not supported on this platform")
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(ep.timeout)
            try:
                sock.connect(ep.unix_socket)
            except OSError as exc:
                sock.close()
                raise CoreUnavailable(f"cannot connect to core unix socket: {exc}") from exc
            return sock
        try:
            return socket.create_connection((ep.host, ep.port), timeout=ep.timeout)
        except OSError as exc:
            raise CoreUnavailable(f"cannot connect to core {ep.host}:{ep.port}: {exc}") from exc

    def raw_request(self, request: dict[str, Any]) -> dict[str, Any]:
        payload = dict(request)
        if self.endpoint.auth_token:
            payload["auth_token"] = self.endpoint.auth_token
        data = (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        limit = self.endpoint.max_response_bytes
        sock = self._connect()
        try:
            sock.sendall(data)
            chunks: list[bytes] = []
            total = 0
            while True:
                try:
                    chunk = sock.recv(65536)
                except socket.timeout as exc:
                    raise CoreUnavailable("core response timed out") from exc
                except OSError as exc:
                    raise CoreUnavailable(f"core connection error: {exc}") from exc
                if not chunk:
                    break
                total += len(chunk)
                if total > limit:
                    raise CoreUnavailable(f"core response exceeds {limit} bytes")
                chunks.append(chunk)
                if chunk.endswith(b"\n"):
                    break
        except OSError as exc:  # sendall failures
            raise CoreUnavailable(f"core connection error: {exc}") from exc
        finally:
            try:
                sock.close()
            except OSError:
                pass
        raw = b"".join(chunks).strip()
        if not raw:
            raise CoreUnavailable("core closed the connection without a reply")
        try:
            reply = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise CoreUnavailable("core reply is not valid JSON") from exc
        if not isinstance(reply, dict):
            raise CoreUnavailable("core reply is not a JSON object")
        return reply

    def call(self, op: str, **fields: Any) -> dict[str, Any]:
        request = {"op": op}
        request.update({k: v for k, v in fields.items() if v is not None})
        reply = self.raw_request(request)
        if not reply.get("ok", False):
            raise CoreError(str(reply.get("error") or f"core op {op} failed"), reply)
        return reply

    # -- typed operations ----------------------------------------------------
    def ping(self) -> dict:
        return self.call("ping")

    def submit(self, task: str, idempotency_key: str, agent: str | None = None,
               session_id: str | None = None, priority: int | None = None) -> dict:
        return self.call("submit", task=task, idempotency_key=idempotency_key, agent=agent,
                         session_id=session_id, priority=priority)

    def status(self, task_id: str) -> dict:
        return self.call("status", task_id=task_id)

    def events(self, task_id: str, after_id: int, limit: int = 100) -> list[dict]:
        reply = self.call("events", task_id=task_id, after_id=int(after_id), limit=int(limit))
        events = reply.get("events") or []
        return [e for e in events if isinstance(e, dict)]

    def answer(self, task_id: str, answer: str, option_id: str | None = None) -> dict:
        return self.call("answer", task_id=task_id, answer=answer, option_id=option_id)

    def cancel(self, task_id: str) -> dict:
        return self.call("cancel", task_id=task_id)

    def retry_task(self, task_id: str) -> dict:
        return self.call("retry_task", task_id=task_id)

    def artifact_list(self, task_id: str) -> list[dict]:
        reply = self.call("artifact_list", task_id=task_id)
        return [a for a in (reply.get("artifacts") or []) if isinstance(a, dict)]

    def workspace_read_bytes(self, task_id: str, path: str, offset: int, limit: int) -> dict:
        return self.call("workspace_read_bytes", task_id=task_id, path=path, offset=int(offset),
                         limit=int(min(max(limit, 1), 1048576)))
