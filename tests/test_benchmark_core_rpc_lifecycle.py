#!/usr/bin/env python3
"""Bounded end-to-end smoke test for the Core RPC lifecycle benchmark."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
SCRIPT = PROJECT / "scripts" / "benchmark_core_rpc_lifecycle.py"


def require(condition: bool, message: str, value=None) -> None:
    if not condition:
        raise AssertionError(f"{message}: {value!r}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="asgend-rpc-perf-smoke-") as tmp:
        output = Path(tmp) / "benchmark.json"
        command = [sys.executable, str(SCRIPT), str(BINARY), str(PROJECT),
                   "--runs", "1", "--samples-per-run", "2", "--concurrencies", "1",
                   "--terminal-timeout", "12", "--output", str(output)]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=90, check=False)
        require(completed.returncode == 0, "Core RPC benchmark smoke must pass",
                {"returncode": completed.returncode, "stdout": completed.stdout[-2000:],
                 "stderr": completed.stderr[-4000:]})
        require(output.is_file(), "benchmark must write a JSON report", output)
        report = json.loads(output.read_text(encoding="utf-8"))
        require(report.get("passed") is True, "benchmark report must pass", report.get("summary"))
        require(report.get("summary", {}).get("error_count") == 0, "benchmark must report zero RPC errors", report.get("summary"))
        methods = report.get("results", {}).get("1", {})
        expected = {"ping", "submit", "status", "list_tasks", "events", "answer", "cancel", "retry_task", "submit_to_terminal"}
        require(set(methods) == expected, "benchmark must measure the complete Core RPC lifecycle", sorted(methods))
        for method, detail in methods.items():
            require(detail.get("samples") == 2, f"{method} should retain exactly two samples", detail)
            require(detail.get("latency_ms", {}).get("p95") is None and
                    detail.get("latency_ms", {}).get("p99") is None,
                    f"{method} must not report tail percentiles for a tiny sample", detail)
            require(detail.get("error_rate") == 0, f"{method} should have zero errors", detail)
        raw = report.get("raw_samples", [])
        require(len(raw) == 18, "raw sample list should contain nine methods x two samples", len(raw))
        # Reports should not retain RPC payloads/replies or auth material.
        serialized = output.read_text(encoding="utf-8")
        require("core-rpc-performance-benchmark-local-token" not in serialized, "report must not contain auth token")
        require("replies" not in serialized, "report must not retain per-request response bodies")
        print("Core RPC benchmark smoke passed: 9 methods, bounded lifecycle fixtures, tiny-sample percentile policy")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
