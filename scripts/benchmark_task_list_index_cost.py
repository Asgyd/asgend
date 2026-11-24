#!/usr/bin/env python3
"""Measure index write-maintenance cost vs task-list read benefit.

Compares the current production task indexes with the isolated partial-index
candidate from benchmark_task_list_ab.py. It covers two deterministic task-status
distributions, insert/status-transition/priority-update/delete+insert batches,
ordered-list reads, DB/index storage, and EXPLAIN QUERY PLAN. It does not alter
production indexes or claim production latency. Tail percentiles are suppressed
until the shared metrics policy has enough samples.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import random
import sqlite3
import statistics
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "scripts"))
import benchmark_task_list_ab as task_ab  # noqa: E402
from performance_metrics import summarize_latency  # noqa: E402

BASE_SCHEMA = task_ab.SCHEMA.replace(
    "CREATE INDEX idx_ab_running_order ON tasks(priority DESC, created_at ASC, id ASC) WHERE status='running';\n", ""
).replace(
    "CREATE INDEX idx_ab_other_order ON tasks(priority DESC, created_at ASC, id ASC) WHERE status NOT IN ('running','queued');\n", ""
)
CANDIDATE_INDEX_SQL = (
    "CREATE INDEX idx_ab_running_order ON tasks(priority DESC, created_at ASC, id ASC) WHERE status='running';",
    "CREATE INDEX idx_ab_other_order ON tasks(priority DESC, created_at ASC, id ASC) WHERE status NOT IN ('running','queued');",
)
DISTRIBUTIONS: dict[str, tuple[tuple[str, int], ...]] = {
    "balanced": task_ab.STATUS_DISTRIBUTION,
    "queued_heavy": (
        ("queued", 70), ("running", 10), ("waiting_input", 3),
        ("completed", 7), ("completed_without_model", 3), ("failed", 4), ("cancelled", 3),
    ),
}
STATUSES = ("queued", "running", "waiting_input", "completed", "completed_without_model", "failed", "cancelled")
COLUMNS = ("id", "idempotency_key", "task_json", "session_id", "workspace_path", "title", "status",
           "attempts", "max_attempts", "cancel_requested", "priority", "result_json", "error",
           "next_attempt_at", "created_at", "updated_at")
INSERT_SQL = "INSERT INTO tasks(" + ",".join(COLUMNS) + ") VALUES(" + ",".join("?" for _ in COLUMNS) + ")"


def require(condition: bool, message: str, details: Any = None) -> None:
    if not condition:
        raise RuntimeError(f"{message}: {details!r}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def cgroup_metadata() -> dict[str, Any]:
    result: dict[str, Any] = {"cpu_quota_cores": None, "cpu_max_raw": None, "memory_max_bytes": None}
    try:
        raw = Path("/sys/fs/cgroup/cpu.max").read_text(encoding="utf-8").strip()
        result["cpu_max_raw"] = raw
        fields = raw.split()
        if len(fields) == 2 and fields[0] != "max" and int(fields[1]) > 0:
            result["cpu_quota_cores"] = round(int(fields[0]) / int(fields[1]), 3)
    except (OSError, ValueError):
        pass
    try:
        raw_memory = Path("/sys/fs/cgroup/memory.max").read_text(encoding="utf-8").strip()
        if raw_memory != "max":
            result["memory_max_bytes"] = int(raw_memory)
    except (OSError, ValueError):
        pass
    return result


def pick_status(i: int, distribution: tuple[tuple[str, int], ...]) -> str:
    x = (i * 37 + 11) % 100
    current = 0
    for status, weight in distribution:
        current += weight
        if x < current:
            return status
    return distribution[-1][0]


def make_row(i: int, distribution: tuple[tuple[str, int], ...], *, prefix: str = "task") -> tuple[Any, ...]:
    rng = random.Random(0xA56E + i)
    status = pick_status(i, distribution)
    created = f"2026-10-{1 + (i % 27):02d}T00:{(i // 60) % 60:02d}:{i % 60:02d}.000Z"
    return (
        f"{prefix}-{i:09d}", None, '{"kind":"index-cost"}', f"session-{i % 1000:05d}",
        f"/workspace/{i % 97}", f"synthetic task {i}", status, i % 4, 5, 0, rng.randrange(10),
        None, None, None, created, created,
    )


def connect(path: Path) -> sqlite3.Connection:
    conn = sqlite3.connect(path, timeout=10.0, isolation_level=None)
    conn.execute("PRAGMA busy_timeout=10000")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=FULL")
    return conn


def create_db(path: Path, config: str) -> sqlite3.Connection:
    conn = connect(path)
    conn.executescript(BASE_SCHEMA)
    if config == "candidate_partial":
        for statement in CANDIDATE_INDEX_SQL:
            conn.execute(statement)
    elif config != "production_indexes":
        raise ValueError(config)
    return conn


def seed(conn: sqlite3.Connection, size: int, distribution: tuple[tuple[str, int], ...]) -> float:
    rows = [make_row(i, distribution) for i in range(size)]
    started = time.perf_counter()
    conn.execute("BEGIN IMMEDIATE")
    conn.executemany(INSERT_SQL, rows)
    conn.execute("COMMIT")
    return (time.perf_counter() - started) * 1000.0


def db_storage(conn: sqlite3.Connection, path: Path) -> dict[str, Any]:
    conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    conn.execute("ANALYZE")
    conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    page_size = int(conn.execute("PRAGMA page_size").fetchone()[0])
    page_count = int(conn.execute("PRAGMA page_count").fetchone()[0])
    dbstat_supported = False
    index_bytes: dict[str, int] = {}
    try:
        rows = conn.execute("SELECT name, SUM(pgsize) FROM dbstat GROUP BY name").fetchall()
        dbstat_supported = True
        index_names = {str(row[1]) for row in conn.execute("PRAGMA index_list(tasks)").fetchall()}
        index_bytes = {str(name): int(size or 0) for name, size in rows if name in index_names}
    except sqlite3.DatabaseError:
        pass
    return {
        "database_file_bytes_after_wal_checkpoint": path.stat().st_size,
        "page_size_bytes": page_size, "page_count": page_count,
        "freelist_pages": int(conn.execute("PRAGMA freelist_count").fetchone()[0]),
        "index_bytes_by_name": index_bytes,
        "index_bytes_total": sum(index_bytes.values()) if dbstat_supported else None,
        "dbstat_supported": dbstat_supported,
        "indexes": [str(row[1]) for row in conn.execute("PRAGMA index_list(tasks)").fetchall()],
    }


def query_plan(conn: sqlite3.Connection, config: str, now: str, aging: int, limit: int) -> list[str]:
    if config == "production_indexes":
        sql = f"SELECT {task_ab.SELECT_FIELDS} FROM tasks ORDER BY {task_ab.ALL_ORDER} LIMIT :limit"
    else:
        # query_plans in the original benchmark covers the specific candidate bands.
        sql = ("SELECT id,status,priority,created_at FROM tasks INDEXED BY idx_ab_running_order "
               "WHERE status='running' ORDER BY priority DESC,created_at ASC,id ASC LIMIT :limit")
    return [str(row[3]) for row in conn.execute(
        "EXPLAIN QUERY PLAN " + sql, {"now": now, "aging": aging, "limit": limit}
    ).fetchall()]


def timed_ms(fn) -> float:
    started = time.perf_counter()
    fn()
    return (time.perf_counter() - started) * 1000.0


def get_rows(conn: sqlite3.Connection, sql: str, params: tuple[Any, ...] = ()) -> list[tuple[Any, ...]]:
    return [tuple(row) for row in conn.execute(sql, params).fetchall()]


def measured_write(conn: sqlite3.Connection, operation: str, *, sample: int, size: int,
                   distribution: tuple[tuple[str, int], ...], batch_size: int) -> float:
    """Measure one committed transaction; state restoration is outside timing."""
    timestamp = "2026-10-28T12:00:00.000Z"
    if operation == "insert_batch":
        base = size + 1_000_000 + sample * batch_size
        rows = [make_row(base + offset, distribution, prefix="insert") for offset in range(batch_size)]
        started = time.perf_counter()
        conn.execute("BEGIN IMMEDIATE")
        conn.executemany(INSERT_SQL, rows)
        conn.execute("COMMIT")
        elapsed = (time.perf_counter() - started) * 1000.0
        conn.execute("BEGIN IMMEDIATE")
        conn.executemany("DELETE FROM tasks WHERE id=?", ((r[0],) for r in rows))
        conn.execute("COMMIT")
        return elapsed

    offset = (sample * batch_size) % max(1, size - batch_size)
    if operation == "status_transition":
        queued = [str(r[0]) for r in conn.execute(
            "SELECT id FROM tasks WHERE status='queued' ORDER BY id LIMIT ?", (batch_size,)).fetchall()]
        running = [str(r[0]) for r in conn.execute(
            "SELECT id FROM tasks WHERE status='running' ORDER BY id LIMIT ?", (batch_size,)).fetchall()]
        require(len(queued) == batch_size and len(running) == batch_size,
                "not enough queued/running rows for status transition", {"size": size, "batch": batch_size})
        started = time.perf_counter()
        conn.execute("BEGIN IMMEDIATE")
        conn.executemany("UPDATE tasks SET status='running',updated_at=? WHERE id=?", ((timestamp, x) for x in queued))
        conn.executemany("UPDATE tasks SET status='completed',updated_at=? WHERE id=?", ((timestamp, x) for x in running))
        conn.execute("COMMIT")
        elapsed = (time.perf_counter() - started) * 1000.0
        conn.execute("BEGIN IMMEDIATE")
        conn.executemany("UPDATE tasks SET status='queued' WHERE id=?", ((x,) for x in queued))
        conn.executemany("UPDATE tasks SET status='running' WHERE id=?", ((x,) for x in running))
        conn.execute("COMMIT")
        return elapsed

    if operation == "priority_update":
        rows = conn.execute("SELECT id,priority FROM tasks ORDER BY id LIMIT ? OFFSET ?", (batch_size, offset)).fetchall()
        originals = [(str(r[0]), int(r[1])) for r in rows]
        updates = [(int(priority + 1) % 10, task_id) for task_id, priority in originals]
        started = time.perf_counter()
        conn.execute("BEGIN IMMEDIATE")
        conn.executemany("UPDATE tasks SET priority=? WHERE id=?", updates)
        conn.execute("COMMIT")
        elapsed = (time.perf_counter() - started) * 1000.0
        conn.execute("BEGIN IMMEDIATE")
        conn.executemany("UPDATE tasks SET priority=? WHERE id=?", ((priority, task_id) for task_id, priority in originals))
        conn.execute("COMMIT")
        return elapsed

    if operation == "delete_reinsert":
        completed_count = int(conn.execute("SELECT COUNT(*) FROM tasks WHERE status='completed'").fetchone()[0])
        max_offset = max(0, completed_count - batch_size)
        completed_offset = (sample * batch_size) % (max_offset + 1)
        rows = conn.execute("SELECT * FROM tasks WHERE status='completed' ORDER BY id LIMIT ? OFFSET ?",
                            (batch_size, completed_offset)).fetchall()
        saved = [tuple(row) for row in rows]
        require(len(saved) == batch_size, "not enough completed rows for delete/reinsert",
                {"size": size, "batch": batch_size})
        started = time.perf_counter()
        conn.execute("BEGIN IMMEDIATE")
        conn.executemany("DELETE FROM tasks WHERE id=?", ((row[0],) for row in saved))
        conn.executemany(INSERT_SQL, saved)
        conn.execute("COMMIT")
        return (time.perf_counter() - started) * 1000.0

    raise ValueError(operation)


def run_queries(conn: sqlite3.Connection, config: str, now: str, aging: int, limit: int) -> list[tuple[Any, ...]]:
    if config == "production_indexes":
        return task_ab.baseline_query(conn, limit, now, aging)
    return task_ab.candidate_query(conn, limit, now, aging)


def prepare_one(path: Path, config: str, size: int, distribution: tuple[tuple[str, int], ...]):
    conn = create_db(path, config)
    seed_ms = seed(conn, size, distribution)
    conn.execute("ANALYZE")
    conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    return conn, seed_ms, db_storage(conn, path)


def run_scenario(size: int, distribution_name: str, runs: int, operations: int, workdir: Path,
                 anchor: str, aging: int, limit: int) -> dict[str, Any]:
    distribution = DISTRIBUTIONS[distribution_name]
    operations_out: dict[str, dict[str, list[float]]] = {
        config: {op: [] for op in ("list_read", "insert_batch", "status_transition", "priority_update", "delete_reinsert")}
        for config in ("production_indexes", "candidate_partial")
    }
    seed_samples: dict[str, list[float]] = {config: [] for config in operations_out}
    storage: dict[str, dict[str, Any]] = {}
    plans: dict[str, list[str]] = {}
    integrity_checks: dict[str, list[dict[str, Any]]] = {config: [] for config in operations_out}
    equivalence_checks = 0
    batch_size = min(max(size // 200, 5), 500)
    insert_batch_size = min(max(size // 1000, 5), 100)
    samples_per_run = operations

    for run_index in range(runs):
        base_path = workdir / f"{distribution_name}-{size}-base-{run_index}.sqlite"
        cand_path = workdir / f"{distribution_name}-{size}-cand-{run_index}.sqlite"
        configs = ["production_indexes", "candidate_partial"] if run_index % 2 == 0 else ["candidate_partial", "production_indexes"]
        connections: dict[str, sqlite3.Connection] = {}
        try:
            for config in configs:
                path = base_path if config == "production_indexes" else cand_path
                conn, seed_ms, size_info = prepare_one(path, config, size, distribution)
                connections[config] = conn
                seed_samples[config].append(seed_ms)
                storage[config] = size_info
                plans[config] = query_plan(conn, config, anchor, aging, min(limit, size))
            base_rows = run_queries(connections["production_indexes"], "production_indexes", anchor, aging, min(limit, size))
            cand_rows = run_queries(connections["candidate_partial"], "candidate_partial", anchor, aging, min(limit, size))
            require(base_rows == cand_rows,
                    "candidate ordered list differs from production-index query",
                    {"size": size, "distribution": distribution_name, "run": run_index + 1,
                     "baseline_ids": [r[0] for r in base_rows[:20]], "candidate_ids": [r[0] for r in cand_rows[:20]]})
            equivalence_checks += 1
            for config in configs:
                conn = connections[config]
                # One warm-up is deliberately excluded from the samples.
                run_queries(conn, config, anchor, aging, min(limit, size))
                for sample in range(samples_per_run):
                    operations_out[config]["list_read"].append(
                        timed_ms(lambda c=conn, cfg=config: run_queries(c, cfg, anchor, aging, min(limit, size))))
                    for operation in ("insert_batch", "status_transition", "priority_update", "delete_reinsert"):
                        latency = measured_write(conn, operation, sample=sample, size=size,
                                                 distribution=distribution,
                                                 batch_size=insert_batch_size if operation == "insert_batch" else batch_size)
                        operations_out[config][operation].append(latency)
                integrity = str(conn.execute("PRAGMA integrity_check").fetchone()[0])
                task_count = int(conn.execute("SELECT COUNT(*) FROM tasks").fetchone()[0])
                check = {"integrity_check": integrity, "task_count": task_count,
                         "expected_task_count": size, "passed": integrity == "ok" and task_count == size}
                integrity_checks[config].append(check)
                require(check["passed"], "database integrity/count invariant failed after write workload",
                        {"config": config, "run": run_index + 1, "check": check})
        finally:
            for conn in connections.values():
                try:
                    conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
                except sqlite3.DatabaseError:
                    pass
                conn.close()
            for p in (base_path, cand_path, Path(str(base_path) + "-wal"), Path(str(base_path) + "-shm"),
                      Path(str(cand_path) + "-wal"), Path(str(cand_path) + "-shm")):
                try:
                    p.unlink()
                except FileNotFoundError:
                    pass

    summary: dict[str, Any] = {}
    for config, op_values in operations_out.items():
        summary[config] = {
            "operations": {op: summarize_latency(values) for op, values in op_values.items()},
            "raw_samples_ms": op_values,
            "seed_insert_ms_per_run": seed_samples[config],
            "seed_insert_summary_ms": {
                "samples": len(seed_samples[config]),
                "min": round(min(seed_samples[config]), 3),
                "median": round(statistics.median(seed_samples[config]), 3),
                "max": round(max(seed_samples[config]), 3),
                "p95": None, "p99": None,
                "note": "tail percentiles intentionally suppressed: only five seed samples",
            },
            "storage_after_seed_and_wal_checkpoint": storage[config],
            "explain_query_plan": plans[config],
        }
    comparison = {}
    for operation in operations_out["production_indexes"]:
        baseline_median = summary["production_indexes"]["operations"][operation]["median"]
        candidate_median = summary["candidate_partial"]["operations"][operation]["median"]
        comparison[operation] = {
            "baseline_over_candidate_median_speedup": round(baseline_median / candidate_median, 4)
            if candidate_median else None,
            "candidate_write_overhead_percent": round((candidate_median / baseline_median - 1.0) * 100.0, 2)
            if baseline_median else None,
            "interpretation": "ratio > 1 means candidate faster; negative write_overhead means candidate writes faster",
        }
    baseline_index_bytes = summary["production_indexes"]["storage_after_seed_and_wal_checkpoint"]["index_bytes_total"]
    candidate_index_bytes = summary["candidate_partial"]["storage_after_seed_and_wal_checkpoint"]["index_bytes_total"]
    return {
        "rows": size, "status_distribution": distribution_name,
        "status_percentages": dict(distribution),
        "runs": runs, "samples_per_operation_per_config": runs * operations,
        "operations_per_run": operations,
        "batch_size": batch_size, "insert_batch_size": insert_batch_size,
        "ordered_read_equivalence_checks": equivalence_checks,
        "ordered_read_equivalence_passed": equivalence_checks == runs,
        "integrity_checks": integrity_checks,
        "integrity_checks_passed": all(check["passed"] for values in integrity_checks.values() for check in values),
        "integrity_check_count": sum(len(values) for values in integrity_checks.values()),
        "configs": summary,
        "comparison": comparison,
        "candidate_index_storage_overhead_bytes": (candidate_index_bytes - baseline_index_bytes)
            if baseline_index_bytes is not None and candidate_index_bytes is not None else None,
        "candidate_index_storage_overhead_percent": round((candidate_index_bytes / baseline_index_bytes - 1.0) * 100.0, 2)
            if baseline_index_bytes and candidate_index_bytes is not None else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", default="1000,10000,100000")
    parser.add_argument("--distributions", default="balanced,queued_heavy")
    parser.add_argument("--runs", type=int, default=5, help="independent DB pairs per size/distribution")
    parser.add_argument("--operations-per-run", type=int, default=20,
                        help="samples per operation per DB per run; p95 requires at least 20 and p99 100")
    parser.add_argument("--limit", type=int, default=200)
    parser.add_argument("--aging-seconds", type=int, default=300)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        sizes = [int(x) for x in args.sizes.split(",") if x]
        distributions = [x for x in args.distributions.split(",") if x]
        require(sizes and all(1 <= x <= 100_000 for x in sizes), "sizes must be in [1,100000]", sizes)
        require(args.runs >= 1 and args.operations_per_run >= 1, "runs/operations must be positive")
        require(distributions and all(x in DISTRIBUTIONS for x in distributions), "invalid distributions", distributions)
        require(1 <= args.limit <= 200 and 1 <= args.aging_seconds <= 86400, "invalid limit/aging")
        source_text = (PROJECT / "src" / "db.c").read_text(encoding="utf-8")
        for marker in ("idx_tasks_status_created", "idx_tasks_queue_priority",
                       "ORDER BY CASE status WHEN 'running' THEN 0 WHEN 'queued' THEN 1 ELSE 2 END",
                       "created_at ASC, id ASC"):
            require(marker in source_text, "benchmark assumptions drifted from src/db.c", marker)
        source_hash = sha256(PROJECT / "src" / "db.c")
        script_hash = sha256(Path(__file__).resolve())
        anchor = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
        report: dict[str, Any] = {
            "schema": "asgend.task_list_index_cost.v1", "passed": False,
            "benchmark_type": "local SQLite diagnostic; production schema/SQL unchanged by benchmark",
            "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "environment": {
                "python": sys.version.split()[0], "sqlite": sqlite3.sqlite_version,
                "platform": platform.platform(), "machine": platform.machine(),
                "logical_cpus": os.cpu_count(), "cgroup": cgroup_metadata(),
                "db_c_sha256": source_hash, "benchmark_script_sha256": script_hash,
            },
            "parameters": {"sizes": sizes, "distributions": distributions, "runs": args.runs,
                           "operations_per_run": args.operations_per_run,
                           "samples_per_operation_per_config": args.runs * args.operations_per_run,
                           "warmup_queries_per_run": 1, "limit": args.limit, "aging_seconds": args.aging_seconds,
                           "write_concurrency": 1,
                           "write_metrics_are_single_writer": True,
                           "percentile_policy": "shared helper: p95 >=20 samples, p99 >=100 samples"},
            "index_definitions": {
                "production": ["idx_tasks_status_created", "idx_tasks_queue_priority"],
                "candidate_extra": ["idx_ab_running_order (partial status=running)",
                                    "idx_ab_other_order (partial status NOT IN running,queued)"],
                "production_changed": False,
            },
            "results": {},
        }
        with tempfile.TemporaryDirectory(prefix="asgend-task-list-index-cost-") as temp:
            workdir = Path(temp)
            for size in sizes:
                report["results"][str(size)] = {}
                for distribution_name in distributions:
                    report["results"][str(size)][distribution_name] = run_scenario(
                        size, distribution_name, args.runs, args.operations_per_run,
                        workdir, anchor, args.aging_seconds, min(args.limit, size))
        report["passed"] = True
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"task-list index-cost benchmark passed: sizes={sizes}, distributions={distributions}, runs={args.runs}, ops={args.operations_per_run}")
        print(f"report={args.output}")
        return 0
    except Exception as exc:
        report = locals().get("report", {"schema": "asgend.task_list_index_cost.v1", "environment": {}})
        report["passed"] = False
        report["error"] = f"{type(exc).__name__}: {exc}"
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"task-list index-cost benchmark failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
