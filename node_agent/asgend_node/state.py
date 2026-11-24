"""Durable local state of the Execution Node.

* ``credentials.json`` (mode 0600, atomic replace): node identity and ``ncp_`` token.
* ``node.sqlite3``: command dedupe, task mapping, per-task cursors, outbound event
  buffer and artifact upload progress.
"""

from __future__ import annotations

import json
import os
import sqlite3
import tempfile
import threading
import time
from datetime import datetime, timezone
from typing import Any, Iterable

CREDENTIALS_FILE = "credentials.json"
DB_FILE = "node.sqlite3"

TERMINAL_STATES = ("completed", "completed_without_model", "failed", "cancelled")


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def ensure_private_dir(path: str) -> None:
    os.makedirs(path, mode=0o700, exist_ok=True)
    if os.name == "posix":
        try:
            os.chmod(path, 0o700)
        except OSError:
            pass


def atomic_write_private(path: str, data: bytes) -> None:
    """Write ``data`` to ``path`` atomically with mode 0600 (never world-readable,
    not even transiently)."""
    directory = os.path.dirname(os.path.abspath(path))
    ensure_private_dir(directory)
    fd, tmp = tempfile.mkstemp(prefix=".tmp-", dir=directory)  # mkstemp creates 0600
    try:
        if os.name == "posix":
            os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.chmod(tmp, 0o600)
        os.replace(tmp, path)
        if os.name == "posix":
            dir_fd = os.open(directory, os.O_RDONLY)
            try:
                os.fsync(dir_fd)
            finally:
                os.close(dir_fd)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


class Credentials:
    FIELDS = ("server_url", "node_id", "workspace_id", "node_token", "name", "labels",
              "heartbeat_interval_seconds", "lease_seconds", "ca_file", "allow_insecure_http",
              "enrolled_at", "revoked", "revoked_at")

    def __init__(self, state_dir: str):
        self.state_dir = state_dir
        self.path = os.path.join(state_dir, CREDENTIALS_FILE)

    def exists(self) -> bool:
        return os.path.exists(self.path)

    def load(self) -> dict[str, Any]:
        with open(self.path, "rb") as fh:
            data = json.loads(fh.read().decode("utf-8"))
        if not isinstance(data, dict):
            raise ValueError("credentials file is not a JSON object")
        return data

    def save(self, data: dict[str, Any]) -> None:
        clean = {k: data[k] for k in self.FIELDS if k in data}
        atomic_write_private(self.path, (json.dumps(clean, indent=2, sort_keys=True) + "\n").encode("utf-8"))

    def update(self, **fields: Any) -> dict[str, Any]:
        data = self.load()
        data.update(fields)
        self.save(data)
        return data


SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS commands (
    command_id TEXT PRIMARY KEY,
    type TEXT NOT NULL,
    task_id TEXT,
    outcome TEXT NOT NULL,          -- accepted | rejected
    core_task_id TEXT,
    error TEXT,
    acked INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL,
    acked_at TEXT
);
CREATE TABLE IF NOT EXISTS tasks (
    task_id TEXT PRIMARY KEY,       -- remote rt_ id
    core_task_id TEXT NOT NULL,
    last_core_event_id INTEGER NOT NULL DEFAULT 0,
    next_seq INTEGER NOT NULL DEFAULT 1,
    last_state TEXT,
    attempts INTEGER NOT NULL DEFAULT 0,
    question_key TEXT,
    terminal INTEGER NOT NULL DEFAULT 0,
    artifacts_listed INTEGER NOT NULL DEFAULT 0,
    done INTEGER NOT NULL DEFAULT 0,
    needs_snapshot INTEGER NOT NULL DEFAULT 0,
    snapshot_json TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS tasks_core ON tasks(core_task_id);
CREATE TABLE IF NOT EXISTS outbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT NOT NULL,
    seq INTEGER NOT NULL,
    type TEXT NOT NULL,
    occurred_at TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    UNIQUE(task_id, seq)
);
CREATE TABLE IF NOT EXISTS artifacts (
    task_id TEXT NOT NULL,
    path TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    name TEXT NOT NULL,
    mime TEXT NOT NULL,
    size INTEGER NOT NULL,
    state TEXT NOT NULL DEFAULT 'pending',  -- pending|uploading|available|skipped|rejected
    artifact_id TEXT,
    upload_url TEXT,
    chunk_size INTEGER,
    uploaded INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0,
    error TEXT,
    next_attempt_at REAL NOT NULL DEFAULT 0,
    updated_at TEXT NOT NULL,
    PRIMARY KEY (task_id, path, sha256)
);
"""


class Store:
    """Thread-safe wrapper around the node SQLite database."""

    def __init__(self, state_dir: str):
        ensure_private_dir(state_dir)
        self.path = os.path.join(state_dir, DB_FILE)
        self._lock = threading.RLock()
        self._db = sqlite3.connect(self.path, check_same_thread=False, isolation_level=None, timeout=30)
        self._db.row_factory = sqlite3.Row
        with self._lock:
            self._db.execute("PRAGMA journal_mode=WAL")
            self._db.execute("PRAGMA synchronous=FULL")
            self._db.execute("PRAGMA foreign_keys=ON")
            self._db.executescript(SCHEMA)
        if os.name == "posix":
            for suffix in ("", "-wal", "-shm"):
                try:
                    os.chmod(self.path + suffix, 0o600)
                except OSError:
                    pass

    def close(self) -> None:
        with self._lock:
            self._db.close()

    def _tx(self):
        store = self

        class _Tx:
            def __enter__(self_inner):
                store._lock.acquire()
                store._db.execute("BEGIN IMMEDIATE")
                return store._db

            def __exit__(self_inner, exc_type, exc, tb):
                try:
                    store._db.execute("ROLLBACK" if exc_type else "COMMIT")
                finally:
                    store._lock.release()
                return False

        return _Tx()

    def _query(self, sql: str, params: Iterable[Any] = ()) -> list[sqlite3.Row]:
        with self._lock:
            return list(self._db.execute(sql, tuple(params)))

    def _exec(self, sql: str, params: Iterable[Any] = ()) -> int:
        with self._lock:
            return self._db.execute(sql, tuple(params)).rowcount

    # -- commands ------------------------------------------------------------
    def get_command(self, command_id: str) -> sqlite3.Row | None:
        rows = self._query("SELECT * FROM commands WHERE command_id=?", (command_id,))
        return rows[0] if rows else None

    def record_command(self, command_id: str, ctype: str, task_id: str | None, outcome: str,
                       core_task_id: str | None = None, error: str | None = None) -> None:
        self._exec("INSERT OR IGNORE INTO commands(command_id,type,task_id,outcome,core_task_id,error,created_at)"
                   " VALUES(?,?,?,?,?,?,?)", (command_id, ctype, task_id, outcome, core_task_id, error, utc_now()))

    def mark_command_acked(self, command_id: str) -> None:
        self._exec("UPDATE commands SET acked=1, acked_at=? WHERE command_id=?", (utc_now(), command_id))

    def count_commands(self) -> int:
        return int(self._query("SELECT COUNT(*) FROM commands")[0][0])

    # -- tasks -----------------------------------------------------------------
    def upsert_task(self, task_id: str, core_task_id: str) -> None:
        now = utc_now()
        with self._tx() as db:
            row = db.execute("SELECT core_task_id FROM tasks WHERE task_id=?", (task_id,)).fetchone()
            if row is None:
                db.execute("INSERT INTO tasks(task_id,core_task_id,created_at,updated_at,needs_snapshot)"
                           " VALUES(?,?,?,?,0)", (task_id, core_task_id, now, now))
            elif row[0] != core_task_id:
                db.execute("UPDATE tasks SET core_task_id=?, updated_at=? WHERE task_id=?",
                           (core_task_id, now, task_id))

    def get_task(self, task_id: str) -> sqlite3.Row | None:
        rows = self._query("SELECT * FROM tasks WHERE task_id=?", (task_id,))
        return rows[0] if rows else None

    def active_tasks(self) -> list[sqlite3.Row]:
        return self._query("SELECT * FROM tasks WHERE done=0 ORDER BY created_at")

    def all_tasks(self) -> list[sqlite3.Row]:
        return self._query("SELECT * FROM tasks ORDER BY created_at")

    def reopen_task(self, task_id: str) -> None:
        self._exec("UPDATE tasks SET terminal=0, done=0, artifacts_listed=0, updated_at=? WHERE task_id=?",
                   (utc_now(), task_id))

    def update_task(self, task_id: str, **fields: Any) -> None:
        if not fields:
            return
        cols = ", ".join(f"{k}=?" for k in fields)
        self._exec(f"UPDATE tasks SET {cols}, updated_at=? WHERE task_id=?",
                   (*fields.values(), utc_now(), task_id))

    def mark_snapshot_needed(self, task_id: str | None = None) -> None:
        if task_id is None:
            self._exec("UPDATE tasks SET needs_snapshot=1 WHERE done=0")
        else:
            self._exec("UPDATE tasks SET needs_snapshot=1 WHERE task_id=?", (task_id,))

    def tasks_needing_snapshot(self) -> list[sqlite3.Row]:
        return self._query("SELECT * FROM tasks WHERE needs_snapshot=1")

    def append_events(self, task_id: str, events: list[tuple[str, str, dict]],
                      last_core_event_id: int | None = None, **task_fields: Any) -> list[int]:
        """Atomically assign seq numbers, buffer events in the outbox and advance the
        task cursor. Either all of it is durable or nothing is."""
        seqs: list[int] = []
        with self._tx() as db:
            row = db.execute("SELECT next_seq FROM tasks WHERE task_id=?", (task_id,)).fetchone()
            if row is None:
                raise KeyError(task_id)
            seq = int(row[0])
            for etype, occurred_at, payload in events:
                db.execute("INSERT INTO outbox(task_id,seq,type,occurred_at,payload_json) VALUES(?,?,?,?,?)",
                           (task_id, seq, etype, occurred_at,
                            json.dumps(payload, ensure_ascii=False, separators=(",", ":"))))
                seqs.append(seq)
                seq += 1
            fields = dict(task_fields)
            fields["next_seq"] = seq
            if last_core_event_id is not None:
                fields["last_core_event_id"] = int(last_core_event_id)
            cols = ", ".join(f"{k}=?" for k in fields)
            db.execute(f"UPDATE tasks SET {cols}, updated_at=? WHERE task_id=?",
                       (*fields.values(), utc_now(), task_id))
        return seqs

    # -- outbox ---------------------------------------------------------------
    def pending_events(self, limit: int = 500) -> list[sqlite3.Row]:
        return self._query("SELECT * FROM outbox ORDER BY id LIMIT ?", (int(limit),))

    def outbox_size(self, task_id: str | None = None) -> int:
        if task_id is None:
            return int(self._query("SELECT COUNT(*) FROM outbox")[0][0])
        return int(self._query("SELECT COUNT(*) FROM outbox WHERE task_id=?", (task_id,))[0][0])

    def delete_events(self, ids: list[int]) -> None:
        if not ids:
            return
        with self._tx() as db:
            db.executemany("DELETE FROM outbox WHERE id=?", [(i,) for i in ids])

    # -- artifacts --------------------------------------------------------------
    def add_artifact(self, task_id: str, path: str, sha256: str, name: str, mime: str, size: int,
                     state: str = "pending", error: str | None = None) -> bool:
        return self._exec("INSERT OR IGNORE INTO artifacts(task_id,path,sha256,name,mime,size,state,error,updated_at)"
                          " VALUES(?,?,?,?,?,?,?,?,?)",
                          (task_id, path, sha256, name, mime, int(size), state, error, utc_now())) > 0

    def artifacts_to_upload(self, now: float | None = None) -> list[sqlite3.Row]:
        now = time.time() if now is None else now
        return self._query("SELECT * FROM artifacts WHERE state IN ('pending','uploading') AND next_attempt_at<=?"
                           " ORDER BY updated_at", (now,))

    def unfinished_artifacts(self, task_id: str) -> int:
        return int(self._query("SELECT COUNT(*) FROM artifacts WHERE task_id=? AND state IN ('pending','uploading')",
                               (task_id,))[0][0])

    def task_artifacts(self, task_id: str) -> list[sqlite3.Row]:
        return self._query("SELECT * FROM artifacts WHERE task_id=? ORDER BY path", (task_id,))

    def update_artifact(self, task_id: str, path: str, sha256: str, **fields: Any) -> None:
        cols = ", ".join(f"{k}=?" for k in fields)
        self._exec(f"UPDATE artifacts SET {cols}, updated_at=? WHERE task_id=? AND path=? AND sha256=?",
                   (*fields.values(), utc_now(), task_id, path, sha256))

    def counts(self) -> dict[str, int]:
        q = lambda sql: int(self._query(sql)[0][0])  # noqa: E731
        return {
            "tasks_total": q("SELECT COUNT(*) FROM tasks"),
            "tasks_active": q("SELECT COUNT(*) FROM tasks WHERE done=0"),
            "commands_seen": q("SELECT COUNT(*) FROM commands"),
            "commands_unacked": q("SELECT COUNT(*) FROM commands WHERE acked=0"),
            "events_buffered": q("SELECT COUNT(*) FROM outbox"),
            "artifacts_pending": q("SELECT COUNT(*) FROM artifacts WHERE state IN ('pending','uploading')"),
            "artifacts_available": q("SELECT COUNT(*) FROM artifacts WHERE state='available'"),
        }

    def get_meta(self, key: str) -> str | None:
        rows = self._query("SELECT value FROM meta WHERE key=?", (key,))
        return rows[0][0] if rows else None

    def set_meta(self, key: str, value: str) -> None:
        self._exec("INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                   (key, value))
