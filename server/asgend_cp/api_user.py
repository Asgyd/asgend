"""User/client endpoints (Bearer ucp_...)."""

from __future__ import annotations

import json
import time

from . import validate as V
from .http import ApiError, Response
from .util import (OBJECT_ID_RE, canonical_json, dumps, new_code, new_id, new_token,
                   normalize_code, rfc3339, sha256_hex)

import re

_ID_RE = re.compile("^" + OBJECT_ID_RE + "$")


class UserApi:
    # ----------------------------------------------------------------- me
    def h_me(self, req):
        p = req.principal
        with self.storage.read() as t:
            rows = t.q("SELECT w.workspace_id, w.name, m.role FROM memberships m JOIN workspaces w "
                       "ON w.workspace_id = m.workspace_id WHERE m.user_id = ? ORDER BY w.workspace_id",
                       (p.user_id,))
        return Response.json({
            "user": {"user_id": p.user_id, "name": p.name},
            "token_id": p.token_id,
            "workspaces": [{"workspace_id": r["workspace_id"], "name": r["name"], "role": r["role"]}
                           for r in rows],
        })

    def h_logout(self, req):
        p = req.principal
        now = self.clock.now_ms()
        with self.storage.tx() as t:
            t.x("UPDATE tokens SET revoked_at = ? WHERE token_hash = ? AND revoked_at IS NULL",
                (now, p.token_hash))
            self.audit(t, None, p.actor, "auth.logout", p.token_id, "ok", req)
        return Response.json({"revoked": True})

    # ------------------------------------------------------------- pairing
    def h_pairing_create(self, req):
        ws = req.params["ws"]
        with self.storage.read() as t:
            self.require_role(req, t, ws, "owner", "pairing.create")
        body = req.json(self.config.max_json_body_bytes)
        role = body.get("role", "viewer")
        if role not in ("owner", "operator", "viewer"):
            raise V.bad("role", "must be owner|operator|viewer")
        code = new_code(12)
        now = self.clock.now_ms()
        exp = now + self.config.pairing_code_ttl_seconds * 1000
        pid = new_id("pair_")
        with self.storage.tx() as t:
            t.x("INSERT INTO pairing_codes (pairing_id, code_hash, workspace_id, role, created_by, "
                "created_at, expires_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                (pid, sha256_hex(normalize_code(code)), ws, role, req.principal.user_id, now, exp))
            self.audit(t, ws, req.principal.actor, "pairing.create", pid, "ok", req, {"role": role})
        return Response.json({"pairing_id": pid, "code": code, "role": role, "expires_at": rfc3339(exp)},
                             201)

    def h_pairing_redeem(self, req):
        self.limit_auth_endpoint(req)
        body = req.json(self.config.max_json_body_bytes)
        code = V.req_str(body, "code", 64, 8)
        device = V.req_str(body, "device_name", 100, 1, required=False, default="device")
        now = self.clock.now_ms()
        h = sha256_hex(normalize_code(code))
        token = new_token("ucp_")
        with self.storage.tx() as t:
            row = t.q1("SELECT * FROM pairing_codes WHERE code_hash = ?" + t.for_update, (h,))
            if row is None or row["used_at"] is not None or row["expires_at"] <= now:
                ws = row["workspace_id"] if row else None
                reason = "unknown" if row is None else ("used" if row["used_at"] else "expired")
                self.audit(t, ws, "anonymous", "auth.pair", row["pairing_id"] if row else None,
                           "denied", req, {"reason": reason})
                deny = True
            else:
                deny = False
                uid = new_id("usr_")
                t.x("UPDATE pairing_codes SET used_at = ?, used_by = ? WHERE pairing_id = ? "
                    "AND used_at IS NULL", (now, uid, row["pairing_id"]))
                t.x("INSERT INTO users (user_id, name, created_at) VALUES (?, ?, ?)", (uid, device, now))
                t.x("INSERT INTO memberships (workspace_id, user_id, role, created_at) VALUES (?, ?, ?, ?)",
                    (row["workspace_id"], uid, row["role"], now))
                tid = new_id("tok_")
                t.x("INSERT INTO tokens (token_hash, token_id, kind, user_id, name, created_at) "
                    "VALUES (?, ?, 'user', ?, ?, ?)", (sha256_hex(token), tid, uid, device, now))
                self.audit(t, row["workspace_id"], uid, "auth.pair", row["pairing_id"], "ok", req,
                           {"role": row["role"], "token_id": tid})
        if deny:
            raise self._auth_fail(req, "pairing_code")
        return Response.json({"token": token, "user_id": uid, "workspace_id": row["workspace_id"],
                              "role": row["role"]}, 201)

    # --------------------------------------------------------------- nodes
    def h_nodes_list(self, req):
        ws = req.params["ws"]
        with self.storage.read() as t:
            self.require_role(req, t, ws, "viewer")
            rows = t.q("SELECT * FROM nodes WHERE workspace_id = ? ORDER BY node_id", (ws,))
        return Response.json({"nodes": [self.node_out(n) for n in rows]})

    def h_enrollment_create(self, req):
        ws = req.params["ws"]
        with self.storage.read() as t:
            self.require_role(req, t, ws, "owner", "node.enrollment_create")
        body = req.json(self.config.max_json_body_bytes)
        name = V.req_str(body, "name", 100, 1, required=False)
        labels = V.labels(body)
        code = new_code(24)
        now = self.clock.now_ms()
        exp = now + self.config.enrollment_code_ttl_seconds * 1000
        eid = new_id("enr_")
        with self.storage.tx() as t:
            t.x("INSERT INTO enrollment_codes (enrollment_id, code_hash, workspace_id, name, labels_json, "
                "created_by, created_at, expires_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (eid, sha256_hex(normalize_code(code)), ws, name, dumps(labels), req.principal.user_id,
                 now, exp))
            self.audit(t, ws, req.principal.actor, "node.enrollment_create", eid, "ok", req)
        return Response.json({"enrollment_id": eid, "enrollment_code": code, "expires_at": rfc3339(exp)},
                             201)

    def h_node_revoke(self, req):
        node_id = req.params["node"]
        with self.storage.read() as t:
            n = t.q1("SELECT * FROM nodes WHERE node_id = ?", (node_id,))
            if n is None:
                raise ApiError("not_found", "not found")
            self.require_role(req, t, n["workspace_id"], "owner", "node.revoke")
        now = self.clock.now_ms()
        with self.storage.tx() as t:
            self.lock_ws(t, n["workspace_id"])
            n = t.q1("SELECT * FROM nodes WHERE node_id = ?", (node_id,))
            if n["revoked_at"] is None:
                t.x("UPDATE nodes SET revoked_at = ?, lease_expires_at = NULL WHERE node_id = ?",
                    (now, node_id))
                t.x("UPDATE tokens SET revoked_at = ? WHERE node_id = ? AND revoked_at IS NULL",
                    (now, node_id))
                for c in t.q("SELECT * FROM commands WHERE node_id = ? AND state IN ('pending', 'leased') "
                             "ORDER BY command_id", (node_id,)):
                    self.expire_command(t, c, "node_revoked")
                self.audit(t, n["workspace_id"], req.principal.actor, "node.revoke", node_id, "ok", req)
            n = t.q1("SELECT * FROM nodes WHERE node_id = ?", (node_id,))
        return Response.json(self.node_out(n))

    # --------------------------------------------------------------- tasks
    def h_task_submit(self, req):
        ws = req.params["ws"]
        with self.storage.read() as t:
            self.require_role(req, t, ws, "operator", "task.submit")
        body = req.json(self.config.max_json_body_bytes)
        node_id = V.req_str(body, "node_id", 80)
        text = V.req_str(body, "task", 512 * 1024)
        if not text.strip():
            raise V.bad("task", "must not be blank")
        agent = V.req_str(body, "agent", 128, required=False)
        session_id = V.req_str(body, "session_id", 128, required=False)
        priority = V.req_int(body, "priority", 0, 9, required=False, default=5)
        key = V.idem_key(body)
        ttl = V.req_int(body, "ttl_seconds", 10, 86400, required=False,
                        default=self.config.default_command_ttl_seconds)
        title = V.req_str(body, "title", 200, required=False)
        if title is None:
            first = text.strip().splitlines()[0] if text.strip() else ""
            title = first[:200]
        fp = sha256_hex(canonical_json({"node_id": node_id, "task": text, "agent": agent,
                                        "session_id": session_id, "priority": priority,
                                        "ttl_seconds": ttl, "title": title}))
        now = self.clock.now_ms()
        with self.storage.tx() as t:
            self.lock_ws(t, ws)
            existing = self.idem_lookup(t, ws, "submit", key, fp)
            if existing:
                task = t.q1(self.TASK_SELECT + " WHERE t.task_id = ?", (existing,))
                return Response.json(self.task_out(task), 200)
            node = t.q1("SELECT * FROM nodes WHERE node_id = ? AND workspace_id = ?", (node_id, ws))
            if node is None:
                raise ApiError("not_found", "node not found", {"field": "node_id"})
            if node["revoked_at"] is not None:
                raise ApiError("conflict", "node is revoked", {"node_id": node_id})
            active = t.q1("SELECT COUNT(*) AS c FROM tasks WHERE workspace_id = ? AND state IN "
                          "('pending_delivery', 'delivered', 'queued', 'running', 'waiting_input')", (ws,))
            if active["c"] >= self.config.max_active_tasks:
                self.metrics.inc("asgend_cp_quota_rejections_total", {"quota": "active_tasks"},
                                 help="Requests rejected by quotas")
                raise ApiError("quota_exceeded", "too many active tasks in workspace",
                               {"quota": "active_tasks", "limit": self.config.max_active_tasks},
                               headers={"Retry-After": "30"})
            task_id = new_id("rt_")
            cmd_id = new_id("cmd_")
            t.x("INSERT INTO tasks (task_id, workspace_id, node_id, title, agent, session_id, priority, "
                "state, attempts, created_at, updated_at, idempotency_key) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, 'pending_delivery', 0, ?, ?, ?)",
                (task_id, ws, node_id, title, agent, session_id, priority, now, now, key))
            payload = {"task": text, "agent": agent, "session_id": session_id, "priority": priority}
            self._insert_command(t, cmd_id, task_id, ws, node_id, "submit", payload, key, now, ttl)
            self.idem_store(t, ws, "submit", key, fp, task_id)
            self.emit(t, ws, task_id, "state_changed",
                      {"state": "pending_delivery", "source": "server", "command_id": cmd_id})
            self.audit(t, ws, req.principal.actor, "task.submit", task_id, "ok", req,
                       {"node_id": node_id, "command_id": cmd_id})
            task = t.q1(self.TASK_SELECT + " WHERE t.task_id = ?", (task_id,))
        return Response.json(self.task_out(task), 201)

    def _insert_command(self, t, cmd_id, task_id, ws, node_id, ctype, payload, key, now, ttl):
        t.x("INSERT INTO commands (command_id, task_id, workspace_id, node_id, type, payload_json, "
            "idempotency_key, state, created_at, expires_at, delivery_attempts) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', ?, ?, 0)",
            (cmd_id, task_id, ws, node_id, ctype, dumps(payload), key, now, now + ttl * 1000))
        if self.commands_notifier.notify not in t.after_commit:
            t.after_commit.append(self.commands_notifier.notify)

    def h_tasks_list(self, req):
        ws = req.params["ws"]
        q = req.query
        limit = V.query_int(q, "limit", 1, 200, 50)
        state = q.get("state") or None
        if state is not None and state not in self.TASK_STATES_ALL:
            raise V.bad("state", "unknown task state")
        node_id = q.get("node_id") or None
        cursor = q.get("cursor") or None
        if cursor is not None and not _ID_RE.match(cursor):
            raise V.bad("cursor", "malformed")
        sql = self.TASK_SELECT + " WHERE t.workspace_id = ?"
        params = [ws]
        if state:
            sql += " AND t.state = ?"
            params.append(state)
        if node_id:
            sql += " AND t.node_id = ?"
            params.append(node_id)
        if cursor:
            sql += " AND t.task_id < ?"
            params.append(cursor)
        sql += " ORDER BY t.task_id DESC LIMIT ?"
        params.append(limit + 1)
        with self.storage.read() as t:
            self.require_role(req, t, ws, "viewer")
            rows = t.q(sql, params)
        nxt = rows[limit - 1]["task_id"] if len(rows) > limit else None
        return Response.json({"tasks": [self.task_out(r) for r in rows[:limit]], "next_cursor": nxt})

    def _task_for_user(self, req, t, min_role="viewer", action=""):
        row = t.q1(self.TASK_SELECT + " WHERE t.task_id = ?", (req.params["task"],))
        if row is None:
            raise ApiError("not_found", "not found")
        self.require_role(req, t, row["workspace_id"], min_role, action)
        return row

    def h_task_get(self, req):
        with self.storage.read() as t:
            row = self._task_for_user(req, t)
        return Response.json(self.task_out(row))

    def h_task_commands(self, req):
        with self.storage.read() as t:
            row = self._task_for_user(req, t)
            cmds = t.q("SELECT * FROM commands WHERE task_id = ? ORDER BY command_id", (row["task_id"],))
        return Response.json({"commands": [self.command_out(c) for c in cmds]})

    # -------------------------------------------------------------- events
    def _long_poll_events(self, req, where: str, params: list):
        q = req.query
        after = V.query_int(q, "after_event_id", 0, 2 ** 62, 0)
        limit = V.query_int(q, "limit", 1, 500, 100)
        wait_ms = V.query_int(q, "wait_ms", 0, 30000, 0)
        deadline = time.monotonic() + wait_ms / 1000.0
        sql = ("SELECT * FROM events WHERE " + where + " AND event_id > ? ORDER BY event_id LIMIT ?")
        while True:
            version = self.events_notifier.version
            with self.storage.read() as t:
                rows = t.q(sql, params + [after, limit])
            remaining = deadline - time.monotonic()
            if rows or remaining <= 0 or self.stopping:
                break
            self.events_notifier.wait(version, min(remaining, 5.0))
        last = int(rows[-1]["event_id"]) if rows else after
        return Response.json({"events": [self.event_out(e) for e in rows], "last_event_id": last})

    def h_task_events(self, req):
        with self.storage.read() as t:
            row = self._task_for_user(req, t)
        return self._long_poll_events(req, "workspace_id = ? AND task_id = ?",
                                      [row["workspace_id"], row["task_id"]])

    def h_ws_events(self, req):
        ws = req.params["ws"]
        with self.storage.read() as t:
            self.require_role(req, t, ws, "viewer")
        return self._long_poll_events(req, "workspace_id = ?", [ws])

    # ------------------------------------------------ answer / cancel / retry
    def _task_command(self, req, kind: str):
        with self.storage.read() as t:
            row = self._task_for_user(req, t, "operator", "task." + kind)
        ws = row["workspace_id"]
        body = req.json(self.config.max_json_body_bytes)
        key = V.idem_key(body)
        ttl = V.req_int(body, "ttl_seconds", 10, 86400, required=False,
                        default=self.config.default_command_ttl_seconds)
        payload = {}
        if kind == "answer":
            payload = {"answer": V.req_str(body, "answer", 64 * 1024, 0)}
        fp = sha256_hex(canonical_json({"task_id": row["task_id"], "kind": kind, "payload": payload,
                                        "ttl_seconds": ttl}))
        now = self.clock.now_ms()
        with self.storage.tx() as t:
            self.lock_ws(t, ws)
            existing = self.idem_lookup(t, ws, kind, key, fp)
            if existing:
                c = t.q1("SELECT * FROM commands WHERE command_id = ?", (existing,))
                return Response.json(self.command_out(c), 200)
            task = t.q1("SELECT * FROM tasks WHERE task_id = ?" + t.for_update, (row["task_id"],))
            node = t.q1("SELECT revoked_at FROM nodes WHERE node_id = ?", (task["node_id"],))
            state = task["state"]
            if kind == "answer" and state != "waiting_input":
                raise ApiError("conflict", "task is not waiting for input", {"state": state})
            if kind == "cancel" and state in self.TERMINAL:
                raise ApiError("conflict", "task already finished", {"state": state})
            if kind == "retry" and state not in ("failed", "delivery_failed"):
                raise ApiError("conflict", "only failed or delivery_failed tasks can be retried",
                               {"state": state})
            if node["revoked_at"] is not None:
                raise ApiError("conflict", "node is revoked", {"node_id": task["node_id"]})
            ctype = kind
            if kind == "retry" and state == "delivery_failed":
                # never reached the node: re-send the original submit (fresh TTL)
                orig = t.q1("SELECT payload_json FROM commands WHERE task_id = ? AND type = 'submit' "
                            "ORDER BY command_id LIMIT 1", (task["task_id"],))
                payload = json.loads(orig["payload_json"]) if orig else {}
                ctype = "submit"
            cmd_id = new_id("cmd_")
            self._insert_command(t, cmd_id, task["task_id"], ws, task["node_id"], ctype, payload, key,
                                 now, ttl)
            self.idem_store(t, ws, kind, key, fp, cmd_id)
            if ctype == "submit":
                t.x("UPDATE tasks SET state = 'pending_delivery', error = NULL, updated_at = ? "
                    "WHERE task_id = ?", (now, task["task_id"]))
                self.emit(t, ws, task["task_id"], "state_changed",
                          {"state": "pending_delivery", "source": "server", "command_id": cmd_id,
                           "retry": True})
            if kind == "answer":
                self.emit(t, ws, task["task_id"], "answer_submitted",
                          {"command_id": cmd_id, "by": req.principal.user_id})
            self.emit(t, ws, task["task_id"], "command_status",
                      {"command_id": cmd_id, "type": ctype, "state": "pending"})
            self.audit(t, ws, req.principal.actor, "task." + kind, task["task_id"], "ok", req,
                       {"command_id": cmd_id})
            c = t.q1("SELECT * FROM commands WHERE command_id = ?", (cmd_id,))
        return Response.json(self.command_out(c), 202)

    def h_task_answer(self, req):
        return self._task_command(req, "answer")

    def h_task_cancel(self, req):
        return self._task_command(req, "cancel")

    def h_task_retry(self, req):
        return self._task_command(req, "retry")

    # ----------------------------------------------------- members & audit
    def _members(self, t, ws):
        rows = t.q("SELECT m.user_id, u.name, m.role, m.created_at FROM memberships m JOIN users u "
                   "ON u.user_id = m.user_id WHERE m.workspace_id = ? ORDER BY m.user_id", (ws,))
        return [{"user_id": r["user_id"], "name": r["name"], "role": r["role"],
                 "created_at": rfc3339(r["created_at"])} for r in rows]

    def h_members_get(self, req):
        ws = req.params["ws"]
        with self.storage.read() as t:
            self.require_role(req, t, ws, "owner")
            members = self._members(t, ws)
        return Response.json({"workspace_id": ws, "members": members})

    def h_members_put(self, req):
        ws = req.params["ws"]
        with self.storage.read() as t:
            self.require_role(req, t, ws, "owner", "member.role_change")
        body = req.json(self.config.max_json_body_bytes)
        changes = body.get("members")
        if changes is None:
            changes = [{"user_id": body.get("user_id"), "role": body.get("role")}]
        if not isinstance(changes, list) or not changes or len(changes) > 100:
            raise V.bad("members", "must be a non-empty list")
        for ch in changes:
            if not isinstance(ch, dict) or not isinstance(ch.get("user_id"), str):
                raise V.bad("user_id", "required")
            if ch.get("role") not in ("owner", "operator", "viewer", None, "none"):
                raise V.bad("role", "must be owner|operator|viewer|null")
        now = self.clock.now_ms()
        with self.storage.tx() as t:
            self.lock_ws(t, ws)
            for ch in changes:
                uid, role = ch["user_id"], ch.get("role")
                role = None if role == "none" else role
                if t.q1("SELECT user_id FROM users WHERE user_id = ?", (uid,)) is None:
                    raise ApiError("not_found", "user not found", {"user_id": uid})
                old = self.role_in(t, uid, ws)
                if old == role:
                    continue
                if role is None:
                    t.x("DELETE FROM memberships WHERE workspace_id = ? AND user_id = ?", (ws, uid))
                elif old is None:
                    t.x("INSERT INTO memberships (workspace_id, user_id, role, created_at) VALUES "
                        "(?, ?, ?, ?)", (ws, uid, role, now))
                else:
                    t.x("UPDATE memberships SET role = ? WHERE workspace_id = ? AND user_id = ?",
                        (role, ws, uid))
                self.audit(t, ws, req.principal.actor, "member.role_change", uid, "ok", req,
                           {"old_role": old, "new_role": role})
            owners = t.q1("SELECT COUNT(*) AS c FROM memberships WHERE workspace_id = ? AND role = 'owner'",
                          (ws,))
            if owners["c"] < 1:
                raise ApiError("conflict", "a workspace must keep at least one owner")
            members = self._members(t, ws)
        return Response.json({"workspace_id": ws, "members": members})

    def h_audit(self, req):
        ws = req.params["ws"]
        limit = V.query_int(req.query, "limit", 1, 200, 50)
        cursor = req.query.get("cursor") or None
        if cursor is not None and not _ID_RE.match(cursor):
            raise V.bad("cursor", "malformed")
        sql = "SELECT * FROM audit WHERE workspace_id = ?"
        params = [ws]
        if cursor:
            sql += " AND audit_id < ?"
            params.append(cursor)
        sql += " ORDER BY audit_id DESC LIMIT ?"
        params.append(limit + 1)
        with self.storage.read() as t:
            self.require_role(req, t, ws, "owner")
            rows = t.q(sql, params)
        nxt = rows[limit - 1]["audit_id"] if len(rows) > limit else None
        return Response.json({"audit": [self.audit_out(a) for a in rows[:limit]], "next_cursor": nxt})
