#!/usr/bin/env python3
"""Reproducible local SQLite/task-store and Core artifact RPC benchmark.

The SQLite database uses the schema and hot SQL shapes from src/db.c. The
artifact half calls the actual local Core RPC and therefore measures the real
artifact walker/hash/JSON response path. Results are diagnostic, not production
SLOs. No task bodies, artifact contents, auth tokens, or absolute temp paths are
written to the report.
"""
from __future__ import annotations

import argparse
import base64
import concurrent.futures
import hashlib
import importlib.util
import json
import os
import platform
import random
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from performance_metrics import summarize_latency

TASK_BY_ID_SQL = (
    "SELECT id,status,attempts,max_attempts,cancel_requested,result_json,error,created_at,"
    "updated_at,session_id,priority,task_json,workspace_path,title,next_attempt_at "
    "FROM tasks WHERE id=?1"
)
TASK_LIST_ALL_SQL = (
    "SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,"
    "session_id,error,CASE WHEN status='queued' THEN "
    "MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)) "
    "ELSE priority END,workspace_path,title,task_json,next_attempt_at FROM tasks "
    "ORDER BY CASE status WHEN 'running' THEN 0 WHEN 'queued' THEN 1 ELSE 2 END, "
    "CASE WHEN status='queued' THEN "
    "MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)) "
    "ELSE priority END DESC, created_at ASC LIMIT ?2"
)
TASK_LIST_FILTER_SQL = (
    "SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,"
    "session_id,error,CASE WHEN status='queued' THEN "
    "MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?2) AS INTEGER)) "
    "ELSE priority END,workspace_path,title,task_json,next_attempt_at FROM tasks WHERE status=?1 "
    "ORDER BY CASE WHEN status='queued' THEN "
    "MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?2) AS INTEGER)) "
    "ELSE priority END DESC, created_at ASC LIMIT ?3"
)
TASK_EVENTS_SQL = (
    "SELECT event_id,event_type,actor,message,payload_json,created_at FROM task_events "
    "WHERE task_id=?1 AND event_id>?2 ORDER BY event_id ASC LIMIT ?3"
)
SESSION_CONTEXT_SQL = (
    "SELECT role,content FROM (SELECT message_id,role,content FROM session_messages "
    "WHERE session_id=?1 AND (task_id IS NULL OR task_id<>?2) "
    "ORDER BY message_id DESC LIMIT 6) ORDER BY message_id ASC"
)
TOKEN = "local-sqlite-artifact-performance-token"
TERMINAL = {"completed", "completed_without_model", "failed", "cancelled"}


def require(condition: bool, message: str, detail: Any = None) -> None:
    if not condition:
        raise RuntimeError(f"{message}: {detail!r}")


def parse_ints(text: str) -> list[int]:
    values = [int(part.strip()) for part in text.split(",") if part.strip()]
    if not values or any(value <= 0 for value in values):
        raise argparse.ArgumentTypeError("expected comma-separated positive integers")
    return values


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_metadata(binary: Path) -> dict[str, Any]:
    cache = binary.parent / "CMakeCache.txt"
    result: dict[str, Any] = {"cmake_compiler": None, "compiler_version": None, "build_type": None,
                              "cmake_version": None, "release_flags": None}
    cmake = shutil.which("cmake")
    if cmake:
        try:
            completed = subprocess.run([cmake, "--version"], capture_output=True, text=True, timeout=3, check=False)
            first_line = (completed.stdout or completed.stderr).splitlines()
            if completed.returncode == 0 and first_line:
                result["cmake_version"] = first_line[0]
        except (OSError, subprocess.TimeoutExpired):
            pass
    if not cache.is_file():
        return result
    wanted = {
        "CMAKE_C_COMPILER": "cmake_compiler", "CMAKE_C_COMPILER_VERSION": "compiler_version",
        "CMAKE_BUILD_TYPE": "build_type", "CMAKE_VERSION": "cmake_version",
        "CMAKE_C_FLAGS_RELEASE": "release_flags",
    }
    try:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(("#", "//")) or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.split(":", 1)[0]
            if key in wanted:
                result[wanted[key]] = value
    except OSError:
        pass
    compiler = result.get("cmake_compiler")
    if compiler:
        try:
            completed = subprocess.run([str(compiler), "--version"], capture_output=True, text=True,
                                       timeout=3, check=False)
            first_line = (completed.stdout or completed.stderr).splitlines()
            result["compiler_identity"] = first_line[0] if completed.returncode == 0 and first_line else None
            if result["compiler_version"] is None and result["compiler_identity"]:
                version_match = re.search(r"\b(\d+\.\d+(?:\.\d+)?)\b", result["compiler_identity"])
                if version_match:
                    result["compiler_version"] = version_match.group(1)
        except (OSError, subprocess.TimeoutExpired):
            result["compiler_identity"] = None
    return result


def cgroup_limits() -> dict[str, Any]:
    result: dict[str, Any] = {"cpu_quota_cores": None, "memory_max_bytes": None}
    try:
        parts = Path("/sys/fs/cgroup/cpu.max").read_text().split()
        if len(parts) == 2 and parts[0] != "max" and int(parts[1]) > 0:
            result["cpu_quota_cores"] = round(int(parts[0]) / int(parts[1]), 3)
    except (OSError, ValueError):
        pass
    try:
        value = Path("/sys/fs/cgroup/memory.max").read_text().strip()
        if value != "max":
            result["memory_max_bytes"] = int(value)
    except (OSError, ValueError):
        pass
    return result


def load_integration(binary: Path, project: Path):
    path = project / "tests" / "integration.py"
    old_argv = sys.argv[:]
    try:
        sys.argv = [str(path), str(binary), str(project)]
        spec = importlib.util.spec_from_file_location("asgend_sqlite_artifact_perf_integration", path)
        if spec is None or spec.loader is None:
            raise RuntimeError("cannot load existing Core integration helpers")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.argv = old_argv


def connect_db(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(path, timeout=5.0, isolation_level=None)
    conn.execute("PRAGMA busy_timeout=5000")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.execute("PRAGMA synchronous=FULL")
    return conn


def create_schema(path: Path) -> sqlite3.Connection:
    conn = connect_db(path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript("""
      CREATE TABLE tasks (
        id TEXT PRIMARY KEY, idempotency_key TEXT UNIQUE, task_json TEXT NOT NULL,
        session_id TEXT, workspace_path TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL CHECK(status IN ('queued','running','waiting_input','completed','completed_without_model','failed','cancelled')),
        attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL,
        cancel_requested INTEGER NOT NULL DEFAULT 0,
        priority INTEGER NOT NULL DEFAULT 0 CHECK(priority BETWEEN 0 AND 9),
        result_json TEXT, error TEXT, next_attempt_at TEXT,
        created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
        updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
      );
      CREATE INDEX idx_tasks_status_created ON tasks(status, created_at);
      CREATE INDEX idx_tasks_queue_priority ON tasks(status, priority DESC, created_at);
      CREATE TABLE sessions (
        session_id TEXT PRIMARY KEY,
        updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
      );
      CREATE TABLE session_messages (
        message_id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL,
        task_id TEXT, role TEXT NOT NULL, content TEXT NOT NULL,
        created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
      );
      CREATE INDEX idx_session_messages ON session_messages(session_id, message_id);
      CREATE TABLE task_events (
        event_id INTEGER PRIMARY KEY AUTOINCREMENT, task_id TEXT NOT NULL,
        event_type TEXT NOT NULL, actor TEXT NOT NULL, message TEXT NOT NULL,
        payload_json TEXT,
        created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
      );
      CREATE INDEX idx_task_events_task ON task_events(task_id, event_id);
      CREATE TABLE plan_items (
        item_id INTEGER PRIMARY KEY AUTOINCREMENT,
        scope_id TEXT NOT NULL DEFAULT 'global', kind TEXT NOT NULL CHECK(kind IN ('todo','goal')),
        title TEXT NOT NULL CHECK(length(title)>0 AND length(title)<=160),
        description TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'open' CHECK(status IN ('open','done')),
        sort_order INTEGER NOT NULL DEFAULT 0,
        created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
        updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
      );
      CREATE INDEX idx_plan_scope_kind ON plan_items(scope_id,kind,status,sort_order,item_id);
    """)
    return conn


def seed_database(conn: sqlite3.Connection, count: int) -> list[str]:
    session_count = min(1000, max(10, count // 10))
    conn.execute("BEGIN IMMEDIATE")
    conn.executemany("INSERT INTO sessions(session_id) VALUES(?)",
                     ((f"session-{i:05d}",) for i in range(session_count)))
    # Six rows/session represent the bounded context window used by db_session_context.
    conn.executemany(
        "INSERT INTO session_messages(session_id,task_id,role,content,created_at) VALUES(?,?,?,?,?)",
        ((f"session-{i:05d}", f"task-{i % count:09d}", "user" if j % 2 == 0 else "assistant",
          f"deterministic context message {j} for session {i}", f"2026-01-01T00:00:{j:02d}.000Z")
         for i in range(session_count) for j in range(6)),
    )
    conn.executemany(
        "INSERT INTO tasks(id,idempotency_key,task_json,session_id,workspace_path,title,status,attempts,max_attempts,priority,created_at,updated_at) "
        "VALUES(?,NULL,?,?,?,?,?,0,3,?,?,?)",
        ((f"task-{i:09d}", json.dumps({"task": "synthetic", "ordinal": i}, separators=(",", ":")),
          f"session-{i % session_count:05d}", "", f"Synthetic task {i}",
          ("queued", "running", "completed", "completed_without_model", "failed", "cancelled")[i % 6],
          i % 10, f"2026-01-01T00:{(i // 60) % 60:02d}:{i % 60:02d}.{i % 1000:03d}Z",
          f"2026-01-01T00:{(i // 60) % 60:02d}:{i % 60:02d}.{i % 1000:03d}Z") for i in range(count)),
    )
    conn.executemany(
        "INSERT INTO task_events(task_id,event_type,actor,message,payload_json,created_at) VALUES(?,?,?,?,?,?)",
        ((f"task-{i:09d}", "created", "benchmark", "seed event", None,
          f"2026-01-01T00:{(i // 60) % 60:02d}:{i % 60:02d}.000Z") for i in range(count)),
    )
    conn.execute("COMMIT")
    require(conn.execute("SELECT COUNT(*) FROM tasks").fetchone()[0] == count, "seed task count mismatch")
    require(conn.execute("SELECT COUNT(*) FROM task_events").fetchone()[0] == count, "seed event count mismatch")
    return [f"task-{i:09d}" for i in range(count)]


def explain(conn: sqlite3.Connection, sql: str, params: tuple[Any, ...]) -> list[str]:
    return [str(row[3]) for row in conn.execute("EXPLAIN QUERY PLAN " + sql, params).fetchall()]


def query_plans(conn: sqlite3.Connection) -> dict[str, Any]:
    plans = {
        "task_by_primary_key": explain(conn, TASK_BY_ID_SQL, ("task-000000000",)),
        "task_events_tail": explain(conn, TASK_EVENTS_SQL, ("task-000000000", 0, 100)),
        "session_context": explain(conn, SESSION_CONTEXT_SQL, ("session-00000", "")),
        "task_list_all": explain(conn, TASK_LIST_ALL_SQL, (2, 50)),
        "task_list_queued": explain(conn, TASK_LIST_FILTER_SQL, ("queued", 2, 50)),
    }
    joined = {key: " | ".join(value) for key, value in plans.items()}
    assertions = {
        "task_by_primary_key_uses_index": "USING INDEX" in joined["task_by_primary_key"].upper() or "USING COVERING INDEX" in joined["task_by_primary_key"].upper() or "INTEGER PRIMARY KEY" in joined["task_by_primary_key"].upper(),
        "task_events_uses_expected_index": "IDX_TASK_EVENTS_TASK" in joined["task_events_tail"].upper(),
        "session_context_uses_expected_index": "IDX_SESSION_MESSAGES" in joined["session_context"].upper(),
        "task_list_sort_behavior_reported": True,
    }
    require(all(assertions.values()), "a required SQLite query plan did not use its expected lookup index", {"plans": joined, "assertions": assertions})
    observations = {
        "all_task_list_uses_temp_sort": "TEMP B-TREE" in joined["task_list_all"].upper(),
        "queued_task_list_uses_status_index": ("IDX_TASKS_STATUS_CREATED" in joined["task_list_queued"].upper() or "IDX_TASKS_QUEUE_PRIORITY" in joined["task_list_queued"].upper()),
        "queued_task_list_uses_temp_sort": "TEMP B-TREE" in joined["task_list_queued"].upper(),
    }
    return {"details": plans, "assertions": assertions, "observations": observations}


def do_read(conn: sqlite3.Connection, operation: str, task_id: str, session_id: str) -> int:
    if operation == "task_by_id":
        rows = conn.execute(TASK_BY_ID_SQL, (task_id,)).fetchall()
        require(len(rows) == 1 and rows[0][0] == task_id, "task primary-key read failed")
        return len(rows)
    if operation == "task_list_all":
        rows = conn.execute(TASK_LIST_ALL_SQL, (2, 50)).fetchall()
        require(len(rows) <= 50, "unfiltered task list exceeded its limit")
        return len(rows)
    if operation == "task_list_queued":
        rows = conn.execute(TASK_LIST_FILTER_SQL, ("queued", 2, 50)).fetchall()
        require(len(rows) <= 50 and all(row[1] == "queued" for row in rows), "queued task list is invalid")
        return len(rows)
    if operation == "task_events_tail":
        rows = conn.execute(TASK_EVENTS_SQL, (task_id, 0, 100)).fetchall()
        require(all(row[2] == "benchmark" for row in rows), "task event read returned unexpected data")
        return len(rows)
    rows = conn.execute(SESSION_CONTEXT_SQL, (session_id, "" )).fetchall()
    require(len(rows) <= 6 and all(row[0] in {"user", "assistant"} for row in rows), "session context read is invalid")
    return len(rows)


def do_write(conn: sqlite3.Connection, task_id: str, op_id: int) -> dict[str, float]:
    started = time.perf_counter()
    begin_started = time.perf_counter()
    conn.execute("BEGIN IMMEDIATE")
    begin_wait = (time.perf_counter() - begin_started) * 1000.0
    conn.execute("UPDATE tasks SET updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?", (task_id,))
    require(conn.execute("SELECT changes()").fetchone()[0] == 1, "write updated the wrong number of tasks")
    conn.execute(
        "INSERT INTO task_events(task_id,event_type,actor,message,payload_json) VALUES(?,?,?,?,NULL)",
        (task_id, "perf_write", "benchmark", f"op-{op_id}"),
    )
    commit_started = time.perf_counter()
    conn.execute("COMMIT")
    commit_ms = (time.perf_counter() - commit_started) * 1000.0
    return {"latency_ms": (time.perf_counter() - started) * 1000.0,
            "lock_wait_ms": begin_wait, "commit_ms": commit_ms}


def atomicity_check(conn: sqlite3.Connection) -> dict[str, Any]:
    task_id = "task-000000000"
    before_updated = conn.execute("SELECT updated_at FROM tasks WHERE id=?", (task_id,)).fetchone()[0]
    before_events = conn.execute("SELECT COUNT(*) FROM task_events").fetchone()[0]
    conn.execute("CREATE TRIGGER perf_abort_event BEFORE INSERT ON task_events WHEN NEW.event_type='perf_abort' BEGIN SELECT RAISE(ABORT,'injected benchmark failure'); END")
    rolled_back = False
    try:
        conn.execute("BEGIN IMMEDIATE")
        conn.execute("UPDATE tasks SET updated_at='rollback-marker' WHERE id=?", (task_id,))
        conn.execute("INSERT INTO task_events(task_id,event_type,actor,message) VALUES(?,'perf_abort','benchmark','must rollback')", (task_id,))
        conn.execute("COMMIT")
    except sqlite3.IntegrityError:
        conn.execute("ROLLBACK")
        rolled_back = True
    finally:
        conn.execute("DROP TRIGGER perf_abort_event")
    after_updated = conn.execute("SELECT updated_at FROM tasks WHERE id=?", (task_id,)).fetchone()[0]
    after_events = conn.execute("SELECT COUNT(*) FROM task_events").fetchone()[0]
    require(rolled_back and before_updated == after_updated and before_events == after_events,
            "SQLite atomic rollback assertion failed", {"rolled_back": rolled_back, "before_updated": before_updated,
                                                       "after_updated": after_updated, "before_events": before_events,
                                                       "after_events": after_events})
    return {"passed": True, "task_update_rolled_back": True, "event_insert_rolled_back": True}


def run_workload(db_path: Path, task_ids: list[str], size: int, workload: str, concurrency: int,
                 run_index: int, operations_per_worker: int) -> tuple[list[dict[str, Any]], float]:
    barrier = threading.Barrier(concurrency)
    clock_lock = threading.Lock()
    phase_start: list[float | None] = [None]
    phase_finish: list[float | None] = [None]

    def worker(worker_index: int) -> list[dict[str, Any]]:
        conn = connect_db(db_path)
        rng = random.Random(size * 100000 + concurrency * 1000 + run_index * 31 + worker_index)
        session_count = min(1000, max(10, size // 10))
        session_id = f"session-{rng.randrange(session_count):05d}"
        results: list[dict[str, Any]] = []
        # A few unrecorded operations warm statement/page caches without including
        # database creation or connection establishment in timed samples.
        for warm_index in range(5):
            tid = task_ids[rng.randrange(size)]
            do_read(conn, ("task_by_id", "task_events_tail", "session_context")[warm_index % 3], tid, session_id)
        barrier.wait(timeout=10)
        with clock_lock:
            now = time.perf_counter()
            if phase_start[0] is None or now < phase_start[0]:
                phase_start[0] = now
        writes_percent = 10 if workload == "read_heavy" else 80
        try:
            for op_index in range(operations_per_worker):
                task_id = task_ids[rng.randrange(size)]
                operation_id = run_index * 1000000 + worker_index * operations_per_worker + op_index
                operation_started = time.perf_counter()
                is_write = rng.randrange(100) < writes_percent
                try:
                    if is_write:
                        record = do_write(conn, task_id, operation_id)
                        record["operation"] = "write_transaction"
                        record["error"] = False
                    else:
                        choices = ("task_by_id", "task_list_all", "task_list_queued", "task_events_tail", "session_context")
                        operation = choices[rng.randrange(len(choices))]
                        rows = do_read(conn, operation, task_id, session_id)
                        record = {"latency_ms": (time.perf_counter() - operation_started) * 1000.0,
                                  "operation": operation, "rows_returned": rows, "error": False}
                    results.append(record)
                except (sqlite3.Error, RuntimeError) as exc:
                    if conn.in_transaction:
                        conn.execute("ROLLBACK")
                    results.append({"latency_ms": (time.perf_counter() - operation_started) * 1000.0,
                                    "operation": "write_transaction" if is_write else "read", "error": True,
                                    "error_type": type(exc).__name__})
        finally:
            conn.close()
            with clock_lock:
                done = time.perf_counter()
                if phase_finish[0] is None or done > phase_finish[0]:
                    phase_finish[0] = done
        return results

    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        future_results = list(pool.map(worker, range(concurrency)))
    samples = [sample for group in future_results for sample in group]
    require(phase_start[0] is not None and phase_finish[0] is not None and phase_finish[0] >= phase_start[0],
            "workload phase timing was not captured")
    return samples, phase_finish[0] - phase_start[0]


def summary_samples(samples: list[dict[str, Any]]) -> dict[str, Any]:
    successful_samples = [sample for sample in samples if not sample["error"]]
    error_count = len(samples) - len(successful_samples)
    result: dict[str, Any] = {"operation_samples": len(samples),
                              "errors": error_count,
                              "error_rate": round(error_count / max(1, len(samples)), 6),
                              "latency_ms": summarize_latency([sample["latency_ms"] for sample in successful_samples])
                                  if successful_samples else None}
    by_operation: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for sample in samples:
        by_operation[sample["operation"]].append(sample)
    result["operations"] = {}
    for operation, items in sorted(by_operation.items()):
        good = [item for item in items if not item["error"]]
        op_result: dict[str, Any] = {
            "samples": len(items), "errors": len(items) - len(good),
            "error_rate": round((len(items) - len(good)) / max(1, len(items)), 6),
        }
        if good:
            op_result["latency_ms"] = summarize_latency([item["latency_ms"] for item in good])
        else:
            op_result["latency_ms"] = None
        lock_samples = [item["lock_wait_ms"] for item in good if "lock_wait_ms" in item]
        commit_samples = [item["commit_ms"] for item in good if "commit_ms" in item]
        if lock_samples:
            op_result["begin_immediate_wait_ms"] = summarize_latency(lock_samples)
        if commit_samples:
            op_result["commit_ms"] = summarize_latency(commit_samples)
        result["operations"][operation] = op_result
    return result


def benchmark_sqlite(args: argparse.Namespace) -> dict[str, Any]:
    results: dict[str, Any] = {}
    all_samples: list[dict[str, Any]] = []
    for size in args.db_sizes:
        with tempfile.TemporaryDirectory(prefix=f"asgend-sqlite-perf-{size}-") as temp:
            db_path = Path(temp) / "core.sqlite3"
            conn = create_schema(db_path)
            seed_start = time.perf_counter()
            task_ids = seed_database(conn, size)
            seed_seconds = time.perf_counter() - seed_start
            # With WAL, most freshly seeded pages may still be in the -wal sidecar.
            # Checkpoint before recording the main database file size so sizes at
            # 1k/10k/100k rows are comparable rather than dependent on auto-checkpoint.
            conn.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchall()
            db_bytes = db_path.stat().st_size
            plan_snapshot = query_plans(conn)
            size_data: dict[str, Any] = {"rows": size, "seed_seconds": round(seed_seconds, 3),
                                        "database_file_bytes": db_bytes,
                                        "query_plans": plan_snapshot, "workloads": {}}
            for workload in ("read_heavy", "write_heavy"):
                for concurrency in args.concurrencies:
                    group_samples: list[dict[str, Any]] = []
                    run_summaries = []
                    for run_index in range(args.runs):
                        run_start = datetime.now(timezone.utc).isoformat()
                        samples, elapsed = run_workload(db_path, task_ids, size, workload, concurrency,
                                                        run_index, args.operations_per_worker)
                        success_count = sum(1 for sample in samples if not sample["error"])
                        run_summaries.append({"run_index": run_index + 1, "started_at_utc": run_start,
                                              "elapsed_seconds": round(elapsed, 3),
                                              "operation_count": len(samples),
                                              "success_count": success_count,
                                              "error_count": len(samples) - success_count})
                        for sample in samples:
                            sample.update({"db_rows": size, "workload": workload,
                                           "concurrency": concurrency, "run_index": run_index + 1})
                        group_samples.extend(samples)
                    aggregate = summary_samples(group_samples)
                    aggregate["runs"] = run_summaries
                    aggregate["aggregate_throughput_ops_s"] = round(
                        sum(1 for s in group_samples if not s["error"]) /
                        max(0.000001, sum(run["elapsed_seconds"] for run in run_summaries)), 3)
                    size_data["workloads"][f"{workload}_c{concurrency}"] = aggregate
                    all_samples.extend(group_samples)
            require(conn.execute("SELECT COUNT(*) FROM tasks").fetchone()[0] == size,
                    "task count changed during workload", size)
            event_count = conn.execute("SELECT COUNT(*) FROM task_events").fetchone()[0]
            expected_events = size + sum(1 for sample in all_samples if sample.get("db_rows") == size and
                                         sample.get("operation") == "write_transaction" and not sample["error"])
            require(event_count == expected_events, "event count differs from successful writes",
                    {"rows": size, "events": event_count, "expected": expected_events})
            atomicity = atomicity_check(conn)
            quick_check = conn.execute("PRAGMA integrity_check").fetchone()[0]
            require(quick_check == "ok", "SQLite integrity_check failed", quick_check)
            size_data["correctness"] = {"task_rows": size, "event_rows": event_count,
                                        "atomicity": atomicity, "integrity_check": quick_check}
            results[str(size)] = size_data
            conn.close()
    return {"results": results, "summary": {"sqlite_error_count": sum(1 for x in all_samples if x["error"]),
                                               "raw_sample_count": len(all_samples),
                                               "integrity_checks_passed": len(results),
                                               "atomicity_checks_passed": len(results)},
            "raw_samples": all_samples}


def clear_workspace(workspace: Path) -> None:
    for child in workspace.iterdir():
        if child.is_symlink() or child.is_file():
            child.unlink()
        elif child.is_dir():
            shutil.rmtree(child)


def artifact_rpc(integration, port: int, op: str, **fields: Any) -> dict[str, Any]:
    response = integration.rpc(port, {"op": op, **fields}, token=TOKEN, timeout=15)
    require(isinstance(response, dict) and response.get("ok") is True,
            f"Core {op} RPC failed", {"op": op, "ok": response.get("ok") if isinstance(response, dict) else None})
    return response


def benchmark_artifacts(binary: Path, project: Path, args: argparse.Namespace) -> dict[str, Any]:
    integration = load_integration(binary, project)
    with tempfile.TemporaryDirectory(prefix="asgend-artifact-perf-") as temp:
        root = Path(temp)
        db_path = root / "core.sqlite3"
        port = integration.free_port()
        proc = integration.start_server(port, db_path, token=TOKEN, llm_enabled=False,
                                        sandbox_enabled=False, runtime_workers=1)
        try:
            submitted = artifact_rpc(integration, port, "submit", task="artifact benchmark workspace bootstrap")
            task_id = submitted.get("task_id")
            require(isinstance(task_id, str) and task_id, "Core did not return a task id")
            deadline = time.monotonic() + 10
            workspace = None
            while time.monotonic() < deadline:
                with sqlite3.connect(db_path, timeout=2) as db:
                    row = db.execute("SELECT workspace_path,status FROM tasks WHERE id=?", (task_id,)).fetchone()
                if row and row[0] and Path(row[0]).is_dir() and row[1] in TERMINAL:
                    workspace = Path(row[0])
                    break
                time.sleep(0.05)
            require(workspace is not None, "Core task workspace did not become ready")
            list_results: dict[str, Any] = {}
            artifact_samples: list[dict[str, Any]] = []
            for count in args.artifact_sizes:
                clear_workspace(workspace)
                fixture = workspace / "perf"
                fixture.mkdir()
                for index in range(count):
                    payload = (f"artifact-{index:08d}\n" + "x" * 55).encode("ascii")
                    (fixture / f"item-{index:08d}.txt").write_bytes(payload)
                scenario_results: dict[str, Any] = {}
                expected_count = min(count, 1000)
                for concurrency in args.concurrencies:
                    samples: list[dict[str, Any]] = []
                    errors = 0
                    for run_index in range(args.artifact_repeats):
                        def one_listing() -> tuple[dict[str, Any], float]:
                            started = time.perf_counter()
                            response = artifact_rpc(integration, port, "artifact_list", task_id=task_id)
                            return response, (time.perf_counter() - started) * 1000.0
                        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                            futures = [pool.submit(one_listing) for _ in range(concurrency)]
                            for future in futures:
                                try:
                                    response, latency = future.result(timeout=25)
                                    items = response.get("artifacts")
                                    require(isinstance(items, list) and response.get("count") == expected_count,
                                            "artifact list size/count mismatch", {"input_files": count,
                                                                                 "count": response.get("count"),
                                                                                 "expected": expected_count})
                                    require(bool(response.get("truncated")) == (count > 1000),
                                            "artifact truncation boundary mismatch", {"input_files": count,
                                                                                      "truncated": response.get("truncated")})
                                    perf_items = [item for item in items if str(item.get("path", "")).startswith("perf/")]
                                    require(len(perf_items) == expected_count, "artifact entries missing from response",
                                            {"input_files": count, "found": len(perf_items)})
                                    for item in perf_items[:1] + perf_items[-1:]:
                                        name = Path(str(item["path"])).name
                                        require(name.startswith("item-") and name.endswith(".txt"),
                                                "artifact path metadata was malformed", item.get("path"))
                                        ordinal = int(name[5:-4])
                                        expected_bytes = (f"artifact-{ordinal:08d}\n" + "x" * 55).encode("ascii")
                                        require(item.get("size") == len(expected_bytes) and
                                                item.get("sha256") == hashlib.sha256(expected_bytes).hexdigest() and
                                                item.get("mime") == "text/plain",
                                                "artifact content metadata/hash mismatch", item.get("path"))
                                    samples.append({"latency_ms": latency, "error": False})
                                except Exception as exc:
                                    errors += 1
                                    samples.append({"latency_ms": 0.0, "error": True, "error_type": type(exc).__name__})
                    all_good = [s["latency_ms"] for s in samples if not s["error"]]
                    scenario = {"input_files": count, "visible_entries": expected_count,
                                "truncated": count > 1000, "concurrency": concurrency,
                                "samples": len(samples), "errors": errors,
                                "error_rate": round(errors / max(1, len(samples)), 6),
                                "latency_ms": summarize_latency(all_good) if all_good else None}
                    scenario_results[str(concurrency)] = scenario
                    for sample in samples:
                        artifact_samples.append({**sample, "op": "artifact_list", "input_files": count,
                                                 "concurrency": concurrency})
                list_results[str(count)] = scenario_results
            # Measure the real workspace_read_bytes RPC at three payload sizes.
            clear_workspace(workspace)
            read_dir = workspace / "perf-read"
            read_dir.mkdir()
            read_fixtures: dict[int, tuple[bytes, str]] = {}
            for size in (4096, 65536, 1048576):
                payload = (hashlib.sha256(f"asgend-artifact-read-{size}".encode()).digest() * ((size // 32) + 1))[:size]
                (read_dir / f"payload-{size}.bin").write_bytes(payload)
                read_fixtures[size] = (payload, hashlib.sha256(payload).hexdigest())
            read_results: dict[str, Any] = {}
            for size, (expected_payload, expected_sha) in read_fixtures.items():
                read_scenarios = {}
                for concurrency in args.concurrencies:
                    latencies: list[float] = []
                    errors = 0
                    batch_wall_seconds = 0.0
                    for _ in range(args.artifact_repeats):
                        batch_started = time.perf_counter()
                        def one_read() -> tuple[float, bool]:
                            started = time.perf_counter()
                            reply = artifact_rpc(integration, port, "workspace_read_bytes", task_id=task_id,
                                                 path=f"perf-read/payload-{size}.bin", offset=0, limit=size)
                            elapsed = (time.perf_counter() - started) * 1000.0
                            decoded = base64.b64decode(reply.get("data_b64", ""), validate=True)
                            require(reply.get("size") == size and decoded == expected_payload and
                                    hashlib.sha256(decoded).hexdigest() == expected_sha,
                                    "workspace_read_bytes content verification failed", size)
                            return elapsed, True
                        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                            futures = [pool.submit(one_read) for _ in range(concurrency)]
                            for future in futures:
                                try:
                                    latency, _ = future.result(timeout=20)
                                    latencies.append(latency)
                                except Exception:
                                    errors += 1
                        batch_wall_seconds += time.perf_counter() - batch_started
                    read_scenarios[str(concurrency)] = {
                        "payload_bytes": size, "concurrency": concurrency, "samples": len(latencies) + errors,
                        "errors": errors, "error_rate": round(errors / max(1, len(latencies) + errors), 6),
                        "latency_ms": summarize_latency(latencies) if latencies else None,
                        "verified_payload_sha256": expected_sha,
                        "aggregate_throughput_mib_s": round((size * len(latencies)) / (1024 * 1024) /
                                                             max(0.000001, batch_wall_seconds), 3) if latencies else None,
                        "aggregate_wall_seconds": round(batch_wall_seconds, 3),
                    }
                read_results[str(size)] = read_scenarios
            return {"workspace_files": list_results, "workspace_read_bytes": read_results,
                    "summary": {"artifact_list_errors": sum(s["errors"] for v in list_results.values() for s in v.values()),
                                "workspace_read_errors": sum(s["errors"] for v in read_results.values() for s in v.values())}}
        finally:
            integration.stop_server(proc)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to the freshly built agent-orchestrator binary")
    parser.add_argument("project", type=Path, help="ASGEND source tree")
    parser.add_argument("--db-sizes", type=parse_ints, default=[1000, 10000, 100000])
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--operations-per-worker", type=int, default=100)
    parser.add_argument("--concurrencies", type=parse_ints, default=[1, 4])
    parser.add_argument("--artifact-sizes", type=parse_ints, default=[100, 1000, 1200])
    parser.add_argument("--artifact-repeats", type=int, default=5)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require(args.runs >= 1 and args.operations_per_worker >= 1 and args.artifact_repeats >= 1,
            "runs, operations, and artifact repeats must be positive")
    require(1000 in args.db_sizes or max(args.db_sizes) >= 1000, "benchmark should include at least 1k database rows")
    binary = args.binary.resolve()
    project = args.project.resolve()
    require(binary.is_file() and os.access(binary, os.X_OK), "Core binary is missing or not executable", str(binary))
    db_started = datetime.now(timezone.utc).isoformat()
    begin = time.perf_counter()
    sqlite_result = benchmark_sqlite(args)
    artifact_result = benchmark_artifacts(binary, project, args)
    metadata = {
        "started_at_utc": db_started,
        "finished_at_utc": datetime.now(timezone.utc).isoformat(),
        "elapsed_seconds": round(time.perf_counter() - begin, 3),
        "platform": platform.platform(), "python_version": platform.python_version(),
        "sqlite_version": sqlite3.sqlite_version,
        "cgroup_limits": cgroup_limits(),
        "build": build_metadata(binary),
        "hashes": {
            "core_binary_sha256": sha256_file(binary),
            "db_source_sha256": sha256_file(project / "src" / "db.c"),
            "artifact_source_sha256": sha256_file(project / "src" / "artifacts.c"),
            "benchmark_script_sha256": sha256_file(Path(__file__).resolve()),
        },
        "methodology": {
            "database_schema": "mirrors tables/indexes in src/db.c (tasks, sessions, session_messages, task_events, plan_items)",
            "database_modes": "WAL + synchronous=FULL + foreign_keys=ON + busy_timeout=5000ms",
            "warmup_reads_per_worker": 5,
            "lock_wait_metric": "elapsed time waiting for BEGIN IMMEDIATE; stdlib sqlite3 does not expose native busy-handler counters",
            "read_heavy_mix": "90% reads / 10% writes",
            "write_heavy_mix": "20% reads / 80% writes",
            "repetitions_per_database_profile": args.runs,
            "operations_per_worker_per_repetition": args.operations_per_worker,
            "concurrencies": args.concurrencies,
            "artifact_backend": "actual local Core TCP RPC with deterministic local files; no LLM/provider calls",
            "artifact_list_cap": 1000,
            "artifact_list_cap_source": "include/artifacts.h: AO_ARTIFACT_LIST_MAX",
            "p95_min_samples": 20,
            "p99_min_samples": 100,
        },
        "command_line": {"db_sizes": args.db_sizes, "runs": args.runs,
                         "operations_per_worker": args.operations_per_worker,
                         "concurrencies": args.concurrencies, "artifact_sizes": args.artifact_sizes,
                         "artifact_repeats": args.artifact_repeats},
    }
    report = {"schema_version": 1, "benchmark": "sqlite-artifacts", "passed": True,
              "metadata": metadata, "sqlite": sqlite_result, "artifacts": artifact_result}
    # One fresh clean profile should fail closed on operation errors or violated row/file invariants.
    report["passed"] = (sqlite_result["summary"]["sqlite_error_count"] == 0 and
                         artifact_result["summary"]["artifact_list_errors"] == 0 and
                         artifact_result["summary"]["workspace_read_errors"] == 0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "output": str(args.output),
                      "sqlite_samples": sqlite_result["summary"]["raw_sample_count"],
                      "sizes": args.db_sizes, "artifact_list_errors": artifact_result["summary"]["artifact_list_errors"],
                      "workspace_read_errors": artifact_result["summary"]["workspace_read_errors"],
                      "elapsed_seconds": metadata["elapsed_seconds"]}, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
