#!/usr/bin/env python3
"""Smoke gate for the instrumented production and test-only candidate C APIs."""
from __future__ import annotations
import json
import subprocess
import sys
import tempfile
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
sys.path.insert(0, str(PROJECT / "scripts"))
from benchmark_task_list_c_api import build_database  # noqa: E402
ANCHOR = "2026-10-28T00:00:00.000000Z"

def require(ok: bool, message: str, details=None) -> None:
    if not ok:
        raise AssertionError(f"{message}: {details!r}")

def run(binary: Path, db_path: Path, root: Path, mode: str) -> dict:
    command = [str(binary), str(db_path), str(root), "300", "200", "5", "2", mode]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=30, check=False)
    require(completed.returncode == 0, f"{mode} C API harness failed", {
        "rc": completed.returncode, "stdout": completed.stdout[-1000:], "stderr": completed.stderr[-2000:]})
    report = json.loads(completed.stdout)
    require(report.get("passed") is True and report.get("error_count") == 0,
            f"{mode} report did not pass", {k: report.get(k) for k in ("passed", "error_count", "sample_count")})
    require(report.get("sample_count") == 10, "expected concurrency x sample count", report.get("sample_count"))
    for sample in report["samples"]:
        for key in ("api_ns", "mutex_wait_ns", "prepare_ns", "sqlite_step_ns", "json_mapping_ns", "serialization_ns"):
            require(isinstance(sample.get(key), int) and sample[key] >= 0, "timing field invalid", (key, sample.get(key)))
        require(sample.get("rows") == 200, "API limit/output rows incorrect", sample.get("rows"))
    require(report.get("initial_call_is_os_cold_cache") is False, "must not mislabel process-first as OS-cold")
    return report

def main() -> int:
    require(BINARY.is_file(), "benchmark binary missing", str(BINARY))
    with tempfile.TemporaryDirectory(prefix="asgend-task-list-c-api-smoke-") as raw:
        work = Path(raw)
        for distribution, expected_running in (("fixture_10pct_running", None), ("low_running_4", 4)):
            reports = {}
            for mode in ("baseline", "candidate"):
                db_path = work / f"{distribution}-{mode}.sqlite"
                info = build_database(db_path, work / f"root-{distribution}-{mode}",
                                      1000, mode == "candidate", distribution)
                if expected_running is not None:
                    require(info["running_task_count"] == expected_running,
                            "low-running fixture must force the expected active count", info["running_task_count"])
                reports[mode] = run(BINARY, db_path, work / f"{distribution}-{mode}", mode)
            require(reports["baseline"]["first_result"] == reports["candidate"]["first_result"],
                    "C production and candidate API JSON rows differ", distribution)
            require(reports["baseline"]["samples"][0]["serialized_bytes"] ==
                    reports["candidate"]["samples"][0]["serialized_bytes"],
                    "serialization sizes differ for equivalent rows", distribution)
    print("task-list C API benchmark smoke passed: baseline/candidate API, normal and low-running distributions, internal stage timings")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
