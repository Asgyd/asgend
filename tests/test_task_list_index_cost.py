#!/usr/bin/env python3
"""Correctness smoke for task-list index maintenance benchmark."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
SCRIPT = PROJECT / "scripts" / "benchmark_task_list_index_cost.py"


def require(condition: bool, message: str, details=None) -> None:
    if not condition:
        raise AssertionError(f"{message}: {details!r}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="asgend-task-list-index-cost-test-") as tmp:
        output = Path(tmp) / "index-cost.json"
        completed = subprocess.run(
            [sys.executable, str(SCRIPT), "--sizes", "1000", "--runs", "1",
             "--operations-per-run", "20", "--output", str(output)],
            capture_output=True, text=True, timeout=75, check=False,
        )
        require(completed.returncode == 0, "index-cost benchmark smoke failed",
                {"rc": completed.returncode, "stdout": completed.stdout[-2000:],
                 "stderr": completed.stderr[-3000:]})
        report = json.loads(output.read_text(encoding="utf-8"))
        require(report.get("passed") is True, "benchmark did not pass")
        require(report.get("index_definitions", {}).get("production_changed") is False,
                "benchmark must not imply candidate indexes are applied to production")
        require(report["parameters"]["write_concurrency"] == 1,
                "write benchmark must disclose single-writer measurements")
        results = report["results"].get("1000", {})
        require(set(results) == {"balanced", "queued_heavy"}, "both task-state distributions must be covered", list(results))
        for distribution, scenario in results.items():
            require(scenario["ordered_read_equivalence_passed"] is True,
                    "candidate must preserve ordered query results", distribution)
            require(scenario["ordered_read_equivalence_checks"] == 1, "smoke should have one paired DB check")
            require(scenario["integrity_checks_passed"] is True and scenario["integrity_check_count"] == 2,
                    "both benchmark DBs must pass integrity and row-count invariants", distribution)
            require(all(check["passed"] and check["integrity_check"] == "ok" and
                        check["task_count"] == check["expected_task_count"]
                        for checks in scenario["integrity_checks"].values() for check in checks),
                    "write operations must leave a valid, complete task table", distribution)
            require(scenario["samples_per_operation_per_config"] == 20, "raw sample count must be explicit")
            for config in ("production_indexes", "candidate_partial"):
                ops = scenario["configs"][config]["operations"]
                for operation in ("list_read", "insert_batch", "status_transition", "priority_update", "delete_reinsert"):
                    require(ops[operation]["samples"] == 20, "operation raw sample count mismatch",
                            {"distribution": distribution, "config": config, "operation": operation})
                    require(ops[operation]["p95"] is not None and ops[operation]["p99"] is None,
                            "p95 needs 20 samples while p99 needs 100", operation)
                    require(all(value >= 0 for value in scenario["configs"][config]["raw_samples_ms"][operation]),
                            "latencies must be non-negative", operation)
        print("task-list index cost smoke passed: balanced/queued-heavy, reads, writes, storage, percentile policy")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
