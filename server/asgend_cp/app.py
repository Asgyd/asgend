"""Control Plane application: request dispatch, auth/RBAC, shared helpers,
event emission, command expiry and maintenance jobs.

Endpoint handlers live in the api_* mixins.
"""

from __future__ import annotations

import ipaddress
import json
import logging
import threading
import time
import traceback

from . import API_VERSION, __version__
from .api_artifacts import ArtifactApi
from .api_node import NodeApi
from .api_user import UserApi
from .blobstore import BlobStore
from .config import Config
from .db import Storage, open_storage
from .http import ApiError, Request, Response, Router
from .logs import get_logger, log
from .metrics import Metrics
from .migrations import LATEST_VERSION
from .ratelimit import RateLimiter
from .util import (OBJECT_ID_RE, REQUEST_ID_RE, Clock, dumps, new_id, rfc3339,
                   sha256_hex)

ROLE_RANK = {"viewer": 1, "operator": 2, "owner": 3}
TASK_STATES = ("pending_delivery", "delivered", "queued", "running", "waiting_input", "completed",
               "completed_without_model", "failed", "cancelled", "delivery_failed")
TERMINAL_STATES = ("completed", "completed_without_model", "failed", "cancelled", "delivery_failed")
ACTIVE_STATES = ("pending_delivery", "delivered", "queued", "running", "waiting_input")
NODE_REPORTED_STATES = ("delivered", "queued", "running", "waiting_input", "completed",
                        "completed_without_model", "failed", "cancelled")
COMMAND_TYPES = ("submit", "answer", "cancel", "retry")
COMMAND_STATES = ("pending", "leased", "acked", "rejected", "expired")


class Principal:
    __slots__ = ("kind", "token_hash", "token_id", "user_id", "node_id", "workspace_id", "name")

    def __init__(self, kind, token_hash=None, token_id=None, user_id=None, node_id=None,
                 workspace_id=None, name=None):
        self.kind = kind
        self.token_hash = token_hash
        self.token_id = token_id
        self.user_id = user_id
        self.node_id = node_id
        self.workspace_id = workspace_id
        self.name = name

    @property
    def actor(self) -> str:
        return self.user_id or self.node_id or "system"


class Notifier:
    """Wakes long-polls after commits (single active instance)."""

    def __init__(self):
        self.cv = threading.Condition()
        self.version = 0
        self.stopping = False

    def notify(self):
        with self.cv:
            self.version += 1
            self.cv.notify_all()

    def wait(self, since: int, timeout: float) -> None:
        with self.cv:
            if self.version == since and not self.stopping and timeout > 0:
                self.cv.wait(timeout)

    def stop(self):
        with self.cv:
            self.stopping = True
            self.cv.notify_all()


def _id_pat(name: str) -> str:
    return f"(?P<{name}>{OBJECT_ID_RE})"


class App(UserApi, NodeApi, ArtifactApi):
    TASK_STATES_ALL = TASK_STATES
    TERMINAL = TERMINAL_STATES
    NODE_REPORTED_STATES = NODE_REPORTED_STATES

    def __init__(self, config: Config, storage: Storage | None = None, clock: Clock | None = None):
        self.config = config
        self.clock = clock or Clock()
        self.storage = storage or open_storage(config.db_url, config.db_password, config.db_pool_size)
        self.blobs = BlobStore(config.data_dir)
        self.blob_lock = threading.Lock()
        self.events_notifier = Notifier()
        self.commands_notifier = Notifier()
        self.rl = RateLimiter()
        self.metrics = Metrics()
        self.log = get_logger()
        self.access_log = get_logger("access")
        self.stopping = False
        self._maint_stop = threading.Event()
        self._maint_thread = None
        self.router = Router()
        self._register_routes()

    # ------------------------------------------------------------------ setup
    def _register_routes(self) -> None:
        r = self.router
        ws, task, node, art, cmd = (_id_pat("ws"), _id_pat("task"), _id_pat("node"),
                                    _id_pat("art"), _id_pat("cmd"))
        r.add("GET", "/v1/health", self.h_health, "none", "health")
        r.add("GET", "/v1/ready", self.h_ready, "none", "ready")
        r.add("GET", "/v1/metrics", self.h_metrics, "metrics", "metrics")
        r.add("GET", "/v1/me", self.h_me, "user", "me")
        r.add("POST", "/v1/auth/logout", self.h_logout, "user", "auth_logout")
        r.add("POST", f"/v1/workspaces/{ws}/pairing-codes", self.h_pairing_create, "user", "pairing_create")
        r.add("POST", "/v1/pairing/redeem", self.h_pairing_redeem, "none", "pairing_redeem")
        r.add("GET", f"/v1/workspaces/{ws}/nodes", self.h_nodes_list, "user", "nodes_list")
        r.add("POST", f"/v1/workspaces/{ws}/node-enrollments", self.h_enrollment_create, "user",
              "enrollment_create")
        r.add("POST", f"/v1/nodes/{node}/revoke", self.h_node_revoke, "user", "node_revoke")
        r.add("POST", f"/v1/workspaces/{ws}/tasks", self.h_task_submit, "user", "task_submit")
        r.add("GET", f"/v1/workspaces/{ws}/tasks", self.h_tasks_list, "user", "tasks_list")
        r.add("GET", f"/v1/tasks/{task}", self.h_task_get, "user", "task_get")
        r.add("GET", f"/v1/tasks/{task}/events", self.h_task_events, "user", "task_events")
        r.add("GET", f"/v1/workspaces/{ws}/events", self.h_ws_events, "user", "workspace_events")
        r.add("POST", f"/v1/tasks/{task}/answer", self.h_task_answer, "user", "task_answer")
        r.add("POST", f"/v1/tasks/{task}/cancel", self.h_task_cancel, "user", "task_cancel")
        r.add("POST", f"/v1/tasks/{task}/retry", self.h_task_retry, "user", "task_retry")
        r.add("GET", f"/v1/tasks/{task}/commands", self.h_task_commands, "user", "task_commands")
        r.add("GET", f"/v1/tasks/{task}/artifacts", self.h_task_artifacts, "user", "task_artifacts")
        r.add("GET", f"/v1/artifacts/{art}", self.h_artifact_get, "user", "artifact_get")
        r.add("POST", f"/v1/artifacts/{art}/download-ticket", self.h_download_ticket, "user",
              "download_ticket")
        r.add("GET", "/v1/downloads/(?P<ticket>[A-Za-z0-9_\\-]{1,128})", self.h_download, "ticket",
              "download")
        r.add("GET", f"/v1/workspaces/{ws}/members", self.h_members_get, "user", "members_get")
        r.add("PUT", f"/v1/workspaces/{ws}/members", self.h_members_put, "user", "members_put")
        r.add("GET", f"/v1/workspaces/{ws}/audit", self.h_audit, "user", "audit")
        # node channel
        r.add("POST", "/v1/node/enroll", self.h_node_enroll, "none", "node_enroll")
        r.add("POST", "/v1/node/heartbeat", self.h_node_heartbeat, "node", "node_heartbeat")
        r.add("POST", "/v1/node/token/rotate", self.h_node_rotate, "node", "node_token_rotate")
        r.add("POST", "/v1/node/commands/lease", self.h_commands_lease, "node", "node_commands_lease")
        r.add("POST", f"/v1/node/commands/{cmd}/ack", self.h_command_ack, "node", "node_command_ack")
        r.add("POST", "/v1/node/events", self.h_node_events, "node", "node_events")
        r.add("POST", f"/v1/node/tasks/{task}/snapshot", self.h_node_snapshot, "node", "node_snapshot")
        r.add("POST", "/v1/node/artifacts", self.h_artifact_init, "node", "node_artifact_init")
        r.add("PUT", f"/v1/node/artifacts/{art}/content", self.h_artifact_put, "node",
              "node_artifact_put", body_limit="chunk")
        r.add("GET", f"/v1/node/artifacts/{art}/content", self.h_artifact_received, "node",
              "node_artifact_received")
        r.add("POST", f"/v1/node/artifacts/{art}/complete", self.h_artifact_complete, "node",
              "node_artifact_complete")

    def start_background(self) -> None:
        if self._maint_thread is None:
            self._maint_thread = threading.Thread(target=self._maintenance_loop,
                                                  name="asgend-cp-maintenance", daemon=True)
            self._maint_thread.start()

    def begin_shutdown(self) -> None:
        self.stopping = True
        self.events_notifier.stop()
        self.commands_notifier.stop()
        self._maint_stop.set()

    def close(self) -> None:
        self.begin_shutdown()
        if self._maint_thread is not None:
            self._maint_thread.join(timeout=5)
        self.storage.close()

    # --------------------------------------------------------------- dispatch
    def dispatch(self, handler) -> None:
        start = time.monotonic()
        req = Request(handler)
        rid = req.header("X-Request-ID")
        req.request_id = rid if rid and REQUEST_ID_RE.match(rid) else new_id("req_")
        resp = None
        try:
            req.client_ip = self._client_ip(req)
            self._check_host(req)
            if req.method == "OPTIONS":
                resp = self._preflight(req)
            else:
                route, params, allowed = self.router.match(req.method, req.raw_path)
                if route is None:
                    if allowed:
                        raise ApiError("invalid_request", "method not allowed", status=405,
                                       headers={"Allow": ", ".join(sorted(set(allowed)))})
                    raise ApiError("not_found", "no such endpoint")
                req.route = route
                req.params = params
                self._authenticate(req, route)
                resp = route.fn(req)
        except ApiError as e:
            resp = self._error_response(req, e)
        except Exception as e:  # pragma: no cover - defensive
            frames = [f"{fs.filename.rsplit('/', 1)[-1].rsplit(chr(92), 1)[-1]}:{fs.lineno}:{fs.name}"
                      for fs in traceback.extract_tb(e.__traceback__)][-6:]
            log(self.log, logging.ERROR, "unhandled error", request_id=req.request_id,
                exc_type=type(e).__name__, frames=frames)
            resp = self._error_response(req, ApiError("internal", "internal server error"))
        self._send(handler, req, resp)
        dur = (time.monotonic() - start) * 1000.0
        route_name = req.route.name if req.route else "unmatched"
        self.metrics.inc("asgend_cp_http_requests_total",
                         {"route": route_name, "method": req.method, "status": str(resp.status)},
                         help="HTTP requests by route template and status")
        p = req.principal
        log(self.access_log, logging.INFO, "request", request_id=req.request_id, method=req.method,
            route=route_name, status=resp.status, duration_ms=round(dur, 2),
            client_ip=req.client_ip, principal=(p.actor if p else None))

    def _error_response(self, req: Request, e: ApiError) -> Response:
        body = {"error": {"code": e.code, "message": e.message, "request_id": req.request_id,
                          "details": e.details}}
        return Response.json(body, e.status, headers=e.headers)

    def _send(self, handler, req: Request, resp: Response) -> None:
        headers = {
            "X-ASGEND-API-Version": str(API_VERSION),
            "X-Request-ID": req.request_id,
            "X-Content-Type-Options": "nosniff",
            "X-Frame-Options": "DENY",
            "Referrer-Policy": "no-referrer",
            "Content-Security-Policy": "default-src 'none'; frame-ancestors 'none'; sandbox",
            "Cross-Origin-Resource-Policy": "same-origin",
            "Cache-Control": "no-store",
        }
        if self.config.tls_enabled or self.config.hsts:
            headers["Strict-Transport-Security"] = "max-age=31536000; includeSubDomains"
        headers.update(self._cors_headers(req))
        headers.update(resp.headers)
        if resp.stream is not None:
            headers["Content-Length"] = str(resp.length or 0)
        else:
            headers["Content-Length"] = str(len(resp.body))
        has_body = req.header("Content-Length") not in ("", "0") or "chunked" in req.header(
            "Transfer-Encoding").lower()
        if (has_body and not req.body_consumed) or self.stopping:
            handler.close_connection = True
        if handler.close_connection:
            headers["Connection"] = "close"
        try:
            handler.send_response(resp.status)
            for k, v in headers.items():
                handler.send_header(k, v)
            handler.end_headers()
            if req.method == "HEAD":
                return
            if resp.stream is not None:
                remaining = resp.length or 0
                while remaining > 0:
                    buf = resp.stream.read(min(256 * 1024, remaining))
                    if not buf:
                        break
                    handler.wfile.write(buf)
                    remaining -= len(buf)
            else:
                handler.wfile.write(resp.body)
        except OSError:
            handler.close_connection = True
        finally:
            if resp.stream is not None:
                try:
                    resp.stream.close()
                except OSError:
                    pass

    # ----------------------------------------------------- host, cors, proxies
    def _client_ip(self, req: Request) -> str:
        peer = req.peer_ip
        if not self.config.trusted_proxies:
            return peer
        try:
            nets = [ipaddress.ip_network(n, strict=False) for n in self.config.trusted_proxies]

            def trusted(ip):
                try:
                    a = ipaddress.ip_address(ip)
                except ValueError:
                    return False
                return any(a in n for n in nets)
            if not trusted(peer):
                return peer
            xff = [x.strip() for x in req.header("X-Forwarded-For").split(",") if x.strip()]
            for ip in reversed(xff):
                if not trusted(ip):
                    return ip
        except ValueError:
            pass
        return peer

    def _check_host(self, req: Request) -> None:
        allowed = self.config.allowed_hosts
        if not allowed:
            return
        host = req.header("Host").strip().lower()
        if host.startswith("["):
            host = host[1:].split("]", 1)[0]
        elif host.count(":") == 1:
            host = host.split(":", 1)[0]
        for a in allowed:
            a = a.lower()
            if a == "*" or a == host or (a.startswith("*.") and host.endswith(a[1:])):
                return
        raise ApiError("invalid_request", "host not allowed")

    def _origin_allowed(self, origin: str) -> bool:
        return bool(origin) and (origin in self.config.cors_origins or "*" in self.config.cors_origins)

    def _cors_headers(self, req: Request) -> dict:
        origin = req.header("Origin")
        if not self._origin_allowed(origin):
            return {}
        return {
            "Access-Control-Allow-Origin": origin,
            "Vary": "Origin",
            "Access-Control-Expose-Headers": "X-Request-ID, X-ASGEND-API-Version, X-Checksum-SHA256, "
                                             "Content-Range, Content-Disposition, Retry-After",
        }

    def _preflight(self, req: Request) -> Response:
        if not self._origin_allowed(req.header("Origin")):
            raise ApiError("forbidden", "origin not allowed")
        return Response(204, b"", {
            "Access-Control-Allow-Methods": "GET, POST, PUT, OPTIONS",
            "Access-Control-Allow-Headers": "Authorization, Content-Type, Content-Range, X-Request-ID",
            "Access-Control-Max-Age": "600",
        })

    # ------------------------------------------------------------ auth & RBAC
    def _bearer(self, req: Request) -> str:
        h = req.header("Authorization")
        if h[:7].lower() == "bearer ":
            return h[7:].strip()
        return ""

    def rate_limit(self, key: str, per_minute: float, burst: float, scope: str) -> None:
        wait = self.rl.hit(key, per_minute, burst)
        if wait:
            self.metrics.inc("asgend_cp_rate_limited_total", {"scope": scope},
                             help="Requests rejected by rate limiting")
            raise ApiError("rate_limited", "too many requests", {"retry_after_seconds": int(wait)},
                           headers={"Retry-After": str(int(wait))})

    def limit_auth_endpoint(self, req: Request) -> None:
        """Stricter per-IP limit for pairing/enrollment/credential endpoints."""
        self.rate_limit("authep:" + req.client_ip, self.config.auth_rate_limit_per_minute,
                        self.config.auth_rate_limit_burst, "auth")

    def _auth_fail(self, req: Request, reason: str) -> ApiError:
        self.metrics.inc("asgend_cp_auth_failures_total", {"reason": reason},
                         help="Failed authentication attempts")
        self.rl.hit("authfail:" + req.client_ip, self.config.failed_auth_per_minute,
                    self.config.failed_auth_per_minute)
        log(self.log, logging.WARNING, "authentication failed", request_id=req.request_id,
            reason=reason, client_ip=req.client_ip)
        return ApiError("unauthorized", "missing, invalid or revoked credentials",
                        headers={"WWW-Authenticate": 'Bearer realm="asgend"'})

    def _check_auth_lockout(self, req: Request) -> None:
        wait = self.rl.peek_blocked("authfail:" + req.client_ip, self.config.failed_auth_per_minute,
                                    self.config.failed_auth_per_minute)
        if wait:
            self.metrics.inc("asgend_cp_rate_limited_total", {"scope": "auth_failures"},
                             help="Requests rejected by rate limiting")
            raise ApiError("rate_limited", "too many failed authentication attempts",
                           {"retry_after_seconds": int(wait)}, headers={"Retry-After": str(int(wait))})

    def _authenticate(self, req: Request, route) -> None:
        if route.auth == "none":
            return
        if route.auth == "ticket":
            self.rate_limit("ip:" + req.client_ip, self.config.rate_limit_per_minute,
                            self.config.rate_limit_burst, "api")
            return
        if route.auth == "metrics":
            tok = self._bearer(req)
            want = self.config.metrics_token
            if not want or not tok or not _consteq(tok, want):
                raise self._auth_fail(req, "metrics")
            req.principal = Principal("metrics")
            return
        self._check_auth_lockout(req)
        tok = self._bearer(req)
        if not tok:
            raise self._auth_fail(req, "missing")
        p = self.lookup_token(tok, route.auth)
        if p is None:
            raise self._auth_fail(req, "invalid")
        req.principal = p
        self.rate_limit("tok:" + p.token_hash[:32], self.config.rate_limit_per_minute,
                        self.config.rate_limit_burst, "api")

    def lookup_token(self, token: str, kind: str) -> Principal | None:
        prefix = {"user": "ucp_", "node": "ncp_"}[kind]
        if not token.startswith(prefix) or len(token) > 200:
            return None
        h = sha256_hex(token)
        now = self.clock.now_ms()
        with self.storage.read() as t:
            row = t.q1("SELECT * FROM tokens WHERE token_hash = ?", (h,))
            if (row is None or row["kind"] != kind or row["revoked_at"] is not None
                    or (row["expires_at"] is not None and row["expires_at"] <= now)):
                return None
            if kind == "user":
                u = t.q1("SELECT user_id, name FROM users WHERE user_id = ? AND disabled = 0",
                         (row["user_id"],))
                if u is None:
                    return None
                p = Principal("user", h, row["token_id"], user_id=u["user_id"], name=u["name"])
            else:
                n = t.q1("SELECT node_id, workspace_id, name, revoked_at FROM nodes WHERE node_id = ?",
                         (row["node_id"],))
                if n is None or n["revoked_at"] is not None:
                    return None
                p = Principal("node", h, row["token_id"], node_id=n["node_id"],
                              workspace_id=n["workspace_id"], name=n["name"])
        if row["last_used_at"] is None or now - row["last_used_at"] > 60_000:
            try:
                with self.storage.tx() as t:
                    t.x("UPDATE tokens SET last_used_at = ? WHERE token_hash = ?", (now, h))
            except Exception:
                pass
        return p

    def role_in(self, t, user_id: str, ws_id: str) -> str | None:
        row = t.q1("SELECT role FROM memberships WHERE workspace_id = ? AND user_id = ?",
                   (ws_id, user_id))
        return row["role"] if row else None

    def require_role(self, req: Request, t, ws_id: str, min_role: str, action: str = "") -> str:
        """Cross-workspace -> not_found (no existence oracle); insufficient role -> forbidden."""
        role = self.role_in(t, req.principal.user_id, ws_id)
        if role is None:
            raise ApiError("not_found", "not found")
        if ROLE_RANK[role] < ROLE_RANK[min_role]:
            if action:
                self.audit_now(ws_id, req.principal.actor, action, None, "denied", req,
                               {"role": role, "required": min_role})
            raise ApiError("forbidden", f"requires role {min_role}")
        return role

    # ---------------------------------------------------------------- helpers
    def lock_ws(self, t, ws_id: str) -> None:
        """Serialize writers per workspace (PG row lock; SQLite already holds the DB lock)."""
        t.q("SELECT event_seq FROM workspaces WHERE workspace_id = ?" + t.for_update, (ws_id,))

    def emit(self, t, ws_id: str, task_id: str, etype: str, payload: dict, node_id=None, seq=None,
             occurred_at=None) -> int:
        now = self.clock.now_ms()
        row = t.q1("UPDATE workspaces SET event_seq = event_seq + 1 WHERE workspace_id = ? "
                   "RETURNING event_seq", (ws_id,))
        eid = int(row["event_seq"])
        t.x("INSERT INTO events (workspace_id, event_id, task_id, node_id, seq, type, occurred_at, "
            "received_at, schema_version, payload_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1, ?)",
            (ws_id, eid, task_id, node_id, seq, etype, occurred_at if occurred_at is not None else now,
             now, dumps(payload)))
        t.x("UPDATE tasks SET last_event_id = ?, updated_at = ? WHERE task_id = ?", (eid, now, task_id))
        if self.events_notifier.notify not in t.after_commit:
            t.after_commit.append(self.events_notifier.notify)
        return eid

    def audit(self, t, ws_id, actor: str, action: str, target, result: str, req: Request | None,
              details: dict | None = None) -> None:
        t.x("INSERT INTO audit (audit_id, workspace_id, actor, action, target, result, request_id, "
            "created_at, details_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (new_id("aud_"), ws_id, actor, action, target, result,
             req.request_id if req else None, self.clock.now_ms(), dumps(details or {})))

    def audit_now(self, ws_id, actor, action, target, result, req, details=None) -> None:
        try:
            with self.storage.tx() as t:
                self.audit(t, ws_id, actor, action, target, result, req, details)
        except Exception:
            pass

    def idem_lookup(self, t, ws_id: str, kind: str, key: str, fingerprint: str):
        """Returns result_id for an exact replay, raises conflict on mismatch, None if new."""
        row = t.q1("SELECT fingerprint, result_id FROM idempotency WHERE workspace_id = ? AND kind = ? "
                   "AND idem_key = ?", (ws_id, kind, key))
        if row is None:
            return None
        if row["fingerprint"] != fingerprint:
            raise ApiError("conflict", "idempotency_key reused with a different request",
                           {"idempotency_key": key})
        return row["result_id"]

    def idem_store(self, t, ws_id, kind, key, fingerprint, result_id) -> None:
        t.x("INSERT INTO idempotency (workspace_id, kind, idem_key, fingerprint, result_id, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?)", (ws_id, kind, key, fingerprint, result_id, self.clock.now_ms()))

    # ----------------------------------------------------------- serializers
    def node_status(self, n: dict, now: int | None = None) -> str:
        now = self.clock.now_ms() if now is None else now
        if n.get("revoked_at") is not None:
            return "revoked"
        if n.get("lease_expires_at") is not None and n["lease_expires_at"] > now:
            return "online"
        return "offline"

    def node_out(self, n: dict) -> dict:
        return {
            "node_id": n["node_id"], "workspace_id": n["workspace_id"], "name": n["name"],
            "labels": json.loads(n["labels_json"] or "[]"), "protocol_version": n["protocol_version"],
            "core_version": n["core_version"], "status": self.node_status(n),
            "last_seen_at": rfc3339(n["last_seen_at"]), "lease_expires_at": rfc3339(n["lease_expires_at"]),
            "created_at": rfc3339(n["created_at"]),
            "node_state": n.get("node_state"), "active_tasks": n.get("active_tasks") or 0,
        }

    TASK_SELECT = ("SELECT t.*, n.lease_expires_at AS n_lease, n.revoked_at AS n_revoked FROM tasks t "
                   "JOIN nodes n ON n.node_id = t.node_id")

    def task_out(self, r: dict) -> dict:
        now = self.clock.now_ms()
        online = r.get("n_revoked") is None and r.get("n_lease") is not None and r["n_lease"] > now
        return {
            "task_id": r["task_id"], "workspace_id": r["workspace_id"], "node_id": r["node_id"],
            "core_task_id": r["core_task_id"], "title": r["title"], "agent": r["agent"],
            "session_id": r["session_id"], "priority": r["priority"], "state": r["state"],
            "question": json.loads(r["question_json"]) if r["question_json"] else None,
            "attempts": r["attempts"], "created_at": rfc3339(r["created_at"]),
            "updated_at": rfc3339(r["updated_at"]), "last_event_id": r["last_event_id"],
            "stale": not online, "idempotency_key": r["idempotency_key"],
            "result_summary": r["result_summary"], "error": r["error"],
        }

    def command_out(self, c: dict) -> dict:
        return {
            "command_id": c["command_id"], "task_id": c["task_id"], "workspace_id": c["workspace_id"],
            "node_id": c["node_id"], "type": c["type"], "payload": json.loads(c["payload_json"]),
            "idempotency_key": c["idempotency_key"], "state": c["state"],
            "created_at": rfc3339(c["created_at"]), "expires_at": rfc3339(c["expires_at"]),
            "lease_id": c["lease_id"], "lease_expires_at": rfc3339(c["lease_expires_at"]),
            "delivery_attempts": c["delivery_attempts"], "error": c["error"],
        }

    def event_out(self, e: dict) -> dict:
        return {
            "event_id": int(e["event_id"]), "task_id": e["task_id"], "workspace_id": e["workspace_id"],
            "node_id": e["node_id"], "seq": None if e["seq"] is None else int(e["seq"]),
            "type": e["type"], "occurred_at": rfc3339(e["occurred_at"]),
            "received_at": rfc3339(e["received_at"]), "schema_version": e["schema_version"],
            "payload": json.loads(e["payload_json"]),
        }

    def artifact_out(self, a: dict) -> dict:
        return {
            "artifact_id": a["artifact_id"], "task_id": a["task_id"], "workspace_id": a["workspace_id"],
            "node_id": a["node_id"], "name": a["name"], "mime": a["mime"], "size": int(a["size"]),
            "sha256": a["sha256"], "state": a["state"], "created_at": rfc3339(a["created_at"]),
            "expires_at": rfc3339(a["expires_at"]),
        }

    def audit_out(self, a: dict) -> dict:
        return {
            "audit_id": a["audit_id"], "workspace_id": a["workspace_id"], "actor": a["actor"],
            "action": a["action"], "target": a["target"], "result": a["result"],
            "request_id": a["request_id"], "at": rfc3339(a["created_at"]),
            "details": json.loads(a["details_json"] or "{}"),
        }

    # ------------------------------------------------------ command expiry
    def expire_command(self, t, c: dict, reason: str) -> None:
        """Mark a pending/leased command expired; an undelivered submit fails its task."""
        now = self.clock.now_ms()
        t.x("UPDATE commands SET state = 'expired', error = ?, lease_id = NULL, lease_expires_at = NULL "
            "WHERE command_id = ?", (reason, c["command_id"]))
        self.metrics.inc("asgend_cp_commands_expired_total", {"type": c["type"]},
                         help="Commands expired before acknowledgement")
        self.emit(t, c["workspace_id"], c["task_id"], "command_status",
                  {"command_id": c["command_id"], "type": c["type"], "state": "expired", "error": reason})
        if c["type"] == "submit":
            task = t.q1("SELECT state FROM tasks WHERE task_id = ?", (c["task_id"],))
            if task and task["state"] == "pending_delivery":
                msg = ("command TTL expired before the node received it" if reason == "ttl_expired"
                       else "node revoked before delivery")
                t.x("UPDATE tasks SET state = 'delivery_failed', error = ?, updated_at = ? "
                    "WHERE task_id = ?", (msg, now, c["task_id"]))
                self.emit(t, c["workspace_id"], c["task_id"], "delivery_failed",
                          {"command_id": c["command_id"], "reason": reason, "state": "delivery_failed"})

    def expire_due_commands(self, node_id: str | None = None) -> int:
        """TTL sweep. Leased commands whose lease is still active are not expired yet."""
        now = self.clock.now_ms()
        sql = ("SELECT command_id, workspace_id FROM commands WHERE state IN ('pending', 'leased') "
               "AND expires_at <= ? AND (lease_expires_at IS NULL OR lease_expires_at <= ?)")
        params = [now, now]
        if node_id:
            sql += " AND node_id = ?"
            params.append(node_id)
        with self.storage.read() as t:
            due = t.q(sql + " ORDER BY command_id LIMIT 500", params)
        n = 0
        for d in due:
            with self.storage.tx() as t:
                self.lock_ws(t, d["workspace_id"])
                c = t.q1("SELECT * FROM commands WHERE command_id = ?" + t.for_update, (d["command_id"],))
                if (c and c["state"] in ("pending", "leased") and c["expires_at"] <= now
                        and (c["lease_expires_at"] is None or c["lease_expires_at"] <= now)):
                    self.expire_command(t, c, "ttl_expired")
                    n += 1
        return n

    # ------------------------------------------------------------ maintenance
    def run_retention(self) -> dict:
        """Expire artifacts/uploads, delete expired tickets/codes/old tokens."""
        now = self.clock.now_ms()
        stats = {"artifacts_expired": 0, "uploads_abandoned": 0, "tickets_deleted": 0,
                 "codes_deleted": 0, "tokens_deleted": 0, "tmp_orphans_deleted": 0}
        abandon_before = now - int(self.config.upload_abandon_hours * 3600_000)
        with self.storage.read() as t:
            arts = t.q("SELECT artifact_id, workspace_id, state, blob_key FROM artifacts WHERE "
                       "(state = 'available' AND expires_at IS NOT NULL AND expires_at <= ?) OR "
                       "(state = 'uploading' AND created_at <= ?)", (now, abandon_before))
        for a in arts:
            with self.blob_lock:
                with self.storage.tx() as t:
                    n = t.x("UPDATE artifacts SET state = 'expired' WHERE artifact_id = ? AND state = ?",
                            (a["artifact_id"], a["state"]))
                    still = None
                    if n and a["blob_key"]:
                        still = t.q1("SELECT artifact_id FROM artifacts WHERE blob_key = ? AND state = "
                                     "'available'", (a["blob_key"],))
                if n:
                    if a["state"] == "uploading":
                        self.blobs.discard_tmp(a["artifact_id"])
                        stats["uploads_abandoned"] += 1
                    else:
                        if a["blob_key"] and still is None:
                            self.blobs.delete_blob(a["blob_key"])
                        stats["artifacts_expired"] += 1
        with self.storage.tx() as t:
            stats["tickets_deleted"] = t.x("DELETE FROM download_tickets WHERE expires_at <= ?", (now,))
            day_ago = now - 86_400_000
            stats["codes_deleted"] = (
                t.x("DELETE FROM pairing_codes WHERE expires_at <= ?", (day_ago,))
                + t.x("DELETE FROM enrollment_codes WHERE expires_at <= ?", (day_ago,)))
            stats["tokens_deleted"] = t.x(
                "DELETE FROM tokens WHERE expires_at IS NOT NULL AND expires_at <= ?",
                (now - 7 * 86_400_000,))
            uploading = {r["artifact_id"] for r in t.q(
                "SELECT artifact_id FROM artifacts WHERE state = 'uploading'")}
            if any(stats.values()):
                self.audit(t, None, "system", "retention.cleanup", None, "ok", None, dict(stats))
        for aid in self.blobs.list_tmp():
            if aid not in uploading:
                self.blobs.discard_tmp(aid)
                stats["tmp_orphans_deleted"] += 1
        self.rl.prune()
        return stats

    def run_maintenance(self) -> dict:
        expired = self.expire_due_commands()
        stats = self.run_retention()
        stats["commands_expired"] = expired
        return stats

    def _maintenance_loop(self) -> None:
        last_cleanup = 0.0
        while not self._maint_stop.wait(self.config.sweep_interval_seconds):
            try:
                self.expire_due_commands()
                if time.monotonic() - last_cleanup >= self.config.cleanup_interval_seconds:
                    last_cleanup = time.monotonic()
                    stats = self.run_retention()
                    if any(stats.values()):
                        log(self.log, logging.INFO, "retention cleanup", **stats)
            except Exception as e:  # pragma: no cover - defensive
                log(self.log, logging.ERROR, "maintenance failed", exc_type=type(e).__name__)

    # ------------------------------------------------------------- endpoints
    def h_health(self, req: Request) -> Response:
        return Response.json({"status": "ok", "version": __version__})

    def h_ready(self, req: Request) -> Response:
        if self.stopping:
            raise ApiError("unavailable", "shutting down")
        if not self.storage.ping():
            raise ApiError("unavailable", "database unreachable")
        try:
            v = self.storage.schema_version()
        except Exception:
            raise ApiError("unavailable", "database unreachable") from None
        if v != LATEST_VERSION:
            raise ApiError("unavailable", "schema not current",
                           {"schema_version": v, "expected": LATEST_VERSION})
        return Response.json({"status": "ready", "schema_version": v})

    def h_metrics(self, req: Request) -> Response:
        now = self.clock.now_ms()
        gauges = []
        with self.storage.read() as t:
            nodes = t.q("SELECT revoked_at, lease_expires_at FROM nodes")
            counts = {"online": 0, "offline": 0, "revoked": 0}
            for n in nodes:
                counts[self.node_status(n, now)] += 1
            gauges.append(("asgend_cp_nodes_online", "Execution nodes with a live lease",
                           [({}, counts["online"])]))
            gauges.append(("asgend_cp_nodes", "Execution nodes by status",
                           [({"status": k}, v) for k, v in sorted(counts.items())]))
            tasks = {r["state"]: r["c"] for r in t.q("SELECT state, COUNT(*) AS c FROM tasks GROUP BY state")}
            gauges.append(("asgend_cp_tasks", "Remote tasks by state",
                           [({"state": s}, tasks.get(s, 0)) for s in TASK_STATES]))
            cmds = {r["state"]: r["c"] for r in t.q("SELECT state, COUNT(*) AS c FROM commands GROUP BY state")}
            gauges.append(("asgend_cp_commands", "Commands by state",
                           [({"state": s}, cmds.get(s, 0)) for s in COMMAND_STATES]))
            ab = t.q1("SELECT COALESCE(SUM(size), 0) AS b, COUNT(*) AS c FROM artifacts WHERE state = 'available'")
            gauges.append(("asgend_cp_artifact_bytes", "Bytes of available artifacts", [({}, ab["b"])]))
            gauges.append(("asgend_cp_artifacts_available", "Available artifacts", [({}, ab["c"])]))
            usage = t.q("SELECT workspace_id, COALESCE(SUM(size), 0) AS b FROM artifacts WHERE state IN "
                        "('uploading', 'available') GROUP BY workspace_id")
            q = max(1, self.config.workspace_artifact_quota_bytes)
            gauges.append(("asgend_cp_workspace_artifact_quota_ratio",
                           "Artifact storage quota usage per workspace (0..1)",
                           [({"workspace_id": u["workspace_id"]}, round(u["b"] / q, 6)) for u in usage]))
            active = t.q("SELECT workspace_id, COUNT(*) AS c FROM tasks WHERE state IN "
                         "('pending_delivery', 'delivered', 'queued', 'running', 'waiting_input') "
                         "GROUP BY workspace_id")
            mq = max(1, self.config.max_active_tasks)
            gauges.append(("asgend_cp_workspace_active_tasks_quota_ratio",
                           "Active task quota usage per workspace (0..1)",
                           [({"workspace_id": a["workspace_id"]}, round(a["c"] / mq, 6)) for a in active]))
        gauges.append(("asgend_cp_schema_version", "Database schema version",
                       [({}, self.storage.schema_version())]))
        body = self.metrics.render(gauges).encode("utf-8")
        return Response(200, body, {"Content-Type": "text/plain; version=0.0.4; charset=utf-8"})


def _consteq(a: str, b: str) -> bool:
    import hmac
    return hmac.compare_digest(a.encode("utf-8"), b.encode("utf-8"))
