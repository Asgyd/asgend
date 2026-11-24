"""Execution Node channel (Bearer ncp_..., except enroll)."""

from __future__ import annotations

import json
import time

from . import validate as V
from .http import ApiError, Response
from .util import dumps, new_id, new_token, normalize_code, parse_rfc3339, rfc3339, sha256_hex

NODE_EVENT_TYPES = ("state_changed", "question", "core_event", "artifact_available", "command_status")


class NodeApi:
    def _touch_presence(self, t, node_id: str, now: int) -> int:
        lease = now + self.config.node_lease_seconds * 1000
        t.x("UPDATE nodes SET last_seen_at = ?, lease_expires_at = ? WHERE node_id = ? "
            "AND revoked_at IS NULL", (now, lease, node_id))
        return lease

    # -------------------------------------------------------------- enroll
    def h_node_enroll(self, req):
        self.limit_auth_endpoint(req)
        body = req.json(self.config.max_json_body_bytes)
        code = V.req_str(body, "enrollment_code", 128, 8)
        pv = body.get("protocol_version", 1)
        if pv != 1:
            raise ApiError("invalid_request", "unsupported protocol_version",
                           {"field": "protocol_version", "supported": [1]})
        name = V.req_str(body, "name", 100, 1, required=False)
        labels = V.labels(body, default=None) if body.get("labels") is not None else None
        core_version = V.req_str(body, "core_version", 64, required=False)
        now = self.clock.now_ms()
        h = sha256_hex(normalize_code(code))
        token = new_token("ncp_")
        with self.storage.tx() as t:
            row = t.q1("SELECT * FROM enrollment_codes WHERE code_hash = ?" + t.for_update, (h,))
            if row is None or row["used_at"] is not None or row["expires_at"] <= now:
                reason = "unknown" if row is None else ("used" if row["used_at"] else "expired")
                self.audit(t, row["workspace_id"] if row else None, "anonymous", "node.enroll",
                           row["enrollment_id"] if row else None, "denied", req, {"reason": reason})
                deny = True
            else:
                deny = False
                ws = row["workspace_id"]
                node_id = new_id("node_")
                final_name = name or row["name"] or "node"
                final_labels = labels if labels is not None else json.loads(row["labels_json"] or "[]")
                t.x("UPDATE enrollment_codes SET used_at = ?, node_id = ? WHERE enrollment_id = ? "
                    "AND used_at IS NULL", (now, node_id, row["enrollment_id"]))
                t.x("INSERT INTO nodes (node_id, workspace_id, name, labels_json, protocol_version, "
                    "core_version, last_seen_at, lease_expires_at, created_at) "
                    "VALUES (?, ?, ?, ?, 1, ?, ?, ?, ?)",
                    (node_id, ws, final_name, dumps(final_labels), core_version, now,
                     now + self.config.node_lease_seconds * 1000, now))
                tid = new_id("tok_")
                t.x("INSERT INTO tokens (token_hash, token_id, kind, node_id, name, created_at) "
                    "VALUES (?, ?, 'node', ?, ?, ?)", (sha256_hex(token), tid, node_id, final_name, now))
                self.audit(t, ws, node_id, "node.enroll", node_id, "ok", req,
                           {"enrollment_id": row["enrollment_id"], "token_id": tid})
        if deny:
            raise self._auth_fail(req, "enrollment_code")
        return Response.json({
            "node_id": node_id, "workspace_id": ws, "node_token": token,
            "heartbeat_interval_seconds": self.config.heartbeat_interval_seconds,
            "lease_seconds": self.config.node_lease_seconds,
        }, 201)

    # ----------------------------------------------------------- heartbeat
    def h_node_heartbeat(self, req):
        p = req.principal
        body = req.json(self.config.max_json_body_bytes)
        status = body.get("status", "ready")
        if status not in ("ready", "busy", "degraded"):
            raise V.bad("status", "must be ready|busy|degraded")
        core_version = V.req_str(body, "core_version", 64, required=False)
        active = V.req_int(body, "active_tasks", 0, 1_000_000, required=False, default=0)
        now = self.clock.now_ms()
        with self.storage.tx() as t:
            lease = self._touch_presence(t, p.node_id, now)
            t.x("UPDATE nodes SET node_state = ?, active_tasks = ?, core_version = COALESCE(?, core_version) "
                "WHERE node_id = ?", (status, active, core_version, p.node_id))
        return Response.json({"server_time": rfc3339(now), "lease_expires_at": rfc3339(lease),
                              "heartbeat_interval_seconds": self.config.heartbeat_interval_seconds})

    def h_node_rotate(self, req):
        p = req.principal
        now = self.clock.now_ms()
        token = new_token("ncp_")
        grace_until = now + self.config.token_rotation_grace_seconds * 1000
        with self.storage.tx() as t:
            tid = new_id("tok_")
            t.x("INSERT INTO tokens (token_hash, token_id, kind, node_id, name, created_at) "
                "VALUES (?, ?, 'node', ?, ?, ?)", (sha256_hex(token), tid, p.node_id, p.name, now))
            t.x("UPDATE tokens SET expires_at = ? WHERE token_hash = ? AND (expires_at IS NULL OR "
                "expires_at > ?)", (grace_until, p.token_hash, grace_until))
            self.audit(t, p.workspace_id, p.actor, "node.token_rotate", p.node_id, "ok", req,
                       {"old_token_id": p.token_id, "new_token_id": tid})
        return Response.json({"node_token": token, "previous_token_expires_at": rfc3339(grace_until)})

    # ------------------------------------------------------------ commands
    def h_commands_lease(self, req):
        p = req.principal
        body = req.json(self.config.max_json_body_bytes)
        mx = V.req_int(body, "max", 1, 50, required=False, default=10)
        wait_ms = V.req_int(body, "wait_ms", 0, 30000, required=False, default=0)
        deadline = time.monotonic() + wait_ms / 1000.0
        with self.storage.tx() as t:
            self._touch_presence(t, p.node_id, self.clock.now_ms())
        while True:
            version = self.commands_notifier.version
            self.expire_due_commands(p.node_id)
            envs = self._lease_batch(p, mx)
            remaining = deadline - time.monotonic()
            if envs or remaining <= 0 or self.stopping:
                break
            self.commands_notifier.wait(version, min(remaining, 5.0))
        return Response.json({"commands": envs})

    def _lease_batch(self, p, mx: int) -> list:
        now = self.clock.now_ms()
        lease_until = now + self.config.command_lease_seconds * 1000
        out = []
        with self.storage.tx() as t:
            self.lock_ws(t, p.workspace_id)
            rows = t.q("SELECT * FROM commands WHERE node_id = ? AND expires_at > ? AND "
                       "(state = 'pending' OR (state = 'leased' AND lease_expires_at <= ?)) "
                       "ORDER BY command_id LIMIT ?" + t.skip_locked, (p.node_id, now, now, mx))
            for c in rows:
                lease_id = new_id("lse_")
                t.x("UPDATE commands SET state = 'leased', lease_id = ?, lease_expires_at = ?, "
                    "delivery_attempts = delivery_attempts + 1, first_leased_at = COALESCE(first_leased_at, ?) "
                    "WHERE command_id = ?", (lease_id, lease_until, now, c["command_id"]))
                c = t.q1("SELECT * FROM commands WHERE command_id = ?", (c["command_id"],))
                out.append(self.command_out(c))
        return out

    def h_command_ack(self, req):
        p = req.principal
        body = req.json(self.config.max_json_body_bytes)
        lease_id = V.req_str(body, "lease_id", 80)
        outcome = body.get("outcome")
        if outcome not in ("accepted", "rejected"):
            raise V.bad("outcome", "must be accepted|rejected")
        core_task_id = V.req_str(body, "core_task_id", 128, required=False)
        err = V.req_str(body, "error", 4000, 0, required=False)
        now = self.clock.now_ms()
        target_state = "acked" if outcome == "accepted" else "rejected"
        with self.storage.tx() as t:
            self.lock_ws(t, p.workspace_id)
            c = t.q1("SELECT * FROM commands WHERE command_id = ?" + t.for_update, (req.params["cmd"],))
            if c is None or c["node_id"] != p.node_id:
                raise ApiError("not_found", "not found")
            if c["state"] in ("acked", "rejected"):
                if c["state"] != target_state:
                    raise ApiError("conflict", "command already acknowledged with another outcome",
                                   {"state": c["state"]})
                return Response.json(self.command_out(c), 200)
            if c["state"] == "expired":
                raise ApiError("gone", "command expired", {"error": c["error"]})
            if c["state"] != "leased" or c["lease_id"] != lease_id:
                raise ApiError("conflict", "lease_id does not match the current lease",
                               {"state": c["state"]})
            t.x("UPDATE commands SET state = ?, outcome = ?, acked_at = ?, error = ?, lease_expires_at = NULL "
                "WHERE command_id = ?", (target_state, outcome, now, err if outcome == "rejected" else None,
                                          c["command_id"]))
            self.metrics.observe("asgend_cp_command_delivery_latency_seconds",
                                 (now - c["created_at"]) / 1000.0,
                                 help="Time from command creation to node acknowledgement")
            self.emit(t, c["workspace_id"], c["task_id"], "command_status",
                      {"command_id": c["command_id"], "type": c["type"], "state": target_state,
                       "error": err if outcome == "rejected" else None}, node_id=p.node_id)
            if c["type"] == "submit":
                task = t.q1("SELECT * FROM tasks WHERE task_id = ?" + t.for_update, (c["task_id"],))
                if outcome == "accepted":
                    if task["state"] in ("pending_delivery", "delivery_failed"):
                        t.x("UPDATE tasks SET state = 'delivered', core_task_id = COALESCE(?, core_task_id), "
                            "attempts = attempts + 1, error = NULL, updated_at = ? WHERE task_id = ?",
                            (core_task_id, now, task["task_id"]))
                        self.emit(t, c["workspace_id"], c["task_id"], "state_changed",
                                  {"state": "delivered", "source": "server", "command_id": c["command_id"],
                                   "core_task_id": core_task_id}, node_id=p.node_id)
                    elif core_task_id:
                        t.x("UPDATE tasks SET core_task_id = COALESCE(core_task_id, ?) WHERE task_id = ?",
                            (core_task_id, task["task_id"]))
                elif task["state"] in ("pending_delivery", "delivery_failed"):
                    msg = err or "submit rejected by node"
                    t.x("UPDATE tasks SET state = 'failed', error = ?, updated_at = ? WHERE task_id = ?",
                        (msg, now, task["task_id"]))
                    self.emit(t, c["workspace_id"], c["task_id"], "state_changed",
                              {"state": "failed", "source": "node_ack", "command_id": c["command_id"],
                               "error": msg}, node_id=p.node_id)
            c = t.q1("SELECT * FROM commands WHERE command_id = ?", (c["command_id"],))
        return Response.json(self.command_out(c), 200)

    # -------------------------------------------------------------- events
    def _node_task(self, t, p, task_id: str):
        task = t.q1("SELECT * FROM tasks WHERE task_id = ?" + t.for_update, (task_id,))
        if task is None or task["node_id"] != p.node_id:
            raise ApiError("not_found", "task not found", {"task_id": task_id})
        return task

    def _apply_projection(self, t, task: dict, etype: str, payload: dict, now: int) -> None:
        sets, params = [], []
        if etype == "state_changed":
            st = payload.get("state")
            if st in self.NODE_REPORTED_STATES:
                sets.append("state = ?")
                params.append(st)
                if st != "waiting_input":
                    sets.append("question_json = NULL")
            for f, mx in (("result_summary", 4000), ("error", 4000), ("core_task_id", 128)):
                v = payload.get(f)
                if isinstance(v, str):
                    sets.append(f"{f} = ?")
                    params.append(v[:mx])
            a = payload.get("attempts")
            if isinstance(a, int) and not isinstance(a, bool) and 0 <= a < 1_000_000:
                sets.append("attempts = ?")
                params.append(a)
        elif etype == "question":
            q = _question(payload)
            if q is not None:
                sets += ["state = 'waiting_input'", "question_json = ?"]
                params.append(dumps(q))
        if sets:
            sets.append("updated_at = ?")
            params.append(now)
            t.x("UPDATE tasks SET " + ", ".join(sets) + " WHERE task_id = ?", params + [task["task_id"]])

    def h_node_events(self, req):
        p = req.principal
        body = req.json(self.config.max_json_body_bytes)
        evs = body.get("events")
        if not isinstance(evs, list):
            raise V.bad("events", "must be a list")
        if len(evs) > 500:
            raise V.bad("events", "at most 500 events per batch")
        now = self.clock.now_ms()
        parsed = []
        for i, e in enumerate(evs):
            if not isinstance(e, dict):
                raise V.bad(f"events[{i}]", "must be an object")
            tid = e.get("task_id")
            seq = e.get("seq")
            et = e.get("type")
            if not isinstance(tid, str) or not tid:
                raise V.bad(f"events[{i}].task_id", "required")
            if isinstance(seq, bool) or not isinstance(seq, int) or seq < 1:
                raise V.bad(f"events[{i}].seq", "must be an integer >= 1")
            if et not in NODE_EVENT_TYPES:
                raise V.bad(f"events[{i}].type", "unsupported event type")
            occ = e.get("occurred_at")
            if occ is None:
                occ_ms = now
            else:
                try:
                    occ_ms = parse_rfc3339(occ)
                except ValueError:
                    raise V.bad(f"events[{i}].occurred_at", "must be RFC3339") from None
            payload = e.get("payload", {})
            if not isinstance(payload, dict):
                raise V.bad(f"events[{i}].payload", "must be an object")
            parsed.append((tid, seq, et, occ_ms, payload))
        accepted = duplicates = 0
        gaps = []
        by_task: dict = {}
        for ev in parsed:
            by_task.setdefault(ev[0], []).append(ev)
        with self.storage.tx() as t:
            self.lock_ws(t, p.workspace_id)
            tasks = {tid: self._node_task(t, p, tid) for tid in by_task}
            for tid, items in by_task.items():
                task = tasks[tid]
                last = int(task["last_seq"])
                for (_, seq, et, occ_ms, payload) in sorted(items, key=lambda x: x[1]):
                    if seq <= last:
                        duplicates += 1
                        continue
                    if seq != last + 1:
                        gaps.append({"task_id": tid, "expected_seq": last + 1})
                        break
                    self.emit(t, task["workspace_id"], tid, et, payload, node_id=p.node_id, seq=seq,
                              occurred_at=occ_ms)
                    self._apply_projection(t, task, et, payload, now)
                    self.metrics.observe("asgend_cp_event_ingest_delay_seconds",
                                         max(0.0, (now - occ_ms) / 1000.0),
                                         help="Delay between node occurred_at and server receipt")
                    last = seq
                    accepted += 1
                t.x("UPDATE tasks SET last_seq = ? WHERE task_id = ?", (last, tid))
            self._touch_presence(t, p.node_id, now)
        self.metrics.inc("asgend_cp_events_ingested_total", value=accepted, help="Node events accepted")
        if duplicates:
            self.metrics.inc("asgend_cp_event_duplicates_total", value=duplicates,
                             help="Duplicate node events dropped")
        if gaps:
            self.metrics.inc("asgend_cp_event_gaps_total", value=len(gaps),
                             help="Per-task sequence gaps reported to nodes")
        return Response.json({"accepted": accepted, "duplicates": duplicates, "gaps": gaps})

    def h_node_snapshot(self, req):
        p = req.principal
        body = req.json(self.config.max_json_body_bytes)
        state = body.get("state")
        if state not in self.NODE_REPORTED_STATES:
            raise V.bad("state", "unsupported state")
        last_seq = V.req_int(body, "last_seq", 0, 2 ** 62)
        attempts = V.req_int(body, "attempts", 0, 1_000_000, required=False)
        core_task_id = V.req_str(body, "core_task_id", 128, required=False)
        result_summary = V.req_str(body, "result_summary", 64 * 1024, 0, required=False)
        err = V.req_str(body, "error", 64 * 1024, 0, required=False)
        question = _question(body.get("question")) if state == "waiting_input" else None
        now = self.clock.now_ms()
        with self.storage.tx() as t:
            self.lock_ws(t, p.workspace_id)
            task = self._node_task(t, p, req.params["task"])
            new_last = max(int(task["last_seq"]), last_seq)
            t.x("UPDATE tasks SET state = ?, core_task_id = COALESCE(?, core_task_id), "
                "attempts = COALESCE(?, attempts), question_json = ?, result_summary = ?, error = ?, "
                "last_seq = ?, updated_at = ? WHERE task_id = ?",
                (state, core_task_id, attempts, dumps(question) if question else None,
                 (result_summary or "")[:4000] or None, (err or "")[:4000] or None, new_last, now,
                 task["task_id"]))
            payload = {"state": state, "source": "snapshot", "last_seq": new_last}
            if question:
                payload["question"] = question
            self.emit(t, task["workspace_id"], task["task_id"], "state_changed", payload,
                      node_id=p.node_id)
            self._touch_presence(t, p.node_id, now)
            row = t.q1(self.TASK_SELECT + " WHERE t.task_id = ?", (task["task_id"],))
        return Response.json(self.task_out(row))


def _question(q):
    if not isinstance(q, dict):
        return None
    text = q.get("text")
    if not isinstance(text, str):
        return None
    opts = q.get("options") or []
    if not isinstance(opts, list):
        opts = []
    return {"text": text[:16000], "options": [str(o)[:500] for o in opts[:50]]}
