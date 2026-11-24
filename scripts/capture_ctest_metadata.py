#!/usr/bin/env python3
"""Combine CTest's show-only JSON, final log, and exit code into an audit record."""
from __future__ import annotations

import argparse
import json
import math
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

RESULT_RE = re.compile(
    r"^\s*(?P<ordinal>\d+)/(?P<total>\d+) Test\s+#(?P<number>\d+):\s+"
    r"(?P<name>.*?)\s{2,}(?P<result>Passed|Failed|Timeout|Not Run|Skipped)"
    r"(?:\s+(?P<seconds>[0-9]+(?:\.[0-9]+)?)\s+sec)?\s*$"
)
TOTAL_RE = re.compile(r"Total Test time \(real\)\s*=\s*([0-9]+(?:\.[0-9]+)?)\s+sec")
SUMMARY_RE = re.compile(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--show-only", required=True, type=Path)
    p.add_argument("--log", required=True, type=Path)
    p.add_argument("--exit-code", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()

    shown: dict[str, Any] = json.loads(args.show_only.read_text(encoding="utf-8"))
    tests_raw = shown.get("tests", [])
    tests: list[dict[str, Any]] = []
    invalid: list[dict[str, Any]] = []
    for t in tests_raw:
        props = {item.get("name"): item.get("value") for item in t.get("properties", [])}
        timeout = props.get("TIMEOUT")
        valid = isinstance(timeout, (int, float)) and math.isfinite(timeout) and timeout > 0
        if not valid:
            invalid.append({"name": t.get("name"), "timeout": timeout})
        tests.append({
            "name": t.get("name"),
            "timeout_seconds": timeout,
            "timeout_positive_finite": valid,
            "command": t.get("command", []),
        })

    log = args.log.read_text(encoding="utf-8", errors="replace")
    observed: dict[str, dict[str, Any]] = {}
    for line in log.splitlines():
        m = RESULT_RE.match(line)
        if m:
            observed[m.group("name").rstrip(". ")] = {
                "result": m.group("result").lower().replace(" ", "_"),
                "seconds": float(m.group("seconds")) if m.group("seconds") is not None else None,
                "test_number": int(m.group("number")),
            }
    for t in tests:
        t["observed"] = observed.get(t["name"], {"result": "not_observed", "seconds": None})

    total_match = TOTAL_RE.search(log)
    summary_match = SUMMARY_RE.search(log)
    exit_code = int(args.exit_code.read_text(encoding="utf-8").strip())
    counts = {key: sum(t["observed"].get("result") == key for t in tests) for key in ("passed", "failed", "timeout", "not_run", "skipped")}
    all_registered_tests_observed = all(t["observed"].get("result") in ("passed", "failed", "timeout", "not_run", "skipped") for t in tests)
    summary_tests = int(summary_match.group(3)) if summary_match else None
    valid = (
        len(tests) > 0
        and not invalid
        and len(observed) == len(tests)
        and all_registered_tests_observed
        and summary_tests == len(tests)
        and exit_code == 0
        and counts["passed"] == len(tests)
        and counts["failed"] == 0
        and counts["timeout"] == 0
        and counts["not_run"] == 0
        and counts["skipped"] == 0
        and total_match is not None
    )
    timeouts = [t["timeout_seconds"] for t in tests if isinstance(t["timeout_seconds"], (int, float))]
    result = {
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "build_type": "Release",
        "ctest_command": "ctest --test-dir /mnt/data/asgend-stage31-build --output-on-failure --timeout 300",
        "exit_code": exit_code,
        "status": "PASS" if valid else "FAIL",
        "observed_final_run": {
            "tests": len(tests),
            "passed": counts["passed"],
            "failed": counts["failed"],
            "timeout": counts["timeout"],
            "not_run": counts["not_run"],
            "skipped": counts["skipped"],
            "total_seconds": float(total_match.group(1)) if total_match else None,
            "exit_code": exit_code,
            "summary_line": summary_match.group(0) if summary_match else None,
        },
        "test_count": len(tests),
        "timeout_range_seconds": [min(timeouts), max(timeouts)] if timeouts else None,
        "all_timeouts_positive_finite": len(tests) > 0 and not invalid,
        "all_registered_tests_observed": all_registered_tests_observed,
        "invalid_timeouts": invalid,
        "tests": tests,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"status": result["status"], "tests": len(tests), **result["observed_final_run"], "timeout_range_seconds": result["timeout_range_seconds"]}, indent=2))
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
