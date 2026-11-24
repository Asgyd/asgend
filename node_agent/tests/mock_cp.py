"""Minimal in-process Control Plane implementing the node channel (contract v1 section 7)
plus fault injection used by the unit tests. Not a reference server: just enough
semantics (leases, seq dedupe/gaps, snapshots, resumable uploads, checksums)."""

from __future__ import annotations

import hashlib
import json
import re
import secrets
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def iso(ts: float | None = None) -> str:
    dt = datetime.fromtimestamp(ts if ts is not None else time.time(), timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def new_id(prefix: str) -> str:
    alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
    return prefix + "".join(secrets.choice(alphabet) for _ in range(26))


class MockCP:
    def __init__(self, chunk_size: int = 8 * 1024 * 1024, lease_seconds: float = 60.0):
        self.lock = threading.RLock()
        self.chunk_size = chunk_size
        self.lease_seconds = lease_seconds
        self.enrollments: dict[str, dict] = {}
        self.nodes: dict[str, dict] = {}
        self.tokens: dict[str, str] = {}  # token -> node_id
        self.commands: dict[str, dict] = {}
        self.command_order: list[str] = []
        self.tasks: dict[str, dict] = {}
        self.artifacts: dict[str, dict] = {}
        self.heartbeats: list[dict] = []
        self.acks: list[dict] = []
        self.snapshots: list[dict] = []
        self.event_posts: list[list[dict]] = []
        self.requests: list[tuple[str, str]] = []
        self.auth_headers: list[str] = []
        # fault injection
        self.outage = False
        self.rate_limit_remaining = 0
        self.rate_limit_retry_after = "1"
        self.lose_put_response_after: int | None = None
        self.put_count = 0
        self.corrupt_uploads = False
        self._server: ThreadingHTTPServer | None = None

    # -------------------------------------------------------------- lifecycle
    def start(self) -> "MockCP":
        cp = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):  # silence
                pass

            def _send(self, status: int, body: dict | None = None, headers: dict | None = None):
                data = json.dumps(dict({"api_version": 1}, **(body or {}))).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("X-ASGEND-API-Version", "1")
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                self.end_headers()
                self.wfile.write(data)

            def _error(self, status: int, code: str, message: str = "", headers=None):
                self._send(status, {"error": {"code": code, "message": message, "request_id": "req-1",
                                              "details": {}}}, headers)

            def _body(self) -> bytes:
                n = int(self.headers.get("Content-Length") or 0)
                return self.rfile.read(n) if n else b""

            def _dispatch(self, method: str):
                body = self._body()
                with cp.lock:
                    cp.requests.append((method, self.path))
                    cp.auth_headers.append(self.headers.get("Authorization") or "")
                if cp.outage:
                    return self._error(503, "unavailable", "maintenance")
                if cp.rate_limit_remaining > 0:
                    cp.rate_limit_remaining -= 1
                    return self._error(429, "rate_limited", "slow down", {"Retry-After": cp.rate_limit_retry_after})
                try:
                    cp.route(self, method, self.path, body)
                except Exception as exc:  # pragma: no cover - debugging aid
                    self._error(500, "internal", repr(exc))

            def do_GET(self):
                self._dispatch("GET")

            def do_POST(self):
                self._dispatch("POST")

            def do_PUT(self):
                self._dispatch("PUT")

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._server.daemon_threads = True
        threading.Thread(target=self._server.serve_forever, args=(0.05,), daemon=True, name="mock-cp").start()
        return self

    @property
    def url(self) -> str:
        assert self._server is not None
        return "http://127.0.0.1:%d" % self._server.server_address[1]

    def stop(self) -> None:
        if self._server:
            self._server.shutdown()
            self._server.server_close()

    # ---------------------------------------------------------------- control
    def add_enrollment(self, workspace_id: str = "ws_TEST") -> str:
        code = "ENR-" + secrets.token_hex(6)
        self.enrollments[code] = {"workspace_id": workspace_id, "used": False}
        return code

    def revoke(self, node_id: str) -> None:
        with self.lock:
            self.nodes[node_id]["revoked"] = True

    def create_task(self, node_id: str, task: str = "do something", ttl: float = 3600, **payload) -> tuple[str, str]:
        with self.lock:
            task_id = new_id("rt_")
            self.tasks[task_id] = {"task_id": task_id, "node_id": node_id, "state": "pending_delivery",
                                   "expected_seq": 1, "events": [], "core_task_id": None}
            body = {"task": task}
            body.update(payload)
            cmd_id = self.add_command(node_id, task_id, "submit", body, ttl)
            return task_id, cmd_id

    def add_command(self, node_id: str, task_id: str, ctype: str, payload: dict, ttl: float = 3600,
                    expires_at: str | None = None) -> str:
        with self.lock:
            cmd_id = new_id("cmd_")
            now = time.time()
            self.commands[cmd_id] = {
                "command_id": cmd_id, "task_id": task_id, "workspace_id": "ws_TEST", "node_id": node_id,
                "type": ctype, "payload": payload, "idempotency_key": "k-" + cmd_id, "state": "pending",
                "created_at": iso(now), "expires_at": expires_at or iso(now + ttl), "lease_id": None,
                "lease_expires_at": None, "delivery_attempts": 0, "error": None, "_lease_deadline": 0.0,
                "outcome": None, "core_task_id": None}
            self.command_order.append(cmd_id)
            return cmd_id

    def redeliver(self, cmd_id: str) -> None:
        """Simulate a lost ack / lease expiry: the command becomes leasable again."""
        with self.lock:
            c = self.commands[cmd_id]
            c["state"] = "pending"
            c["lease_id"] = None

    def forget_events(self, task_id: str) -> None:
        """Simulate server-side event loss (e.g. restore from an older backup)."""
        with self.lock:
            self.tasks[task_id]["expected_seq"] = 1
            self.tasks[task_id]["events"] = []

    def task_events(self, task_id: str) -> list[dict]:
        with self.lock:
            return list(self.tasks[task_id]["events"])

    # ------------------------------------------------------------------ routes
    def _node(self, h) -> str | None:
        auth = h.headers.get("Authorization") or ""
        if not auth.startswith("Bearer "):
            return None
        node_id = self.tokens.get(auth[7:])
        if not node_id or self.nodes[node_id]["revoked"]:
            return None
        return node_id

    def route(self, h, method: str, path: str, raw: bytes) -> None:
        def js():
            return json.loads(raw.decode("utf-8")) if raw else {}

        if method == "POST" and path == "/v1/node/enroll":
            body = js()
            e = self.enrollments.get(body.get("enrollment_code"))
            if not e or e["used"]:
                return h._error(401, "unauthorized", "invalid enrollment code")
            e["used"] = True
            node_id = new_id("node_")
            token = "ncp_" + secrets.token_urlsafe(32)
            self.nodes[node_id] = {"node_id": node_id, "name": body.get("name"), "labels": body.get("labels"),
                                   "core_version": body.get("core_version"), "revoked": False,
                                   "workspace_id": e["workspace_id"]}
            self.tokens[token] = node_id
            return h._send(200, {"node_id": node_id, "workspace_id": e["workspace_id"], "node_token": token,
                                 "heartbeat_interval_seconds": 15, "lease_seconds": 45})
        node_id = self._node(h)
        if node_id is None:
            return h._error(401, "unauthorized", "invalid node token")

        if method == "POST" and path == "/v1/node/heartbeat":
            body = js()
            self.heartbeats.append(body)
            return h._send(200, {"server_time": iso(), "lease_expires_at": iso(time.time() + 45)})
        if method == "POST" and path == "/v1/node/token/rotate":
            token = "ncp_" + secrets.token_urlsafe(32)
            self.tokens[token] = node_id
            return h._send(200, {"node_token": token})
        if method == "POST" and path == "/v1/node/commands/lease":
            body = js()
            return h._send(200, {"commands": self._lease(node_id, int(body.get("max", 10)),
                                                         int(body.get("wait_ms", 0)))})
        m = re.fullmatch(r"/v1/node/commands/([^/]+)/ack", path)
        if method == "POST" and m:
            return self._ack(h, node_id, m.group(1), js())
        if method == "POST" and path == "/v1/node/events":
            return self._events(h, node_id, js())
        m = re.fullmatch(r"/v1/node/tasks/([^/]+)/snapshot", path)
        if method == "POST" and m:
            task = self.tasks.get(m.group(1))
            if not task or task["node_id"] != node_id:
                return h._error(404, "not_found")
            body = js()
            with self.lock:
                self.snapshots.append(dict(body, task_id=m.group(1)))
                task["expected_seq"] = int(body.get("last_seq", 0)) + 1
                task["state"] = body.get("state")
                task["snapshot"] = body
            return h._send(200, {"task_id": m.group(1)})
        if method == "POST" and path == "/v1/node/artifacts":
            return self._art_init(h, node_id, js())
        m = re.fullmatch(r"/v1/node/artifacts/([^/]+)/content", path)
        if m:
            art = self.artifacts.get(m.group(1))
            if not art or art["node_id"] != node_id or art["state"] != "uploading":
                return h._error(404, "not_found")
            if method == "GET":
                return h._send(200, {"received": len(art["data"])})
            if method == "PUT":
                return self._art_put(h, art, raw)
        m = re.fullmatch(r"/v1/node/artifacts/([^/]+)/complete", path)
        if method == "POST" and m:
            art = self.artifacts.get(m.group(1))
            if not art or art["node_id"] != node_id:
                return h._error(404, "not_found")
            data = bytes(art["data"])
            if self.corrupt_uploads and data:
                data = b"!" + data[1:]
            if len(data) != art["size"] or hashlib.sha256(data).hexdigest() != art["sha256"]:
                art["state"] = "rejected"
                art["data"] = bytearray()
                return h._error(422, "checksum_mismatch")
            art["state"] = "available"
            return h._send(200, {"artifact_id": art["artifact_id"], "state": "available"})
        return h._error(404, "not_found", path)

    def _lease(self, node_id: str, max_n: int, wait_ms: int) -> list[dict]:
        deadline = time.time() + min(wait_ms, 30000) / 1000.0
        while True:
            out = []
            with self.lock:
                now = time.time()
                for cmd_id in self.command_order:
                    c = self.commands[cmd_id]
                    if c["node_id"] != node_id:
                        continue
                    if c["state"] == "leased" and c["_lease_deadline"] < now:
                        c["state"] = "pending"
                    if c["state"] != "pending":
                        continue
                    c["state"] = "leased"
                    c["lease_id"] = new_id("lease_")
                    c["_lease_deadline"] = now + self.lease_seconds
                    c["lease_expires_at"] = iso(c["_lease_deadline"])
                    c["delivery_attempts"] += 1
                    out.append({k: v for k, v in c.items() if not k.startswith("_") and k not in
                                ("outcome", "core_task_id")})
                    if len(out) >= max_n:
                        break
            if out or time.time() >= deadline:
                return out
            time.sleep(0.02)

    def _ack(self, h, node_id: str, cmd_id: str, body: dict):
        with self.lock:
            c = self.commands.get(cmd_id)
            if not c or c["node_id"] != node_id:
                return h._error(404, "not_found")
            self.acks.append(dict(body, command_id=cmd_id))
            outcome = body.get("outcome")
            if c["state"] in ("acked", "rejected"):
                if c["outcome"] == outcome:
                    return h._send(200, {"command_id": cmd_id, "state": c["state"]})
                return h._error(409, "conflict", "different outcome")
            if body.get("lease_id") != c["lease_id"]:
                return h._error(409, "conflict", "stale lease")
            c["outcome"] = outcome
            c["state"] = "acked" if outcome == "accepted" else "rejected"
            c["error"] = body.get("error")
            c["core_task_id"] = body.get("core_task_id")
            task = self.tasks.get(c["task_id"])
            if task is not None and c["type"] == "submit" and outcome == "accepted":
                task["core_task_id"] = body.get("core_task_id")
                if task["state"] == "pending_delivery":
                    task["state"] = "delivered"
            return h._send(200, {"command_id": cmd_id, "state": c["state"]})

    def _events(self, h, node_id: str, body: dict):
        events = body.get("events") or []
        if len(events) > 500:
            return h._error(413, "payload_too_large")
        accepted = duplicates = 0
        gaps: dict[str, int] = {}
        with self.lock:
            self.event_posts.append(events)
            for ev in events:
                task = self.tasks.get(ev.get("task_id"))
                if not task or task["node_id"] != node_id:
                    return h._error(404, "not_found")
            for ev in events:
                task = self.tasks[ev["task_id"]]
                seq = int(ev["seq"])
                if seq < task["expected_seq"]:
                    duplicates += 1
                elif seq == task["expected_seq"] and ev["task_id"] not in gaps:
                    task["events"].append(ev)
                    task["expected_seq"] += 1
                    accepted += 1
                    if ev["type"] == "state_changed":
                        task["state"] = ev["payload"].get("state")
                else:
                    gaps.setdefault(ev["task_id"], task["expected_seq"])
        return h._send(200, {"accepted": accepted, "duplicates": duplicates,
                             "gaps": [{"task_id": t, "expected_seq": s} for t, s in gaps.items()]})

    def _art_init(self, h, node_id: str, body: dict):
        task = self.tasks.get(body.get("task_id"))
        if not task or task["node_id"] != node_id:
            return h._error(404, "not_found")
        name = body.get("name") or ""
        if "/" in name or "\\" in name or "\x00" in name or not name:
            return h._error(400, "invalid_request", "bad name")
        if int(body.get("size", 0)) > 64 * 1024 * 1024:
            return h._error(413, "payload_too_large")
        art_id = new_id("art_")
        self.artifacts[art_id] = {"artifact_id": art_id, "task_id": body["task_id"], "node_id": node_id,
                                  "name": name, "mime": body.get("mime"), "size": int(body["size"]),
                                  "sha256": body.get("sha256"), "state": "uploading", "data": bytearray(),
                                  "ranges": []}
        return h._send(201, {"artifact_id": art_id, "upload_url": f"/v1/node/artifacts/{art_id}/content",
                             "chunk_size": self.chunk_size})

    def _art_put(self, h, art: dict, raw: bytes):
        m = re.fullmatch(r"bytes (\d+)-(\d+)/(\d+)", h.headers.get("Content-Range") or "")
        if not m:
            return h._error(400, "invalid_request", "Content-Range required")
        start, end, total = map(int, m.groups())
        if total != art["size"] or end - start + 1 != len(raw) or end >= total:
            return h._error(400, "invalid_request", "bad range")
        if len(raw) > 8 * 1024 * 1024:
            return h._error(413, "payload_too_large")
        with self.lock:
            if start != len(art["data"]):
                return h._error(409, "conflict", "expected offset %d" % len(art["data"]))
            art["data"] += raw
            art["ranges"].append((start, end))
            self.put_count += 1
            lose = self.lose_put_response_after is not None and self.put_count > self.lose_put_response_after
            if lose:
                self.lose_put_response_after = None
        if lose:  # data stored, but the node never sees the reply
            return h._error(503, "unavailable", "response lost")
        return h._send(200, {"received": len(art["data"])})
