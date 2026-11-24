#!/usr/bin/env python3
"""Fast correctness gate for the isolated task-list SQL A/B prototype."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
PROJECT = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).resolve().parents[1]
SCRIPT = PROJECT / "scripts" / "benchmark_task_list_ab.py"


def require(condition: bool, message: str, detail=None) -> None:
    if not condition:
        raise AssertionError(f"{message}: {detail!r}")


def main() -> int:
    if BINARY is not None:
        require(BINARY.is_file(), "Core binary must exist for test environment", str(BINARY))
    source = (PROJECT / "src" / "db.c").read_text(encoding="utf-8")
    for marker in ("idx_tasks_status_created", "idx_tasks_queue_priority",
                   "CASE WHEN status='queued' THEN MIN(9,priority+CAST(MAX(0,(julianday('now')-julianday(created_at))*86400/?1) AS INTEGER)) ELSE priority END",
                   "ORDER BY CASE status WHEN 'running' THEN 0 WHEN 'queued' THEN 1 ELSE 2 END",
                   "created_at ASC, id ASC LIMIT ?2", "created_at ASC, id ASC LIMIT ?3"):
        require(marker in source, "benchmark reference drifted from production source", marker)
    with tempfile.TemporaryDirectory(prefix="asgend-task-list-ab-smoke-") as tmp:
        output = Path(tmp) / "ab.json"
        completed = subprocess.run([
            sys.executable, str(SCRIPT), "--sizes", "1000", "--runs", "1",
            "--operations-per-worker", "2", "--concurrencies", "1,2",
            "--limit", "50", "--reference-binary", str(BINARY), "--output", str(output),
        ], capture_output=True, text=True, timeout=60, check=False)
        require(completed.returncode == 0, "A/B SQL correctness + smoke benchmark must pass",
                {"rc": completed.returncode, "stdout": completed.stdout[-2000:],
                 "stderr": completed.stderr[-3000:]})
        report = json.loads(output.read_text(encoding="utf-8"))
        require(report.get("passed") is True, "benchmark report did not pass")
        result = report["database_results"]["1000"]
        correctness = result["correctness"]
        require(correctness["passed"] is True and correctness["checks"] == 32,
                "all statuses x limit correctness matrix must pass", correctness)
        require(correctness["includes_ties"] is True and correctness["includes_priority_aging"] is True,
                "fixture must cover ties and aging", correctness)
        c_reference = result["c_reference_validation"]
        require(c_reference["passed"] is True and c_reference["checks"] == 32 and
                c_reference["rows_compared_to_c"] > 0,
                "candidate and mirror must match the real C db_list_tasks() API", c_reference)
        observations = result["query_plans"]["observations"]
        require(observations["baseline_all_uses_temp_sort"] is True,
                "baseline full-list query should expose known temp-sort risk", observations)
        require(observations["candidate_running_index_used"] is True and
                observations["candidate_other_index_used"] is True,
                "candidate status bands should use isolated partial indexes", observations)
        for concurrency in ("1", "2"):
            aggregate = result["results"][concurrency]["aggregate"]
            require(aggregate["baseline_error_count"] == 0 and aggregate["candidate_error_count"] == 0,
                    "benchmark SQL must return without errors", aggregate)
        require(report["correctness_policy"]["direct_c_db_list_tasks_oracle"] is True,
                "benchmark must validate against the C implementation")
        policy = report["correctness_policy"]
        require(policy["production_sql_modified_for_deterministic_tie_break"] is True,
                "report must disclose the deterministic id tie-break added to production")
        require(policy["candidate_partial_indexes_applied_to_production"] is False,
                "experimental partial indexes must remain isolated from production")
        print("task-list SQL A/B smoke passed: all statuses, aging, ties, LIMIT and query plans")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
