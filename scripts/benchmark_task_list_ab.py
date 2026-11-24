#!/usr/bin/env python3
"""Isolated A/B benchmark for db_list_tasks ordering.

The production C query is not modified by this script. It builds a temporary
SQLite database from the tasks schema/indexes in src/db.c, compares the current
single-sort query with a candidate that fetches the status bands in production
priority order, and rejects the candidate unless the complete ordered result
matches on deterministic fixtures including aging, ties, every status and LIMIT.

Measurements are local SQLite diagnostics, not production service SLOs.
"""
from __future__ import annotations

import argparse
import concurrent.futures
from contextlib import closing
import datetime as dt
import hashlib
import json
import os
import platform
import random
import subprocess
import sqlite3
import statistics
import sys
import threading
import time
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve()
PROJECT = HERE.parent.parent
DB_SOURCE = PROJECT / "src" / "db.c"
METRICS_SOURCE = PROJECT / "scripts" / "performance_metrics.py"

STATUSES = (
    "queued", "running", "waiting_input", "completed",
    "completed_without_model", "failed", "cancelled",
)
STATUS_DISTRIBUTION = (
    ("queued", 25), ("running", 10), ("waiting_input", 5),
    ("completed", 30), ("completed_without_model", 10),
    ("failed", 10), ("cancelled", 10),
)
SELECT_FIELDS = (
    "id,status,attempts,max_attempts,priority,cancel_requested,created_at,"
    "updated_at,session_id,error,"
    "CASE WHEN status='queued' THEN MIN(9,priority+CAST(MAX(0,(julianday(:now)-"
    "julianday(created_at))*86400/:aging) AS INTEGER)) ELSE priority END AS effective_priority,"
    "workspace_path,title,task_json,next_attempt_at,rowid AS _rowid"
)
ALL_ORDER = (
    "CASE status WHEN 'running' THEN 0 WHEN 'queued' THEN 1 ELSE 2 END, "
    "effective_priority DESC, created_at ASC, id ASC"
)

SCHEMA = """
CREATE TABLE tasks (
 id TEXT PRIMARY KEY, idempotency_key TEXT UNIQUE, task_json TEXT NOT NULL,
 session_id TEXT, workspace_path TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',
 status TEXT NOT NULL CHECK(status IN ('queued','running','waiting_input','completed','completed_without_model','failed','cancelled')),
 attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL,
 cancel_requested INTEGER NOT NULL DEFAULT 0, priority INTEGER NOT NULL DEFAULT 0 CHECK(priority BETWEEN 0 AND 9),
 result_json TEXT, error TEXT, next_attempt_at TEXT,
 created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE INDEX idx_tasks_status_created ON tasks(status, created_at);
CREATE INDEX idx_tasks_queue_priority ON tasks(status, priority DESC, created_at);
CREATE INDEX idx_ab_running_order ON tasks(priority DESC, created_at ASC, id ASC) WHERE status='running';
CREATE INDEX idx_ab_other_order ON tasks(priority DESC, created_at ASC, id ASC) WHERE status NOT IN ('running','queued');
"""


def require(condition: bool, message: str, details: Any = None) -> None:
    if not condition:
        raise RuntimeError(f"{message}: {details!r}")


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def iso_at(now: dt.datetime, age_seconds: int) -> str:
    return (now - dt.timedelta(seconds=age_seconds)).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def status_for(index: int) -> str:
    # Integer threshold selection avoids platform-dependent weighted sampling.
    x = (index * 37 + 11) % 100
    total = 0
    for status, weight in STATUS_DISTRIBUTION:
        total += weight
        if x < total:
            return status
    return "cancelled"


def populate_database(path: Path, size: int, anchor: str) -> None:
    now = dt.datetime.fromisoformat(anchor.replace("Z", "+00:00")).replace(tzinfo=None)
    rng = random.Random(20261028 + size)
    with sqlite3.connect(path) as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.executescript(SCHEMA)
        rows = []
        for i in range(size):
            # The first 56 rows intentionally force exact key ties in several
            # status bands so LIMIT boundary equivalence is tested, not assumed.
            if i < 56:
                status = ("running" if i < 8 else "queued" if i < 24 else "completed")
                priority = 5
                created_at = iso_at(now, 750)
            else:
                status = status_for(i)
                priority = rng.randrange(10)
                # Repeatable age buckets deliberately avoid aging boundaries by
                # 120–180 seconds so the direct C reference (SQLite 'now') and
                # fixed-anchor SQL oracle cannot drift to adjacent priority buckets.
                age = ((i % 41) * 300 + 120 + (i % 61)) if i % 3 else (rng.randrange(576) * 300 + 120 + rng.randrange(61))
                created_at = iso_at(now, age)
            task_id = f"task-{i:08d}"
            rows.append((
                task_id, None, '{"kind":"benchmark"}', f"session-{i % 100}",
                f"/workspace/{i % 37}", f"task title {i}", status, i % 4,
                5, 0, priority, None, None, None, created_at, created_at,
            ))
        conn.executemany(
            "INSERT INTO tasks(id,idempotency_key,task_json,session_id,workspace_path,title,status,"
            "attempts,max_attempts,cancel_requested,priority,result_json,error,next_attempt_at,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", rows)
        conn.commit()
        conn.execute("ANALYZE")
        conn.commit()
        conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")


def connect_readonly(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True, timeout=5.0)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA query_only=ON")
    conn.execute("PRAGMA busy_timeout=5000")
    return conn


def baseline_query(conn: sqlite3.Connection, limit: int, now: str, aging: int,
                   status: str | None = None) -> list[tuple[Any, ...]]:
    sql = f"SELECT {SELECT_FIELDS} FROM tasks"
    params: dict[str, Any] = {"now": now, "aging": aging, "limit": limit}
    if status:
        sql += " WHERE status=:status"
        params["status"] = status
        sql += " ORDER BY effective_priority DESC, created_at ASC, id ASC LIMIT :limit"
    else:
        sql += f" ORDER BY {ALL_ORDER} LIMIT :limit"
    return [tuple(row) for row in conn.execute(sql, params).fetchall()]


def candidate_query(conn: sqlite3.Connection, limit: int, now: str, aging: int,
                    status: str | None = None) -> list[tuple[Any, ...]]:
    params: dict[str, Any] = {"now": now, "aging": aging, "limit": limit}
    if status:
        params["status"] = status
        if status == "queued":
            sql = (f"SELECT {SELECT_FIELDS} FROM tasks WHERE status=:status "
                   "ORDER BY effective_priority DESC, created_at ASC, id ASC LIMIT :limit")
        else:
            # For a non-queued status effective_priority == priority exactly;
            # making this explicit allows the (status, priority, created_at)
            # index to satisfy ordering without a computed ORDER BY expression.
            sql = (f"SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,"
                   "updated_at,session_id,error,priority AS effective_priority,workspace_path,title,"
                   "task_json,next_attempt_at,rowid AS _rowid FROM tasks WHERE status=:status "
                   "ORDER BY priority DESC, created_at ASC, id ASC LIMIT :limit")
        return [tuple(row) for row in conn.execute(sql, params).fetchall()]

    # The current global order has three rank bands: running, queued, then all
    # remaining statuses. Each band contributes only the remaining top rows.
    # The last band deliberately combines all non-active statuses, matching the
    # current cross-status priority/created_at ordering.
    statements = [
        ("SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,"
         "session_id,error,priority AS effective_priority,workspace_path,title,task_json,next_attempt_at,"
         "rowid AS _rowid FROM tasks INDEXED BY idx_ab_running_order WHERE status='running' "
         "ORDER BY priority DESC, created_at ASC, id ASC LIMIT :limit", "running"),
        (f"SELECT {SELECT_FIELDS} FROM tasks WHERE status='queued' "
         "ORDER BY effective_priority DESC, created_at ASC, id ASC LIMIT :limit", "queued"),
        ("SELECT id,status,attempts,max_attempts,priority,cancel_requested,created_at,updated_at,"
         "session_id,error,priority AS effective_priority,workspace_path,title,task_json,next_attempt_at,"
         "rowid AS _rowid FROM tasks INDEXED BY idx_ab_other_order WHERE status NOT IN ('running','queued') "
         "ORDER BY priority DESC, created_at ASC, id ASC LIMIT :limit", "other"),
    ]
    result: list[tuple[Any, ...]] = []
    for sql, _band in statements:
        remaining = limit - len(result)
        if remaining <= 0:
            break
        band_params = {"limit": remaining, "now": now, "aging": aging}
        result.extend(tuple(row) for row in conn.execute(sql, band_params).fetchall())
    return result


def verify_equivalence(conn: sqlite3.Connection, size: int, limits: list[int], now: str,
                       aging: int) -> dict[str, Any]:
    checks = 0
    ids_compared = 0
    for status in [None, *STATUSES]:
        for limit in limits:
            expected = baseline_query(conn, limit, now, aging, status)
            actual = candidate_query(conn, limit, now, aging, status)
            require(actual == expected,
                    "A/B ordered task-list results differ",
                    {"size": size, "status": status or "all", "limit": limit,
                     "baseline_ids": [row[0] for row in expected[:20]],
                     "candidate_ids": [row[0] for row in actual[:20]],
                     "expected_count": len(expected), "actual_count": len(actual)})
            checks += 1
            ids_compared += len(expected)
    return {
        "passed": True, "checks": checks, "rows_compared": ids_compared,
        "status_filters": ["all", *STATUSES], "limits": limits,
        "includes_ties": True, "includes_priority_aging": True,
        "deterministic_tie_break": "id ASC (stable tertiary key explicitly added to db_list_tasks())",
    }


def plan_rows(conn: sqlite3.Connection, sql: str, params: dict[str, Any]) -> list[str]:
    return [str(row[3]) for row in conn.execute("EXPLAIN QUERY PLAN " + sql, params).fetchall()]


def query_plans(conn: sqlite3.Connection, now: str, aging: int, limit: int) -> dict[str, Any]:
    params = {"now": now, "aging": aging, "limit": limit}
    baseline = plan_rows(conn, f"SELECT {SELECT_FIELDS} FROM tasks ORDER BY {ALL_ORDER} LIMIT :limit", params)
    running = plan_rows(conn,
        "SELECT id FROM tasks INDEXED BY idx_ab_running_order WHERE status='running' "
        "ORDER BY priority DESC,created_at ASC,id ASC LIMIT :limit", params)
    queued = plan_rows(conn,
        f"SELECT {SELECT_FIELDS} FROM tasks WHERE status='queued' "
        "ORDER BY effective_priority DESC,created_at ASC,id ASC LIMIT :limit", params)
    other = plan_rows(conn,
        "SELECT id FROM tasks INDEXED BY idx_ab_other_order WHERE status NOT IN ('running','queued') "
        "ORDER BY priority DESC,created_at ASC,id ASC LIMIT :limit", params)
    return {
        "baseline_all": baseline,
        "candidate_running_band": running,
        "candidate_queued_band": queued,
        "candidate_other_band": other,
        "observations": {
            "baseline_all_uses_temp_sort": any("USE TEMP B-TREE FOR ORDER BY" in x for x in baseline),
            "candidate_running_uses_temp_sort": any("USE TEMP B-TREE FOR ORDER BY" in x for x in running),
            "candidate_queued_uses_temp_sort": any("USE TEMP B-TREE FOR ORDER BY" in x for x in queued),
            "candidate_other_uses_temp_sort": any("USE TEMP B-TREE FOR ORDER BY" in x for x in other),
            "candidate_running_index_used": any("idx_ab_running_order" in x for x in running),
            "candidate_other_index_used": any("idx_ab_other_order" in x for x in other),
        },
    }


def run_worker(db_path: Path, method: str, status: str | None, count: int,
               limit: int, now: str, aging: int, barrier: threading.Barrier) -> list[float]:
    conn = connect_readonly(db_path)
    samples: list[float] = []
    try:
        # Warm one query on this connection, then synchronize workers. Cold-open
        # and first-page costs are deliberately outside measured latency.
        if method == "baseline":
            baseline_query(conn, limit, now, aging, status)
        else:
            candidate_query(conn, limit, now, aging, status)
        barrier.wait(timeout=10)
        for _ in range(count):
            start = time.perf_counter_ns()
            if method == "baseline":
                baseline_query(conn, limit, now, aging, status)
            else:
                candidate_query(conn, limit, now, aging, status)
            samples.append((time.perf_counter_ns() - start) / 1_000_000.0)
    finally:
        conn.close()
    return samples


def run_batch(db_path: Path, method: str, concurrency: int, per_worker: int,
              limit: int, now: str, aging: int) -> dict[str, Any]:
    barrier = threading.Barrier(concurrency)
    wall_start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [executor.submit(run_worker, db_path, method, None, per_worker,
                                   limit, now, aging, barrier) for _ in range(concurrency)]
        samples = [value for future in futures for value in future.result(timeout=120)]
    wall_ms = (time.perf_counter() - wall_start) * 1000.0
    from performance_metrics import summarize_latency
    return {
        "sample_count": len(samples),
        "latency_ms": summarize_latency(samples),
        "wall_ms": round(wall_ms, 3),
        "throughput_queries_s": round(len(samples) / (wall_ms / 1000.0), 3) if wall_ms else 0.0,
        "error_count": 0,
        "raw_samples_ms": [round(x, 6) for x in samples],
    }


def to_api_row(row: tuple[Any, ...]) -> dict[str, Any]:
    """Project a selected SQL row into the public db_list_tasks JSON shape."""
    out: dict[str, Any] = {
        "task_id": row[0], "status": row[1], "attempts": row[2],
        "max_attempts": row[3], "priority": row[4],
        "cancel_requested": bool(row[5]), "effective_priority": row[10],
    }
    for key, index in (("created_at", 6), ("updated_at", 7), ("session_id", 8), ("error", 9),
                       ("next_attempt_at", 14)):
        if row[index] is not None:
            out[key] = row[index]
    if row[11]:
        out["workspace_path"] = row[11]
    if row[12]:
        out["title"] = row[12]
    return out


def validate_against_c_reference(db_path: Path, binary: Path, root: Path, aging: int,
                                 limits: list[int], now: str, size: int) -> dict[str, Any]:
    """Compare modeled reference and candidate rows to actual C db_list_tasks()."""
    root.mkdir(parents=True, exist_ok=True)
    command = [str(binary), str(db_path), str(root), str(aging),
               ",".join(str(limit) for limit in limits)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=45, check=False)
    require(completed.returncode == 0, "C db_list_tasks reference harness failed",
            {"rc": completed.returncode, "stderr": completed.stderr[-3000:]})
    try:
        actual_c = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"C reference returned invalid JSON: {exc}") from exc
    compared = 0
    with closing(connect_readonly(db_path)) as conn:
        for status in [None, *STATUSES]:
            for limit in limits:
                key = f"{status or 'all'}_{limit}"
                c_rows = actual_c.get(key)
                require(isinstance(c_rows, list), "C reference response key missing", key)
                baseline = [to_api_row(row) for row in baseline_query(conn, limit, now, aging, status)]
                candidate = [to_api_row(row) for row in candidate_query(conn, limit, now, aging, status)]
                require(baseline == c_rows, "modeled baseline differs from real db_list_tasks",
                        {"size": size, "key": key, "baseline_ids": [x["task_id"] for x in baseline[:20]],
                         "c_ids": [x.get("task_id") for x in c_rows[:20]]})
                require(candidate == c_rows, "candidate differs from real db_list_tasks ordered response",
                        {"size": size, "key": key, "candidate_ids": [x["task_id"] for x in candidate[:20]],
                         "c_ids": [x.get("task_id") for x in c_rows[:20]]})
                compared += len(c_rows)
    return {"passed": True, "checks": len(actual_c), "rows_compared_to_c": compared,
            "oracle": "actual C db_list_tasks() using same SQLite database",
            "reference_binary_sha256": file_sha256(binary)}


def benchmark_size(db_path: Path, size: int, runs: int, operations_per_worker: int,
                   concurrencies: list[int], limit: int, now: str, aging: int,
                   reference_binary: Path | None, reference_root: Path) -> dict[str, Any]:
    with closing(connect_readonly(db_path)) as conn:
        limits = [1, 10, 50, min(200, size)]
        correctness = verify_equivalence(conn, size, limits, now, aging)
        plans = query_plans(conn, now, aging, min(limit, size))
    c_reference = validate_against_c_reference(db_path, reference_binary, reference_root, aging, limits, now, size) if reference_binary else {
        "passed": None, "checks": 0, "note": "not run; pass --reference-binary to validate real C db_list_tasks()"}
    results: dict[str, Any] = {}
    for concurrency in concurrencies:
        key = str(concurrency)
        results[key] = {"runs": []}
        for run_index in range(runs):
            started_utc = dt.datetime.now(dt.timezone.utc).isoformat()
            order = ["baseline", "candidate"] if run_index % 2 == 0 else ["candidate", "baseline"]
            pair = {"run": run_index + 1, "started_utc": started_utc, "order": order, "methods": {}}
            for method in order:
                pair["methods"][method] = run_batch(db_path, method, concurrency,
                    operations_per_worker, min(limit, size), now, aging)
            pair["completed_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
            base = pair["methods"]["baseline"]
            candidate = pair["methods"]["candidate"]
            pair["candidate_speedup_median"] = round(
                base["latency_ms"]["median"] / candidate["latency_ms"]["median"], 4
            ) if candidate["latency_ms"]["median"] else None
            pair["candidate_throughput_ratio"] = round(
                candidate["throughput_queries_s"] / base["throughput_queries_s"], 4
            ) if base["throughput_queries_s"] else None
            results[key]["runs"].append(pair)
        baseline_samples = [s for run in results[key]["runs"] for s in run["methods"]["baseline"]["raw_samples_ms"]]
        candidate_samples = [s for run in results[key]["runs"] for s in run["methods"]["candidate"]["raw_samples_ms"]]
        from performance_metrics import summarize_latency
        base_summary = summarize_latency(baseline_samples)
        candidate_summary = summarize_latency(candidate_samples)
        results[key]["aggregate"] = {
            "runs": runs,
            "samples_per_method": len(baseline_samples),
            "baseline": base_summary,
            "candidate": candidate_summary,
            "median_speedup": round(base_summary["median"] / candidate_summary["median"], 4)
                if candidate_summary["median"] else None,
            "baseline_error_count": 0,
            "candidate_error_count": 0,
        }
    return {"rows": size, "correctness": correctness, "c_reference_validation": c_reference,
            "query_plans": plans, "results": results}


def cgroup_limit(path: Path) -> str | None:
    try:
        value = path.read_text(encoding="utf-8").strip()
        return None if value == "max" else value
    except OSError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", default="1000,10000,100000")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--operations-per-worker", type=int, default=10)
    parser.add_argument("--concurrencies", default="1,4")
    parser.add_argument("--limit", type=int, default=200)
    parser.add_argument("--aging-seconds", type=int, default=300)
    parser.add_argument("--reference-binary", type=Path, default=None,
                        help="optional compiled C harness that calls the real db_list_tasks()")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sizes = [int(x) for x in args.sizes.split(",") if x]
    concurrencies = [int(x) for x in args.concurrencies.split(",") if x]
    require(sizes and all(1 <= size <= 1_000_000 for size in sizes), "invalid sizes", sizes)
    require(args.runs >= 1 and args.operations_per_worker >= 1, "runs/operations must be positive")
    require(concurrencies and all(1 <= x <= 32 for x in concurrencies), "invalid concurrency", concurrencies)
    require(1 <= args.limit <= 200 and args.aging_seconds >= 1, "invalid limit/aging")
    # Ensure the measured expression and index creation stay aligned with the
    # real source before any benchmark numbers are emitted.
    source_text = DB_SOURCE.read_text(encoding="utf-8")
    for marker in ("idx_tasks_status_created", "idx_tasks_queue_priority",
                   "CASE WHEN status='queued' THEN MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)) ELSE priority END",
                   "ORDER BY CASE status WHEN 'running' THEN 0 WHEN 'queued' THEN 1 ELSE 2 END"):
        require(marker in source_text, "benchmark SQL/schema no longer matches src/db.c", marker)
    metrics_path = PROJECT / "scripts" / "performance_metrics.py"
    sys.path.insert(0, str(metrics_path.parent))
    now = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
    reference_binary = args.reference_binary.resolve() if args.reference_binary else None
    if reference_binary is not None:
        require(reference_binary.is_file() and os.access(reference_binary, os.X_OK),
                "reference binary must be an executable file", str(reference_binary))
    report: dict[str, Any] = {
        "schema": "asgend.task_list_ab.v1",
        "passed": False,
        "benchmark_type": "isolated SQLite A/B prototype; production SQL untouched",
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "environment": {
            "python": sys.version.split()[0], "sqlite": sqlite3.sqlite_version,
            "platform": platform.platform(), "machine": platform.machine(),
            "logical_cpus": os.cpu_count(),
            "cgroup_cpu_max": cgroup_limit(Path("/sys/fs/cgroup/cpu.max")),
            "cgroup_memory_max_bytes": cgroup_limit(Path("/sys/fs/cgroup/memory.max")),
            "db_c_sha256": file_sha256(DB_SOURCE),
            "benchmark_script_sha256": file_sha256(HERE),
            "metrics_script_sha256": file_sha256(metrics_path),
        },
        "parameters": {"sizes": sizes, "runs": args.runs,
            "operations_per_worker": args.operations_per_worker, "warmup_queries_per_worker": 1,
            "concurrencies": concurrencies, "limit": args.limit,
            "priority_aging_seconds": args.aging_seconds, "fixed_now": now},
        "correctness_policy": {
            "all_ordered_rows_must_match": True,
            "all_status_filters_tested": True,
            "aging_and_equal_keys_exercised": True,
            "direct_c_db_list_tasks_oracle": reference_binary is not None,
            "stable_tie_break": "id ASC appended to db_list_tasks() as a stable tertiary tie-break; baseline and candidate preserve this order",
            "production_sql_modified_for_deterministic_tie_break": True,
            "candidate_partial_indexes_applied_to_production": False,
        },
        "database_results": {},
    }
    try:
        with __import__("tempfile").TemporaryDirectory(prefix="asgend-task-list-ab-") as tempdir:
            for size in sizes:
                db_path = Path(tempdir) / f"tasks-{size}.sqlite"
                populate_database(db_path, size, now)
                report["database_results"][str(size)] = benchmark_size(
                    db_path, size, args.runs, args.operations_per_worker,
                    concurrencies, args.limit, now, args.aging_seconds, reference_binary,
                    Path(tempdir) / f"reference-root-{size}")
        report["passed"] = True
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"task-list A/B benchmark passed: sizes={sizes}, runs={args.runs}, concurrencies={concurrencies}")
        print(f"report={args.output}")
        return 0
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"task-list A/B benchmark failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
