#!/usr/bin/env python3
"""Bounded smoke/regression test for the SQLite + Core artifact profiler."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
SCRIPT = PROJECT / "scripts" / "benchmark_sqlite_artifacts.py"


def require(condition: bool, message: str, detail=None) -> None:
    if not condition:
        raise AssertionError(f"{message}: {detail!r}")


def main() -> int:
    source = (PROJECT / "src" / "db.c").read_text(encoding="utf-8")
    for marker in ("idx_tasks_status_created", "idx_tasks_queue_priority", "idx_session_messages",
                   "idx_task_events_task", "SELECT event_id,event_type,actor,message,payload_json,created_at FROM task_events"):
        require(marker in source, "benchmark mirrored schema/query drifted from src/db.c", marker)
    with tempfile.TemporaryDirectory(prefix="asgend-sqlite-artifacts-smoke-") as temporary:
        output = Path(temporary) / "profile.json"
        command = [sys.executable, str(SCRIPT), str(BINARY), str(PROJECT),
                   "--db-sizes", "1000", "--runs", "1", "--operations-per-worker", "2",
                   "--concurrencies", "1", "--artifact-sizes", "10,1001", "--artifact-repeats", "1",
                   "--output", str(output)]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=90, check=False)
        require(completed.returncode == 0, "SQLite/artifact benchmark smoke must pass",
                {"returncode": completed.returncode, "stdout": completed.stdout[-3000:],
                 "stderr": completed.stderr[-4000:]})
        require(output.is_file(), "benchmark JSON must exist")
        report = json.loads(output.read_text(encoding="utf-8"))
        require(report.get("passed") is True, "benchmark JSON must say passed")
        require(report.get("sqlite", {}).get("summary", {}).get("sqlite_error_count") == 0,
                "SQLite operations should have no errors", report.get("sqlite", {}).get("summary"))
        require(report.get("sqlite", {}).get("summary", {}).get("raw_sample_count") == 4,
                "smoke should retain all four read/write workload samples",
                report.get("sqlite", {}).get("summary"))
        db_result = report["sqlite"]["results"]["1000"]
        require(db_result["database_file_bytes"] > 4096, "SQLite file size should be measured after WAL checkpoint",
                db_result["database_file_bytes"])
        require(db_result["correctness"]["integrity_check"] == "ok", "SQLite integrity check failed")
        require(db_result["correctness"]["atomicity"]["passed"] is True, "rollback injection did not pass")
        assertions = db_result["query_plans"]["assertions"]
        require(all(assertions.values()), "required query-plan index assertions failed", assertions)
        require(db_result["query_plans"]["observations"]["all_task_list_uses_temp_sort"] is True,
                "task-list temporary-sort risk must be recorded")
        artifact_result = report.get("artifacts", {})
        require(artifact_result.get("summary", {}).get("artifact_list_errors") == 0,
                "artifact listing RPC failed", artifact_result.get("summary"))
        require(artifact_result.get("summary", {}).get("workspace_read_errors") == 0,
                "workspace read RPC failed", artifact_result.get("summary"))
        require(artifact_result["workspace_files"]["10"]["1"]["visible_entries"] == 10,
                "small artifact listing was incorrect")
        require(artifact_result["workspace_files"]["1001"]["1"]["visible_entries"] == 1000 and
                artifact_result["workspace_files"]["1001"]["1"]["truncated"] is True,
                "artifact truncation limit was not exercised")
        serialized = output.read_text(encoding="utf-8")
        require("local-sqlite-artifact-performance-token" not in serialized,
                "benchmark report must not contain the local RPC token")
        require("artifact benchmark workspace bootstrap" not in serialized,
                "benchmark report must not contain task text")
        print("SQLite/artifacts benchmark smoke passed: query plans, rollback, DB integrity, artifact cap, binary reads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
