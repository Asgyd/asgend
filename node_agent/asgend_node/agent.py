"""Execution Node main loops: heartbeat, command lease, event pump/flush, artifacts.

The agent only talks to the local core API and the Control Plane. It never spawns
processes or shells (enforced by tests/test_no_exec.py).
"""

from __future__ import annotations

import base64
import hashlib
import random
import re
import threading
import time
from datetime import datetime, timezone
from typing import Any, Callable

from . import PROTOCOL_VERSION
from .config import NodeConfig
from .core_client import CoreClient, CoreError, CoreUnavailable
from .cp_client import CPClient, CPError, CPUnavailable, RateLimited, Unauthorized
from .logutil import get_logger
from .state import TERMINAL_STATES, Credentials, Store, utc_now
from . import translate

log = get_logger()

COMMAND_ID_RE = re.compile(r"^[A-Za-z0-9._:-]{1,128}$")
MAX_CHUNK = 8 * 1024 * 1024
EXIT_OK = 0
EXIT_REVOKED = 3


class NodeRevoked(Exception):
    pass


class Backoff:
    """Exponential backoff with "equal jitter": delay in [cap/2, cap]."""

    def __init__(self, initial: float = 1.0, maximum: float = 60.0, rng: Callable[[], float] = random.random):
        self.initial = max(0.001, initial)
        self.maximum = max(self.initial, maximum)
        self.rng = rng
        self.attempt = 0

    def next(self) -> float:
        cap = min(self.maximum, self.initial * (2 ** self.attempt))
        self.attempt += 1
        return cap / 2 + self.rng() * cap / 2

    def reset(self) -> None:
        self.attempt = 0


def to_rfc3339(value: Any) -> str:
    if isinstance(value, str) and value:
        text = value.strip().replace(" ", "T", 1)
        try:
            dt = datetime.fromisoformat(text.replace("Z", "+00:00"))
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=timezone.utc)
            return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
        except ValueError:
            pass
    return utc_now()


def _parse_time(value: Any) -> float | None:
    if not isinstance(value, str) or not value:
        return None
    try:
        dt = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.timestamp()


class NodeAgent:
    def __init__(self, config: NodeConfig, credentials: dict, store: Store, core: CoreClient,
                 cp: CPClient, credentials_store: Credentials | None = None,
                 clock: Callable[[], float] = time.time):
        self.config = config
        self.credentials = credentials
        self.node_id = credentials.get("node_id")
        self.store = store
        self.core = core
        self.cp = cp
        self.credentials_store = credentials_store
        self.clock = clock
        self.stop_event = threading.Event()
        self.revoked = False
        self.core_version: str | None = None
        self._cp_down = False
        self._cp_lock = threading.Lock()
        self._threads: list[threading.Thread] = []
        hb = config.heartbeat_interval_seconds or credentials.get("heartbeat_interval_seconds") or 15
        self.heartbeat_interval = max(0.05, float(hb))
        # A fresh process counts as a reconnect: resync every unfinished task once.
        self.store.mark_snapshot_needed()

    # ------------------------------------------------------------------ CP calls
    def _cp(self, fn: Callable, *args, **kwargs):
        if self.revoked:
            raise NodeRevoked()
        try:
            result = fn(*args, **kwargs)
        except Unauthorized:
            self._on_revoked()
            raise NodeRevoked()
        except CPUnavailable:
            with self._cp_lock:
                self._cp_down = True
            raise
        except CPError:
            self._note_cp_up()
            raise
        self._note_cp_up()
        return result

    def _note_cp_up(self) -> None:
        with self._cp_lock:
            was_down, self._cp_down = self._cp_down, False
        if was_down:
            log.info("control plane reachable again; scheduling task snapshots")
            self.store.mark_snapshot_needed()

    def _on_revoked(self) -> None:
        if self.revoked:
            return
        self.revoked = True
        log.error("control plane rejected the node token (401): node revoked or token invalid; stopping. "
                  "Re-enroll the node to continue.")
        if self.credentials_store is not None:
            try:
                self.credentials_store.update(revoked=True, revoked_at=utc_now())
            except OSError as exc:  # pragma: no cover - disk failure
                log.error("could not persist revoked flag: %s", exc)
        self.stop_event.set()

    # ---------------------------------------------------------------- heartbeat
    def active_task_count(self) -> int:
        return sum(1 for t in self.store.active_tasks() if not t["terminal"])

    def heartbeat_once(self) -> dict:
        status = "ready"
        try:
            ping = self.core.ping()
            self.core_version = str(ping.get("version") or "") or None
        except (CoreUnavailable, CoreError) as exc:
            log.warning("core ping failed: %s", exc)
            status = "degraded"
        active = self.active_task_count()
        if status == "ready" and active > 0:
            status = "busy"
        return self._cp(self.cp.heartbeat, status, active, self.core_version)

    # ----------------------------------------------------------------- commands
    def poll_commands_once(self, wait_ms: int | None = None) -> int:
        wait = self.config.lease_wait_ms if wait_ms is None else wait_ms
        commands = self._cp(self.cp.lease_commands, self.config.lease_max, wait)
        for cmd in commands:
            if self.stop_event.is_set() and self.revoked:
                break
            self.handle_command(cmd)
        return len(commands)

    def handle_command(self, cmd: dict) -> str | None:
        """Execute (or re-acknowledge) one leased command. Returns the outcome acked,
        or None when the command is left for redelivery (core unavailable)."""
        command_id = str(cmd.get("command_id") or "")
        lease_id = cmd.get("lease_id")
        ctype = str(cmd.get("type") or "")
        task_id = cmd.get("task_id")
        if not COMMAND_ID_RE.match(command_id):
            log.warning("ignoring command with invalid command_id")
            return None
        known = self.store.get_command(command_id)
        if known is not None:
            log.info("command %s redelivered; re-acking previous outcome %s", command_id, known["outcome"])
            self._ack(command_id, lease_id, known["outcome"], known["core_task_id"], known["error"],
                      ctype=known["type"], task_id=known["task_id"])
            return known["outcome"]

        outcome, core_task_id, error = "rejected", None, None
        expires = _parse_time(cmd.get("expires_at"))
        if expires is not None and expires < self.clock():
            error = "command expired before execution"
        elif cmd.get("node_id") and self.node_id and cmd.get("node_id") != self.node_id:
            error = "command addressed to another node"
        elif not isinstance(task_id, str) or not task_id:
            error = "command has no task_id"
        else:
            payload = cmd.get("payload") if isinstance(cmd.get("payload"), dict) else {}
            try:
                outcome, core_task_id, error = self._execute(command_id, ctype, task_id, payload)
            except CoreUnavailable as exc:
                log.warning("core unavailable while handling command %s (%s); leaving it for redelivery",
                            command_id, exc)
                return None
            except CoreError as exc:
                if exc.retryable:
                    log.warning("core asked to retry command %s later: %s", command_id, exc)
                    return None
                outcome, error = "rejected", str(exc)[:1000]
        self.store.record_command(command_id, ctype, task_id if isinstance(task_id, str) else None,
                                  outcome, core_task_id, error)
        log.info("command %s (%s) -> %s%s", command_id, ctype, outcome, f": {error}" if error else "")
        self._ack(command_id, lease_id, outcome, core_task_id, error, ctype=ctype, task_id=task_id)
        return outcome

    def _execute(self, command_id: str, ctype: str, task_id: str, payload: dict):
        if ctype == "submit":
            text = payload.get("task")
            if not isinstance(text, str) or not text.strip():
                return "rejected", None, "submit payload requires a non-empty task"
            priority = payload.get("priority")
            reply = self.core.submit(
                text, idempotency_key="cp:" + command_id,
                agent=payload.get("agent") if isinstance(payload.get("agent"), str) else None,
                session_id=payload.get("session_id") if isinstance(payload.get("session_id"), str) else None,
                priority=priority if isinstance(priority, int) and not isinstance(priority, bool) else None)
            core_task_id = str(reply.get("task_id") or "")
            if not core_task_id:
                return "rejected", None, "core did not return a task_id"
            return "accepted", core_task_id, None
        if ctype not in ("answer", "cancel", "retry"):
            return "rejected", None, f"unsupported command type {ctype!r}"
        task = self.store.get_task(task_id)
        if task is None:
            return "rejected", None, "unknown task on this node"
        core_task_id = task["core_task_id"]
        if ctype == "answer":
            answer = payload.get("answer")
            if not isinstance(answer, str) or not answer.strip():
                return "rejected", core_task_id, "answer payload requires a non-empty answer"
            option_id = payload.get("option_id") if isinstance(payload.get("option_id"), str) else None
            self.core.answer(core_task_id, answer, option_id)
            self.store.reopen_task(task_id)
        elif ctype == "cancel":
            self.core.cancel(core_task_id)
        else:  # retry
            self.core.retry_task(core_task_id)
            self.store.reopen_task(task_id)
        return "accepted", core_task_id, None

    def _ack(self, command_id: str, lease_id, outcome: str, core_task_id, error, ctype: str = "",
             task_id: str | None = None) -> bool:
        try:
            self._cp(self.cp.ack_command, command_id, lease_id, outcome, core_task_id, error)
        except CPError as exc:
            if exc.status in (404, 409, 410):
                log.warning("ack of command %s refused (%s); not retrying", command_id, exc.code)
                self.store.mark_command_acked(command_id)
                return False
            raise
        self.store.mark_command_acked(command_id)
        if ctype == "submit" and outcome == "accepted" and core_task_id and task_id:
            self.store.upsert_task(task_id, core_task_id)
        return True

    # --------------------------------------------------------------- event pump
    def pump_events_once(self) -> int:
        produced = 0
        for row in self.store.active_tasks():
            if self.stop_event.is_set():
                break
            try:
                produced += self._pump_task(row)
            except CoreError as exc:
                log.warning("core rejected polling of task %s: %s", row["task_id"], exc)
        return produced

    def _pump_task(self, row) -> int:
        task_id, core_id = row["task_id"], row["core_task_id"]
        produced = 0
        after = int(row["last_core_event_id"] or 0)
        page = self.config.core_events_page
        while True:
            events = self.core.events(core_id, after, page)
            batch: list[tuple[str, str, dict]] = []
            last = after
            for ev in events:
                try:
                    eid = int(ev.get("event_id") or 0)
                except (TypeError, ValueError):
                    continue
                if eid <= last:
                    continue
                last = eid
                batch.append(("core_event", to_rfc3339(ev.get("created_at")),
                              translate.core_event_payload(ev, self.config.forward_core_payloads)))
            if batch or last != after:
                self.store.append_events(task_id, batch, last_core_event_id=last)
                produced += len(batch)
            after = last
            if len(events) < page or not batch:
                break

        status = self.core.status(core_id)
        state = str(status.get("status") or "unknown")
        current = self.store.get_task(task_id)
        new_events: list[tuple[str, str, dict]] = []
        fields: dict[str, Any] = {}
        now = utc_now()
        if state != current["last_state"]:
            new_events.append(("state_changed", now,
                               translate.state_changed_payload(status, current["last_state"], core_id)))
            fields["last_state"] = state
        attempts = int(status.get("attempts") or 0)
        if attempts != current["attempts"]:
            fields["attempts"] = attempts
        if state == "waiting_input":
            question = translate.extract_question(status)
            key = translate.question_key(question)
            if question and key != current["question_key"]:
                new_events.append(("question", now, question))
                fields["question_key"] = key
        elif current["question_key"]:
            fields["question_key"] = None
        terminal = state in TERMINAL_STATES
        if terminal != bool(current["terminal"]):
            fields["terminal"] = 1 if terminal else 0
        if new_events or fields:
            self.store.append_events(task_id, new_events, **fields)
            produced += len(new_events)

        if terminal:
            current = self.store.get_task(task_id)
            if not current["artifacts_listed"]:
                self._list_artifacts(task_id, core_id)
            if self.store.unfinished_artifacts(task_id) == 0:
                self.store.update_task(task_id, done=1)
        return produced

    def _list_artifacts(self, task_id: str, core_id: str) -> None:
        if not self.config.artifacts_enabled:
            self.store.update_task(task_id, artifacts_listed=1)
            return
        try:
            items = self.core.artifact_list(core_id)
        except CoreError as exc:
            log.warning("core artifact_list failed for task %s: %s", task_id, exc)
            items = []
        for item in items:
            path = item.get("path")
            sha = str(item.get("sha256") or "").lower()
            try:
                size = int(item.get("size"))
            except (TypeError, ValueError):
                continue
            name = translate.artifact_name(path) if isinstance(path, str) else None
            if not name or not re.fullmatch(r"[0-9a-f]{64}", sha) or size < 0:
                log.warning("skipping invalid artifact entry for task %s", task_id)
                continue
            if item.get("symlink") or item.get("type") in ("symlink", "link"):
                continue
            mime = str(item.get("mime") or "application/octet-stream")[:200]
            if size > self.config.max_artifact_bytes:
                self.store.add_artifact(task_id, path, sha, name, mime, size, state="skipped",
                                        error="exceeds max_artifact_bytes")
                log.info("artifact %s of task %s exceeds the size limit; skipped", name, task_id)
                continue
            self.store.add_artifact(task_id, path, sha, name, mime, size)
        self.store.update_task(task_id, artifacts_listed=1)

    # --------------------------------------------------------------- event flush
    def flush_events_once(self) -> int:
        rows = self.store.pending_events(self.config.event_batch_size)
        if not rows:
            return 0
        events = [{"task_id": r["task_id"], "seq": r["seq"], "type": r["type"],
                   "occurred_at": r["occurred_at"], "payload": _loads(r["payload_json"])} for r in rows]
        try:
            reply = self._cp(self.cp.post_events, events)
        except CPError as exc:
            if exc.status in (400, 403, 404, 409, 413, 422):
                return self._flush_split(rows, exc)
            raise
        self._handle_events_reply(rows, reply)
        return len(rows)

    def _handle_events_reply(self, rows, reply: dict) -> None:
        self.store.delete_events([r["id"] for r in rows])
        own = {r["task_id"] for r in rows}
        for gap in reply.get("gaps") or []:
            if isinstance(gap, dict) and gap.get("task_id") in own:
                log.info("control plane reported a gap for task %s (expected seq %s); sending snapshot",
                         gap.get("task_id"), gap.get("expected_seq"))
                self.store.mark_snapshot_needed(gap["task_id"])

    def _flush_split(self, rows, exc: CPError) -> int:
        """A batch was refused as a whole: retry per task so one bad task cannot block
        the buffer forever. Events refused for a single task are dropped and the task
        is resynchronised by snapshot later."""
        by_task: dict[str, list] = {}
        for r in rows:
            by_task.setdefault(r["task_id"], []).append(r)
        if len(by_task) == 1 and exc.status != 413:
            task_id = rows[0]["task_id"]
            log.error("control plane refused events of task %s (%s); dropping %d buffered events",
                      task_id, exc.code, len(rows))
            self.store.delete_events([r["id"] for r in rows])
            self.store.mark_snapshot_needed(task_id)
            return len(rows)
        sent = 0
        for task_rows in by_task.values():
            chunks = [task_rows] if exc.status != 413 else [task_rows[i:i + 50] for i in range(0, len(task_rows), 50)]
            for chunk in chunks:
                events = [{"task_id": r["task_id"], "seq": r["seq"], "type": r["type"],
                           "occurred_at": r["occurred_at"], "payload": _loads(r["payload_json"])} for r in chunk]
                try:
                    reply = self._cp(self.cp.post_events, events)
                except CPError as inner:
                    if inner.status in (400, 403, 404, 409, 413, 422):
                        log.error("control plane refused %d events of task %s (%s); dropping them",
                                  len(chunk), chunk[0]["task_id"], inner.code)
                        self.store.delete_events([r["id"] for r in chunk])
                        self.store.mark_snapshot_needed(chunk[0]["task_id"])
                        continue
                    raise
                self._handle_events_reply(chunk, reply)
                sent += len(chunk)
        return sent

    def send_snapshots_once(self) -> int:
        sent = 0
        for row in self.store.tasks_needing_snapshot():
            task_id, core_id = row["task_id"], row["core_task_id"]
            if self.store.outbox_size(task_id) > 0:
                continue  # flush buffered events first, then resync
            try:
                status = self.core.status(core_id)
            except CoreError as exc:
                log.warning("cannot snapshot task %s: %s", task_id, exc)
                self.store.update_task(task_id, needs_snapshot=0)
                continue
            body = translate.snapshot_body(status, core_id, int(row["next_seq"]) - 1)
            try:
                self._cp(self.cp.snapshot, task_id, body)
            except CPError as exc:
                if exc.status in (400, 403, 404, 409, 422):
                    log.warning("snapshot of task %s refused (%s)", task_id, exc.code)
                    self.store.update_task(task_id, needs_snapshot=0)
                    continue
                raise
            self.store.update_task(task_id, needs_snapshot=0, snapshot_json=None)
            sent += 1
        return sent

    def sync_events_once(self) -> None:
        self.pump_events_once()
        while self.flush_events_once():
            if self.stop_event.is_set():
                break
        self.send_snapshots_once()

    # ---------------------------------------------------------------- artifacts
    def upload_artifacts_once(self) -> int:
        done = 0
        for art in self.store.artifacts_to_upload(self.clock()):
            if self.stop_event.is_set():
                break
            task = self.store.get_task(art["task_id"])
            if task is None:
                continue
            try:
                if self._upload_artifact(art, task["core_task_id"]):
                    done += 1
            except CoreError as exc:
                log.warning("artifact %s of task %s cannot be read from core: %s", art["name"], art["task_id"], exc)
                self.store.update_artifact(art["task_id"], art["path"], art["sha256"], state="rejected",
                                           error=str(exc)[:500])
            except CPError as exc:
                self._artifact_cp_error(art, exc)
            self._maybe_finish_task(art["task_id"])
        return done

    def _artifact_cp_error(self, art, exc: CPError) -> None:
        key = (art["task_id"], art["path"], art["sha256"])
        if exc.status == 413 or exc.code == "payload_too_large":
            self.store.update_artifact(*key, state="skipped", error="payload_too_large")
        elif exc.code == "quota_exceeded":
            self.store.update_artifact(*key, state="rejected", error="quota_exceeded")
        elif exc.status in (404, 410):
            self._reset_artifact(art, f"upload object gone ({exc.code})")
        else:
            attempts = int(art["attempts"]) + 1
            state = "rejected" if attempts >= 5 else art["state"]
            self.store.update_artifact(*key, attempts=attempts, state=state, error=exc.code,
                                       next_attempt_at=self.clock() + min(60, 2 ** attempts))
        log.warning("artifact %s of task %s: control plane error %s", art["name"], art["task_id"], exc.code)

    def _reset_artifact(self, art, reason: str) -> None:
        attempts = int(art["attempts"]) + 1
        state = "rejected" if attempts >= 3 else "pending"
        self.store.update_artifact(art["task_id"], art["path"], art["sha256"], artifact_id=None, upload_url=None,
                                   uploaded=0, attempts=attempts, state=state, error=reason)

    def _maybe_finish_task(self, task_id: str) -> None:
        task = self.store.get_task(task_id)
        if task and task["terminal"] and task["artifacts_listed"] and self.store.unfinished_artifacts(task_id) == 0:
            self.store.update_task(task_id, done=1)

    def _read_core(self, core_id: str, path: str, offset: int, limit: int) -> tuple[bytes, bool]:
        reply = self.core.workspace_read_bytes(core_id, path, offset, limit)
        try:
            data = base64.b64decode(reply.get("data_b64") or "", validate=True)
        except (ValueError, TypeError) as exc:
            raise CoreError("invalid base64 from workspace_read_bytes") from exc
        if int(reply.get("offset", offset)) != offset:
            raise CoreError("core returned an unexpected offset")
        return data, bool(reply.get("eof"))

    def _upload_artifact(self, art, core_id: str) -> bool:
        key = (art["task_id"], art["path"], art["sha256"])
        size = int(art["size"])
        artifact_id, upload_url = art["artifact_id"], art["upload_url"]
        chunk_size = int(art["chunk_size"] or MAX_CHUNK)
        if not artifact_id:
            reply = self._cp(self.cp.artifact_init, {"task_id": art["task_id"], "name": art["name"],
                                                    "mime": art["mime"], "size": size, "sha256": art["sha256"]})
            artifact_id = str(reply.get("artifact_id") or "")
            if not artifact_id:
                raise CPError(502, "invalid_reply", "artifact init returned no artifact_id")
            upload_url = str(reply.get("upload_url") or f"/v1/node/artifacts/{artifact_id}/content")
            chunk_size = int(reply.get("chunk_size") or MAX_CHUNK)
            self.store.update_artifact(*key, artifact_id=artifact_id, upload_url=upload_url,
                                       chunk_size=chunk_size, state="uploading", uploaded=0)
        upload_url = upload_url or f"/v1/node/artifacts/{artifact_id}/content"
        chunk_size = max(1, min(chunk_size, MAX_CHUNK))
        read_chunk = self.config.artifact_read_chunk

        received = self._cp(self.cp.artifact_received, upload_url) if size > 0 else 0
        received = max(0, min(int(received), size))
        hasher = hashlib.sha256()
        offset = 0
        while offset < received:  # re-hash the part the server already has
            data, eof = self._read_core(core_id, art["path"], offset, min(read_chunk, received - offset))
            if not data:
                raise CoreError("artifact shrank while uploading")
            hasher.update(data)
            offset += len(data)
        if received:
            log.info("resuming upload of artifact %s at byte %d/%d", art["name"], received, size)
        while offset < size:
            if self.stop_event.is_set():
                return False
            buf = bytearray()
            while len(buf) < chunk_size and offset + len(buf) < size:
                want = min(read_chunk, chunk_size - len(buf), size - offset - len(buf))
                data, eof = self._read_core(core_id, art["path"], offset + len(buf), want)
                buf += data
                if eof or not data:
                    break
            if not buf:
                raise CoreError("artifact shrank while uploading")
            hasher.update(buf)
            try:
                reply = self._cp(self.cp.artifact_put, upload_url, bytes(buf), offset, size)
            except CPError as exc:
                if exc.status in (409, 416):  # offset mismatch: resync with the server on next pass
                    log.warning("upload offset mismatch for artifact %s; will resume", art["name"])
                    return False
                raise
            offset += len(buf)
            got = reply.get("received") if isinstance(reply, dict) else None
            if isinstance(got, int) and got != offset:
                log.warning("server reports %d bytes received, expected %d; will resume", got, offset)
                self.store.update_artifact(*key, uploaded=got)
                return False
            self.store.update_artifact(*key, uploaded=offset)
        if hasher.hexdigest() != art["sha256"]:
            log.error("artifact %s changed while uploading (sha256 mismatch); rejected", art["name"])
            self.store.update_artifact(*key, state="rejected", error="content changed during upload")
            return False
        try:
            self._cp(self.cp.artifact_complete, artifact_id)
        except CPError as exc:
            if exc.status == 422 or exc.code == "checksum_mismatch":
                log.error("control plane rejected checksum of artifact %s; re-uploading", art["name"])
                self._reset_artifact(art, "checksum_mismatch")
                return False
            raise
        self.store.update_artifact(*key, state="available", uploaded=size, error=None)
        self.store.append_events(art["task_id"], [("artifact_available", utc_now(), {
            "artifact_id": artifact_id, "name": art["name"], "path": art["path"], "size": size,
            "sha256": art["sha256"], "mime": art["mime"]})])
        log.info("artifact %s of task %s uploaded (%d bytes)", art["name"], art["task_id"], size)
        return True

    # ------------------------------------------------------------- token rotate
    def rotate_token(self) -> None:
        reply = self._cp(self.cp.rotate_token)
        token = reply.get("node_token")
        if not isinstance(token, str) or not token.startswith("ncp_"):
            raise CPError(502, "invalid_reply", "rotate returned no node token")
        if self.credentials_store is not None:
            self.credentials = self.credentials_store.update(node_token=token)
        self.cp.token = token
        from .logutil import register_secret
        register_secret(token)
        log.info("node token rotated")

    # -------------------------------------------------------------------- loops
    def _loop(self, name: str, step: Callable[[], float]) -> None:
        backoff = Backoff(self.config.backoff_initial, self.config.backoff_max)
        while not self.stop_event.is_set():
            try:
                wait = step()
                backoff.reset()
            except NodeRevoked:
                break
            except RateLimited as exc:
                wait = max(0.0, exc.retry_after or 1.0)
                log.warning("%s: rate limited by control plane; retrying in %.1fs", name, wait)
            except (CPUnavailable, CoreUnavailable) as exc:
                wait = getattr(exc, "retry_after", None) or backoff.next()
                log.warning("%s: %s; retrying in %.1fs", name, exc, wait)
            except CPError as exc:
                wait = backoff.next()
                log.warning("%s: control plane error %s; retrying in %.1fs", name, exc.code, wait)
            except Exception:  # keep the daemon alive, but never spin
                wait = backoff.next()
                log.exception("%s: unexpected error; retrying in %.1fs", name, wait)
            if wait > 0:
                self.stop_event.wait(wait)

    def _heartbeat_step(self) -> float:
        self.heartbeat_once()
        return self.heartbeat_interval

    def _commands_step(self) -> float:
        self.poll_commands_once()
        return 0.0 if self.config.lease_wait_ms > 0 else self.config.event_poll_interval

    def _events_step(self) -> float:
        self.sync_events_once()
        return self.config.event_poll_interval

    def _artifacts_step(self) -> float:
        self.upload_artifacts_once()
        return self.config.event_poll_interval

    def start(self) -> None:
        steps = [("heartbeat", self._heartbeat_step), ("commands", self._commands_step),
                 ("events", self._events_step)]
        if self.config.artifacts_enabled:
            steps.append(("artifacts", self._artifacts_step))
        for name, step in steps:
            t = threading.Thread(target=self._loop, args=(name, step), name=f"asgend-node-{name}", daemon=True)
            t.start()
            self._threads.append(t)
        log.info("node %s running (protocol v%d)", self.node_id, PROTOCOL_VERSION)

    def stop(self) -> None:
        self.stop_event.set()

    def wait(self, join_timeout: float = 10.0) -> int:
        while not self.stop_event.is_set():
            self.stop_event.wait(0.5)
        deadline = time.monotonic() + join_timeout
        for t in self._threads:
            t.join(max(0.0, deadline - time.monotonic()))
        if not self.revoked:
            try:  # best effort: push what is buffered before exiting
                self.flush_events_once()
            except Exception as exc:  # noqa: BLE001
                log.info("final event flush skipped: %s", exc)
        log.info("node agent stopped%s", " (revoked)" if self.revoked else "")
        return EXIT_REVOKED if self.revoked else EXIT_OK

    def run(self) -> int:
        self.start()
        return self.wait()


def _loads(text: str):
    import json

    try:
        return json.loads(text)
    except ValueError:
        return {}
