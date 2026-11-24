"""Faithful in-process mock of the ASGEND core JSON API (src/server.c op shapes).

One JSON object + LF per TCP connection, one JSON reply + LF, then close. Implements
ping, submit (with idempotency_key), status, events (after_id/limit), answer, cancel,
retry_task, artifact_list, workspace_read_bytes and workspace_list. Event ids are global
and increasing (like the SQLite autoincrement in the real core).

Optional ``script`` drives tasks automatically (used by the e2e test):
  "interactive": queued -> running -> waiting_input(question) ; answer -> running -> completed + report.txt
  "simple":      queued -> running -> completed + result.txt
  "fail_first":  queued -> running -> failed ; retry_task -> running -> completed
"""

from __future__ import annotations

import base64
import hashlib
import json
import mimetypes
import socketserver
import threading
import time
from datetime import datetime, timezone


def _now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


TERMINAL = ("completed", "completed_without_model", "failed", "cancelled")


class MockCore:
    def __init__(self, auth_token: str | None = None, script: str | None = None, tick: float = 0.05,
                 agents: tuple[str, ...] = ("generalist", "coder")):
        self.auth_token = auth_token
        self.script = script
        self.tick = tick
        self.agents = set(agents)
        self.lock = threading.RLock()
        self.tasks: dict[str, dict] = {}
        self.idempotency: dict[str, str] = {}
        self.next_task = 1
        self.next_event = 1
        self.op_counts: dict[str, int] = {}
        self.requests: list[dict] = []
        self.down = False
        self.fail_next: dict[str, dict] = {}
        self.oversize_reply = False
        self.hang = False
        self._server: socketserver.ThreadingTCPServer | None = None
        self._ticker: threading.Thread | None = None
        self._stop = threading.Event()

    # ------------------------------------------------------------------ server
    def start(self) -> "MockCore":
        core = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                if core.down:
                    return  # close without reply -> transport failure
                line = self.rfile.readline(1024 * 1024 + 1)
                if core.hang:
                    time.sleep(5)
                    return
                try:
                    request = json.loads(line.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    reply = {"ok": False, "error": "request must be a JSON object"}
                else:
                    reply = core.handle(request)
                data = json.dumps(reply, ensure_ascii=False).encode("utf-8") + b"\n"
                if core.oversize_reply:
                    data = b'{"ok":true,"pad":"' + b"x" * (9 * 1024 * 1024) + b'"}\n'
                try:
                    self.wfile.write(data)
                except OSError:
                    pass

        class Server(socketserver.ThreadingTCPServer):
            daemon_threads = True
            allow_reuse_address = True

        self._server = Server(("127.0.0.1", 0), Handler)
        threading.Thread(target=self._server.serve_forever, args=(0.05,), daemon=True, name="mock-core").start()
        if self.script:
            self._ticker = threading.Thread(target=self._tick_loop, daemon=True, name="mock-core-tick")
            self._ticker.start()
        return self

    @property
    def port(self) -> int:
        assert self._server is not None
        return self._server.server_address[1]

    def stop(self) -> None:
        self._stop.set()
        if self._server:
            self._server.shutdown()
            self._server.server_close()

    # ----------------------------------------------------------------- helpers
    def _event(self, task: dict, etype: str, message: str, payload=None, actor: str = "orchestrator") -> int:
        eid = self.next_event
        self.next_event += 1
        ev = {"event_id": eid, "type": etype, "actor": actor, "message": message, "created_at": _now()}
        if payload is not None:
            ev["payload"] = payload
        task["events"].append(ev)
        return eid

    def set_state(self, core_id: str, status: str, result=None, error=None, event: str | None = None,
                  message: str = "", payload=None) -> None:
        with self.lock:
            task = self.tasks[core_id]
            task["status"] = status
            task["updated_at"] = _now()
            if result is not None:
                task["result"] = result
            if error is not None:
                task["error"] = error
            if status == "running":
                task["attempts"] += 1
            if event:
                self._event(task, event, message or status, payload)

    def ask(self, core_id: str, text: str, options: list[tuple[str, str]]) -> None:
        q = {"question": text, "options": [{"id": i, "label": l} for i, l in options]}
        result = {"status": "waiting_input", "task_id": core_id, "questions": [q], "question": q}
        self.set_state(core_id, "waiting_input", result=result, event="question", message=text, payload=result)

    def add_event(self, core_id: str, etype: str, message: str, payload=None) -> int:
        with self.lock:
            return self._event(self.tasks[core_id], etype, message, payload)

    def add_file(self, core_id: str, path: str, data: bytes, symlink: bool = False) -> None:
        with self.lock:
            self.tasks[core_id]["files"][path] = {"data": data, "symlink": symlink}

    def count(self, op: str) -> int:
        return self.op_counts.get(op, 0)

    # ------------------------------------------------------------------- ops
    def handle(self, req: dict) -> dict:
        with self.lock:
            self.requests.append({k: v for k, v in req.items() if k != "auth_token"})
            if not isinstance(req, dict):
                return {"ok": False, "error": "request must be a JSON object"}
            if self.auth_token and req.get("auth_token") != self.auth_token:
                return {"ok": False, "error": "unauthorized"}
            op = req.get("op")
            if not isinstance(op, str):
                return {"ok": False, "error": "op is required"}
            self.op_counts[op] = self.op_counts.get(op, 0) + 1
            if op in self.fail_next:
                return self.fail_next.pop(op)
            if op in ("status", "events", "cancel", "answer", "retry_task", "workspace_list",
                      "workspace_read_bytes", "artifact_list"):
                tid = req.get("task_id")
                if not isinstance(tid, str) or not tid:
                    return {"ok": False, "error": "task_id is required"}
                if tid not in self.tasks:
                    return {"ok": False, "error": "task not found"}
            fn = getattr(self, "op_" + op, None)
            if fn is None:
                return {"ok": False, "error": "unsupported op"}
            return fn(req)

    def op_ping(self, req):
        return {"ok": True, "service": "agent-orchestrator-c", "version": "0.9.9-mock"}

    def op_submit(self, req):
        task = req.get("task", req.get("prompt"))
        if not isinstance(task, str):
            return {"ok": False, "error": "task is required and must be a string"}
        if not task:
            return {"ok": False, "error": "task must not be empty"}
        agent = req.get("agent")
        if agent is not None and agent not in self.agents:
            return {"ok": False, "error": "unknown agent id"}
        priority = req.get("priority", 5)
        if not isinstance(priority, int) or isinstance(priority, bool) or not 0 <= priority <= 9:
            return {"ok": False, "error": "priority must be an integer in 0..9"}
        idem = req.get("idempotency_key")
        if isinstance(idem, str) and len(idem) > 128:
            return {"ok": False, "error": "idempotency_key exceeds 128 bytes"}
        if isinstance(idem, str) and idem in self.idempotency:
            tid = self.idempotency[idem]
            return {"ok": True, "accepted": True, "task_id": tid, "status": "existing", "duplicate": True,
                    "priority": self.tasks[tid]["priority"]}
        tid = "task-%06d" % self.next_task
        self.next_task += 1
        t = {"task_id": tid, "status": "queued", "attempts": 0, "max_attempts": 3, "priority": priority,
             "cancel_requested": False, "created_at": _now(), "updated_at": _now(), "task": task,
             "title": task[:60], "session_id": req.get("session_id"), "agent": agent, "result": None,
             "error": None, "events": [], "files": {}, "workspace_path": f"/var/lib/asgend/workspaces/{tid}",
             "script_step": 0}
        self.tasks[tid] = t
        if isinstance(idem, str):
            self.idempotency[idem] = tid
        self._event(t, "task_workspace_created", "A private working directory was created for this task.",
                    {"workspace_path": t["workspace_path"]})
        self._event(t, "task_created", task, None, actor="user")
        return {"ok": True, "accepted": True, "task_id": tid, "status": "queued", "duplicate": False,
                "priority": priority}

    def op_status(self, req):
        t = self.tasks[req["task_id"]]
        out = {k: t[k] for k in ("task_id", "status", "attempts", "priority", "max_attempts", "cancel_requested",
                                 "workspace_path", "created_at", "updated_at", "title", "task")}
        if t["session_id"]:
            out["session_id"] = t["session_id"]
        if t["error"]:
            out["error"] = t["error"]
        if t["result"] is not None:
            out["result"] = t["result"]
        out["ok"] = True
        return out

    def op_events(self, req):
        t = self.tasks[req["task_id"]]
        after = req.get("after_id", 0)
        after = after if isinstance(after, int) and after > 0 else 0
        limit = req.get("limit", 100)
        limit = limit if isinstance(limit, int) else 100
        limit = 100 if limit < 1 else min(limit, 500)
        events = [e for e in t["events"] if e["event_id"] > after][:limit]
        return {"ok": True, "events": json.loads(json.dumps(events))}

    def op_answer(self, req):
        t = self.tasks[req["task_id"]]
        answer = req.get("answer")
        if not isinstance(answer, str) or not answer:
            return {"ok": False, "error": "answer is required"}
        if t["status"] != "waiting_input":
            return {"ok": False, "error": "task is not waiting for input"}
        t["answer"] = answer
        t["status"] = "queued"
        t["updated_at"] = _now()
        self._event(t, "answer_received", "The user answered the clarification question.", None, actor="user")
        return {"ok": True, "task_id": t["task_id"], "status": "queued"}

    def op_cancel(self, req):
        t = self.tasks[req["task_id"]]
        if t["status"] in TERMINAL:
            return {"ok": False, "error": "task does not exist or is already terminal"}
        t["cancel_requested"] = True
        if t["status"] in ("queued", "waiting_input"):
            t["status"] = "cancelled"
        self._event(t, "task_cancel_requested", "Cancellation requested.", None, actor="user")
        return {"ok": True, "task_id": t["task_id"], "status": "cancellation_requested"}

    def op_retry_task(self, req):
        t = self.tasks[req["task_id"]]
        if t["status"] != "failed":
            return {"ok": False, "error": "task is not a failed dead letter"}
        t["status"] = "queued"
        t["max_attempts"] += 1
        t["error"] = None
        t["updated_at"] = _now()
        self._event(t, "task_retry_requested", "Manual retry requested.", None, actor="user")
        return {"ok": True, "task_id": t["task_id"], "status": "queued", "additional_attempts": 1}

    def op_workspace_list(self, req):
        t = self.tasks[req["task_id"]]
        return {"ok": True, "entries": [{"path": p, "type": "symlink" if f["symlink"] else "file",
                                         "size": len(f["data"])} for p, f in sorted(t["files"].items())]}

    def op_artifact_list(self, req):
        t = self.tasks[req["task_id"]]
        out = []
        for path, f in sorted(t["files"].items()):
            if f["symlink"] or len(f["data"]) > 256 * 1024 * 1024:
                continue  # real core: regular non-symlink files <= 256 MiB only
            out.append({"path": path, "size": len(f["data"]), "sha256": f.get("sha_override") or
                        hashlib.sha256(f["data"]).hexdigest(),
                        "mime": mimetypes.guess_type(path)[0] or "application/octet-stream"})
        return {"ok": True, "artifacts": out}

    def op_workspace_read_bytes(self, req):
        t = self.tasks[req["task_id"]]
        path = req.get("path")
        if not isinstance(path, str) or not path or path.startswith("/") or ".." in path.split("/"):
            return {"ok": False, "error": "path must be a safe relative file path"}
        f = t["files"].get(path)
        if f is None or f["symlink"]:
            return {"ok": False, "error": "file not found"}
        offset, limit = req.get("offset", 0), req.get("limit", 65536)
        if not isinstance(offset, int) or offset < 0:
            return {"ok": False, "error": "offset must be a non-negative integer"}
        if not isinstance(limit, int) or not 1 <= limit <= 1048576:
            return {"ok": False, "error": "limit must be between 1 and 1048576 bytes"}
        data = f["data"]
        chunk = data[offset:offset + limit]
        nxt = offset + len(chunk)
        return {"ok": True, "data_b64": base64.b64encode(chunk).decode("ascii"), "offset": offset,
                "next_offset": nxt, "size": len(data), "eof": nxt >= len(data)}

    # --------------------------------------------------------------- scripting
    def _tick_loop(self) -> None:
        while not self._stop.wait(self.tick):
            with self.lock:
                for t in list(self.tasks.values()):
                    self._advance(t)

    def _advance(self, t: dict) -> None:
        s, tid = t["status"], t["task_id"]
        if s in TERMINAL or s == "waiting_input":
            return
        if t["cancel_requested"]:
            t["status"] = "cancelled"
            self._event(t, "task_cancelled", "Task cancelled.")
            return
        if s == "queued":
            t["status"] = "running"
            t["attempts"] += 1
            self._event(t, "task_started", "Worker started the task.", {"attempt": t["attempts"],
                                                                       "auth_token": "secret-should-not-leak"})
            return
        # running
        if self.script == "interactive" and not t.get("answer"):
            q = {"question": "Which output format?", "options": [{"id": "md", "label": "Markdown"},
                                                                 {"id": "txt", "label": "Plain text"}]}
            t["result"] = {"status": "waiting_input", "task_id": tid, "questions": [q], "question": q}
            t["status"] = "waiting_input"
            self._event(t, "question", q["question"], t["result"])
            return
        if self.script == "fail_first" and not t.get("failed_once"):
            t["failed_once"] = True
            t["status"] = "failed"
            t["error"] = "model endpoint unavailable"
            self._event(t, "task_failed", "Task failed.", {"error": t["error"]})
            return
        body = (f"Report for: {t['task']}\nanswer={t.get('answer')}\n" + "x" * 5000).encode("utf-8")
        t["files"]["report.txt"] = {"data": body, "symlink": False}
        t["files"]["link-to-secret"] = {"data": b"secret", "symlink": True}
        t["status"] = "completed"
        t["result"] = {"summary": f"Done: {t['task'][:40]}", "agent_results": [{"output": "ok"}]}
        self._event(t, "task_completed", "Task completed.", {"files": ["report.txt"]})
