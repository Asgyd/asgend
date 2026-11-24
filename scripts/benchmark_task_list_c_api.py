#!/usr/bin/env python3
"""Benchmark production db_list_tasks() vs its isolated C candidate path.

The benchmark executable is compiled from the real src/db.c with a test-only
instrumentation define. It records API wall time, mutex wait, prepare time,
SQLite step time, C-to-JSON row mapping and json-c serialization. The candidate
path is exposed only in this benchmark binary; production builds keep using
only db_list_tasks(). All timings are local diagnostics, not production SLOs.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import statistics
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve()
PROJECT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(PROJECT / "scripts"))
import benchmark_task_list_ab as task_ab  # noqa: E402
from performance_metrics import summarize_latency  # noqa: E402

ANCHOR = "2026-10-28T00:00:00.000000Z"
DISTRIBUTIONS = ("fixture_10pct_running", "low_running_4")
METHODS = (
    ("api_ms", "api_ns"),
    ("mutex_wait_ms", "mutex_wait_ns"),
    ("prepare_ms", "prepare_ns"),
    ("sqlite_step_ms", "sqlite_step_ns"),
    ("json_mapping_ms", "json_mapping_ns"),
    ("serialization_ms", "serialization_ns"),
)


def require(ok: bool, message: str, details: Any = None) -> None:
    if not ok:
        raise RuntimeError(f"{message}: {details!r}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def cgroup_value(path: str) -> str | None:
    try:
        value = Path(path).read_text(encoding="utf-8").strip()
        return value if value and value != "max" else None
    except OSError:
        return None


def build_database(path: Path, root: Path, size: int, candidate: bool, distribution: str) -> dict[str, Any]:
    task_ab.populate_database(path, size, ANCHOR)
    with sqlite3.connect(path) as conn:
        if distribution == "low_running_4":
            # Four active tasks is more representative of a small local worker pool.
            # Move the fixture's running rows to queued first, then select exactly four.
            conn.execute("UPDATE tasks SET status='queued' WHERE status='running'")
            conn.execute("UPDATE tasks SET status='running' WHERE id IN (SELECT id FROM tasks ORDER BY id LIMIT ?)",
                         (min(4, size),))
        elif distribution != "fixture_10pct_running":
            raise ValueError(f"unknown task-status distribution: {distribution}")
        if not candidate:
            conn.execute("DROP INDEX IF EXISTS idx_ab_running_order")
            conn.execute("DROP INDEX IF EXISTS idx_ab_other_order")
        conn.commit()
        # Compact dropped candidate-index pages so the paired file-size comparison is meaningful.
        conn.execute("VACUUM")
        conn.execute("ANALYZE")
        conn.commit()
        conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        row_count = int(conn.execute("SELECT COUNT(*) FROM tasks").fetchone()[0])
        integrity = str(conn.execute("PRAGMA integrity_check").fetchone()[0])
        page_count = int(conn.execute("PRAGMA page_count").fetchone()[0])
        page_size = int(conn.execute("PRAGMA page_size").fetchone()[0])
        index_names = [row[0] for row in conn.execute(
            "SELECT name FROM sqlite_master WHERE type='index' AND name NOT LIKE 'sqlite_%' ORDER BY name")]
        status_counts = {str(status): int(count) for status, count in conn.execute(
            "SELECT status, COUNT(*) FROM tasks GROUP BY status ORDER BY status")}
        try:
            index_bytes_by_name = {str(name): int(size_bytes) for name, size_bytes in conn.execute(
                "SELECT name, SUM(pgsize) FROM dbstat "
                "WHERE name IN (SELECT name FROM sqlite_master WHERE type='index' AND name NOT LIKE 'sqlite_%') "
                "GROUP BY name ORDER BY name")}
            dbstat_available = True
        except sqlite3.OperationalError:
            index_bytes_by_name = {}
            dbstat_available = False
    require(row_count == size and integrity == "ok", "seeded database verification failed", {
        "path": str(path), "rows": row_count, "expected": size, "integrity": integrity})
    root.mkdir(parents=True, exist_ok=True)
    return {"rows": row_count, "integrity_check": integrity, "page_count": page_count,
            "page_size": page_size, "logical_bytes": page_count * page_size,
            "file_bytes_after_checkpoint_after_vacuum": path.stat().st_size,
            "candidate_indexes_present": candidate,
            "status_counts": status_counts,
            "running_task_count": status_counts.get("running", 0),
            "indexes": index_names,
            "dbstat_available": dbstat_available,
            "index_bytes_by_name": index_bytes_by_name,
            "total_index_bytes": sum(index_bytes_by_name.values()) if dbstat_available else None,
            "candidate_partial_index_bytes": sum(value for key, value in index_bytes_by_name.items() if key.startswith("idx_ab_")) if dbstat_available else None,
            "sha256": sha256(path)}


def inspect_query_plans(path: Path, mode: str, limit: int) -> dict[str, Any]:
    params = {"now": ANCHOR, "aging": 300, "limit": limit}
    plans: dict[str, list[str]] = {}
    with sqlite3.connect(path) as conn:
        def plan(name: str, sql: str) -> None:
            plans[name] = [str(row[3]) for row in conn.execute("EXPLAIN QUERY PLAN " + sql, params)]
        plan("production_global", f"SELECT {task_ab.SELECT_FIELDS} FROM tasks ORDER BY {task_ab.ALL_ORDER} LIMIT :limit")
        if mode == "candidate":
            plan("candidate_running", "SELECT id FROM tasks INDEXED BY idx_ab_running_order WHERE status='running' ORDER BY priority DESC,created_at ASC,id ASC LIMIT :limit")
            plan("candidate_queued", f"SELECT {task_ab.SELECT_FIELDS} FROM tasks WHERE status='queued' ORDER BY effective_priority DESC,created_at ASC,id ASC LIMIT :limit")
            plan("candidate_other", "SELECT id FROM tasks INDEXED BY idx_ab_other_order WHERE status NOT IN ('running','queued') ORDER BY priority DESC,created_at ASC,id ASC LIMIT :limit")
    plan_text = {key: " | ".join(value) for key, value in plans.items()}
    return {
        "plan_details": plans,
        "baseline_global_uses_temp_btree": any("USE TEMP B-TREE FOR ORDER BY" in row for row in plans["production_global"]),
        "candidate_running_index_used": any("idx_ab_running_order" in row for row in plans.get("candidate_running", [])),
        "candidate_other_index_used": any("idx_ab_other_order" in row for row in plans.get("candidate_other", [])),
        "candidate_running_uses_temp_btree": any("USE TEMP B-TREE FOR ORDER BY" in row for row in plans.get("candidate_running", [])),
        "candidate_queued_uses_temp_btree": any("USE TEMP B-TREE FOR ORDER BY" in row for row in plans.get("candidate_queued", [])),
        "candidate_other_uses_temp_btree": any("USE TEMP B-TREE FOR ORDER BY" in row for row in plans.get("candidate_other", [])),
        "human_readable": plan_text,
    }


def run_c(binary: Path, db_path: Path, root: Path, mode: str, *, aging: int, limit: int,
          samples_per_worker: int, concurrency: int) -> dict[str, Any]:
    command = [str(binary), str(db_path), str(root), str(aging), str(limit),
               str(samples_per_worker), str(concurrency), mode]
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    wall_start = time.perf_counter()
    completed = subprocess.run(command, capture_output=True, text=True,
                               timeout=180, check=False)
    process_wall_ms = (time.perf_counter() - wall_start) * 1000.0
    require(completed.returncode == 0, "C API benchmark helper failed", {
        "mode": mode, "rows": db_path.name, "concurrency": concurrency,
        "return_code": completed.returncode, "stderr": completed.stderr[-3000:]})
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"C benchmark returned invalid JSON: {exc}: {completed.stdout[-1000:]}") from exc
    expected = samples_per_worker * concurrency
    require(report.get("passed") is True and report.get("error_count") == 0 and
            report.get("sample_count") == expected, "C API benchmark incomplete", {
                "mode": mode, "concurrency": concurrency, "expected_samples": expected,
                "sample_count": report.get("sample_count"), "errors": report.get("error_count")})
    require(isinstance(report.get("first_result"), list), "C benchmark omitted API result for equivalence", mode)
    return {"mode": mode, "started_utc": started, "completed_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "process_wall_ms": round(process_wall_ms, 3), "command": command,
            "report": report}


def collect_samples(run: dict[str, Any]) -> list[dict[str, Any]]:
    return run["report"]["samples"]


def aggregate_runs(runs: list[dict[str, Any]]) -> dict[str, Any]:
    all_samples = [sample for run in runs for sample in collect_samples(run)]
    latency: dict[str, Any] = {}
    for label, field in METHODS:
        vals = [float(sample[field]) / 1_000_000.0 for sample in all_samples]
        latency[label] = summarize_latency(vals)
    initial_calls = [call for run in runs for call in run["report"]["initial_calls_after_db_init"]]
    first_call_ms = [float(call["api_ns"]) / 1_000_000.0 for call in initial_calls]
    serial_bytes = [int(s["serialized_bytes"]) for s in all_samples]
    walls = [float(run["report"]["wall_ns"]) / 1e9 for run in runs]
    throughputs = [float(run["report"]["throughput_calls_per_second"]) for run in runs]
    return {
        "samples": len(all_samples),
        "latency": latency,
        "initial_call_per_worker_ms": summarize_latency(first_call_ms),
        "serialized_bytes_per_call": summarize_latency([float(x) for x in serial_bytes]),
        "run_wall_throughput_calls_per_second": summarize_latency(throughputs),
        "run_measured_wall_seconds": [round(x, 6) for x in walls],
        "error_count": sum(int(run["report"]["error_count"]) for run in runs),
        "total_payload_bytes": sum(serial_bytes),
        "timing_interpretation": {
            "api_ms": "db_list_tasks API wall time, includes db_mutex wait and JSON object construction",
            "mutex_wait_ms": "time blocked acquiring Runtime.db_mutex",
            "prepare_ms": "SQLite statement preparation time; candidate sums its 1-3 band statements",
            "sqlite_step_ms": "time inside sqlite3_step calls; excludes C row mapping between calls",
            "json_mapping_ms": "C mapping from SQLite row columns into json-c objects",
            "serialization_ms": "json-c rendering after API returns, measured outside API wall time",
            "initial_call_per_worker_ms": "each worker’s first API call after db_init; calls may overlap while workers start and this is not an OS page-cache cold-cache measure",
        },
    }


def compare_rows(base: dict[str, Any], candidate: dict[str, Any]) -> None:
    require(base == candidate, "C API baseline and candidate result JSON differ", {
        "baseline_count": len(base) if isinstance(base, list) else None,
        "candidate_count": len(candidate) if isinstance(candidate, list) else None,
        "baseline_ids": [x.get("task_id") for x in base[:12]] if isinstance(base, list) else None,
        "candidate_ids": [x.get("task_id") for x in candidate[:12]] if isinstance(candidate, list) else None})


def benchmark(binary: Path, sizes: list[int], runs_per_cell: int, samples_per_worker: int,
              concurrencies: list[int], limit: int, output: Path) -> dict[str, Any]:
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    report: dict[str, Any] = {
        "schema_version": 1,
        "benchmark": "task_list_c_api_baseline_vs_candidate",
        "started_utc": started,
        "passed": False,
        "configuration": {"sizes": sizes, "status_distributions": list(DISTRIBUTIONS), "runs_per_cell": runs_per_cell,
                           "samples_per_worker": samples_per_worker, "concurrencies": concurrencies,
                           "limit": limit, "priority_aging_seconds": 300,
                           "warmup_calls_per_worker": 3,
                           "result_limit_api_max": 200,
                           "fixture_anchor": ANCHOR},
        "environment": {
            "platform": platform.platform(), "python": sys.version.split()[0],
            "sqlite_python": sqlite3.sqlite_version,
            "cpu_count_logical": os.cpu_count(),
            "cgroup_cpu_max": cgroup_value("/sys/fs/cgroup/cpu.max"),
            "cgroup_memory_max": cgroup_value("/sys/fs/cgroup/memory.max"),
            "gcc_version": subprocess.run(["gcc", "--version"], capture_output=True, text=True,
                                           timeout=5).stdout.splitlines()[0],
            "binary_sha256": sha256(binary),
            "db_source_sha256": sha256(PROJECT / "src" / "db.c"),
            "helper_source_sha256": sha256(PROJECT / "tests" / "task_list_c_api_benchmark.c"),
            "metrics_source_sha256": sha256(PROJECT / "scripts" / "performance_metrics.py"),
            "benchmark_source_sha256": sha256(HERE),
        },
        "methodology": {
            "actual_c_api": True,
            "production_method": "db_list_tasks(Runtime*, NULL, limit, ...), compiled from src/db.c",
            "candidate_method": "db_list_tasks_candidate(), test-only path compiled with ASGEND_DB_PERF_BENCHMARK",
            "candidate_indexes": ["idx_ab_running_order", "idx_ab_other_order"],
            "production_source_modified_for_instrumentation": False,
            "production_partial_indexes_added": False,
            "database_pairing": "same deterministic task fixture, separate DB files, order alternated across paired runs",
            "warmup": "three calls per worker excluded from samples",
            "cold_cache": "OS cache is not flushed; first call after db_init reported separately and is not labelled cold cache",
            "serialization": "json-c compact serialization timed separately after API return",
            "mutex": "Runtime.db_mutex acquisition wait instrumented only in this benchmark target; production build has instrumentation compiled out",
            "samples_are_raw": True,
        },
        "database_fixtures": {},
        "results": {},
    }
    total_errors = 0
    with tempfile.TemporaryDirectory(prefix="asgend-c-api-perf-") as raw:
        work = Path(raw)
        for size in sizes:
            report["database_fixtures"][str(size)] = {}
            report["results"][str(size)] = {}
            for distribution in DISTRIBUTIONS:
                dbs = {}
                db_info = {}
                for mode in ("baseline", "candidate"):
                    path = work / f"tasks-{size}-{distribution}-{mode}.sqlite"
                    db_info[mode] = build_database(path, work / f"root-{size}-{distribution}-{mode}",
                                                   size, mode == "candidate", distribution)
                    dbs[mode] = path
                report["database_fixtures"][str(size)][distribution] = db_info
                report.setdefault("query_plans", {}).setdefault(str(size), {})[distribution] = {
                    mode: inspect_query_plans(dbs[mode], mode, min(limit, 200))
                    for mode in ("baseline", "candidate")}
                report["results"][str(size)][distribution] = {}
                for concurrency in concurrencies:
                    key = str(concurrency)
                    mode_runs: dict[str, list[dict[str, Any]]] = {"baseline": [], "candidate": []}
                    paired = []
                    for run_index in range(runs_per_cell):
                        order = ["baseline", "candidate"] if run_index % 2 == 0 else ["candidate", "baseline"]
                        run_pair: dict[str, Any] = {"run": run_index + 1, "order": order, "methods": {}}
                        for mode in order:
                            run = run_c(binary, dbs[mode], work / f"runtime-{size}-{distribution}-{mode}-{concurrency}-{run_index}",
                                        mode, aging=300, limit=min(limit, 200),
                                        samples_per_worker=samples_per_worker, concurrency=concurrency)
                            mode_runs[mode].append(run)
                            run_pair["methods"][mode] = run
                        baseline_result = run_pair["methods"]["baseline"]["report"]["first_result"]
                        candidate_result = run_pair["methods"]["candidate"]["report"]["first_result"]
                        compare_rows(baseline_result, candidate_result)
                        base_med = statistics.median(s["api_ns"] for s in collect_samples(run_pair["methods"]["baseline"]))
                        cand_med = statistics.median(s["api_ns"] for s in collect_samples(run_pair["methods"]["candidate"]))
                        base_tput = float(run_pair["methods"]["baseline"]["report"]["throughput_calls_per_second"])
                        cand_tput = float(run_pair["methods"]["candidate"]["report"]["throughput_calls_per_second"])
                        run_pair["candidate_api_speedup_median"] = round(base_med / cand_med, 5) if cand_med else None
                        run_pair["candidate_throughput_ratio"] = round(cand_tput / base_tput, 5) if base_tput else None
                        paired.append({"run": run_index + 1, "order": order,
                                       "candidate_api_speedup_median": run_pair["candidate_api_speedup_median"],
                                       "candidate_throughput_ratio": run_pair["candidate_throughput_ratio"],
                                       "rows_equal": True,
                                       "baseline_throughput": base_tput, "candidate_throughput": cand_tput})
                    baseline_agg = aggregate_runs(mode_runs["baseline"])
                    candidate_agg = aggregate_runs(mode_runs["candidate"])
                    total_errors += baseline_agg["error_count"] + candidate_agg["error_count"]
                    report["results"][str(size)][distribution][key] = {
                        "rows": size, "status_distribution": distribution,
                        "running_task_count": db_info["baseline"]["running_task_count"],
                        "concurrency": concurrency, "paired_runs": paired,
                        "baseline": baseline_agg, "candidate": candidate_agg,
                        "candidate_api_speedup_median": round(
                            baseline_agg["latency"]["api_ms"]["median"] /
                            candidate_agg["latency"]["api_ms"]["median"], 5)
                        if candidate_agg["latency"]["api_ms"]["median"] else None,
                        "candidate_throughput_ratio_median": round(
                            candidate_agg["run_wall_throughput_calls_per_second"]["median"] /
                            baseline_agg["run_wall_throughput_calls_per_second"]["median"], 5)
                        if baseline_agg["run_wall_throughput_calls_per_second"]["median"] else None,
                        "correctness": {"passed": True, "pair_runs_compared": runs_per_cell,
                                        "ordered_result_json_equal": True,
                                        "production_api": "db_list_tasks", "candidate_api": "db_list_tasks_candidate"},
                        "raw_runs": mode_runs,
                    }
    report["completed_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
    report["total_error_count"] = total_errors
    report["passed"] = total_errors == 0
    report["summary"] = {
        "configuration_cells": len(sizes) * len(DISTRIBUTIONS) * len(concurrencies),
        "paired_runs": len(sizes) * len(DISTRIBUTIONS) * len(concurrencies) * runs_per_cell,
        "all_pairs_ordered_json_equal": True,
        "production_indexes_only": True,
        "candidate_indexes_isolated": True,
        "error_count": total_errors,
        "recommendation": "Keep partial indexes isolated until C API latency, write cost, storage overhead and concurrency results support the same decision.",
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="test-task-list-c-api-benchmark executable")
    parser.add_argument("--sizes", default="1000,10000,100000")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--samples-per-worker", type=int, default=20)
    parser.add_argument("--concurrencies", default="1,4")
    parser.add_argument("--limit", type=int, default=200)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require(args.binary.is_file(), "benchmark binary not found", str(args.binary))
    sizes = [int(x) for x in args.sizes.split(",") if x]
    concurrencies = [int(x) for x in args.concurrencies.split(",") if x]
    require(sizes and all(1 <= x <= 1_000_000 for x in sizes), "invalid sizes", sizes)
    require(1 <= args.runs <= 20, "runs must be in [1,20]", args.runs)
    require(1 <= args.samples_per_worker <= 1000, "samples-per-worker must be in [1,1000]", args.samples_per_worker)
    require(concurrencies and all(1 <= x <= 64 for x in concurrencies), "invalid concurrencies", concurrencies)
    require(1 <= args.limit <= 200, "limit must be in [1,200]", args.limit)
    report = benchmark(args.binary.resolve(), sizes, args.runs, args.samples_per_worker,
                       concurrencies, args.limit, args.output.resolve())
    print(json.dumps({"passed": report["passed"], "output": str(args.output.resolve()),
                      "cells": report["summary"]["configuration_cells"],
                      "paired_runs": report["summary"]["paired_runs"],
                      "errors": report["total_error_count"]}, indent=2))
    return 0 if report["passed"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
