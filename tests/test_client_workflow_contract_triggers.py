#!/usr/bin/env python3
"""Guard cross-layer CI path filters so API changes run every client workflow."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = {
    ".github/workflows/native-android.yml": {
        "label": "native Android",
        "own_paths": ("clients/asgend_android/**", "docs/ANDROID_NATIVE_*.md", ".github/workflows/native-android.yml"),
    },
    ".github/workflows/flutter-android.yml": {
        "label": "Flutter Android",
        "own_paths": ("clients/asgend_flutter/**", ".github/workflows/flutter-android.yml"),
    },
    ".github/workflows/qt-client.yml": {
        "label": "Qt",
        "own_paths": ("clients/asgend_qt/**", "packaging/linux/**", ".github/workflows/qt-client.yml"),
    },
}
REQUIRED_RPC_TESTS = (
    "core_rpc_contract_inventory",
    "core_rpc_runtime_contract",
    "core_rpc_state_transitions",
)
REQUIRED_RPC_BUILD_MARKERS = (
    "cmake -S . -B build-core-ci -DCMAKE_BUILD_TYPE=Release",
    "cmake --build build-core-ci --target agent-orchestrator --parallel 2",
)
REQUIRED_PATHS = (
    "src/**",
    "include/**",
    "server/**",
    "contracts/**",
    "tests/contracts/**",
    "tests/test_core_rpc_contract_inventory.py",
    "tests/test_core_rpc_runtime_contract.py",
    "tests/test_core_rpc_state_transitions.py",
    "docs/API_CONTRACT.md",
    "docs/PROTOCOL.md",
    "docs/REMOTE_CONTROL_PLANE.md",
    "CMakeLists.txt",
)


def workflow_path_filters(text: str, trigger: str) -> set[str]:
    """Read only the paths list for a top-level push/pull_request trigger."""
    match = re.search(
        rf"(?ms)^  {re.escape(trigger)}:\n(.*?)(?=^  [A-Za-z_][A-Za-z0-9_-]*:|\Z)",
        text,
    )
    if not match:
        raise AssertionError(f"missing top-level {trigger} trigger")
    block = match.group(1)
    paths_match = re.search(r"(?ms)^    paths:\n(.*?)(?=^    [A-Za-z_][A-Za-z0-9_-]*:|\Z)", block)
    if not paths_match:
        raise AssertionError(f"{trigger} trigger is not path filtered")
    return {
        item.strip().strip("'\"")
        for item in re.findall(r"(?m)^      - (.+?)\s*$", paths_match.group(1))
    }


def workflow_rpc_execution_issues(text: str) -> list[str]:
    """Require client workflows to build Core and execute every shared RPC test.

    Path-filter coverage is insufficient by itself: a workflow could trigger for a
    changed test file but never run that test. Keep this guard dependency-free so
    the root CTest suite can execute it before optional Python packages are installed.
    """
    issues: list[str] = []
    active_lines = [line.strip() for line in text.splitlines()
                    if line.strip() and not line.lstrip().startswith("#")]
    active_text = "\n".join(active_lines)
    for marker in REQUIRED_RPC_BUILD_MARKERS:
        if marker not in active_text:
            issues.append(f"missing Core RPC build command: {marker}")
    ctest_lines = [line for line in active_lines
                   if line.startswith(("run: ctest --test-dir build-core-ci", "ctest --test-dir build-core-ci"))]
    if not any(all(test in line for test in REQUIRED_RPC_TESTS) for line in ctest_lines):
        issues.append("missing CTest command that runs all shared RPC tests: " + ", ".join(REQUIRED_RPC_TESTS))
    return issues


def main() -> int:
    failures: list[str] = []
    for rel, spec in WORKFLOWS.items():
        label = spec["label"]
        path = ROOT / rel
        if not path.is_file():
            failures.append(f"{label}: workflow missing: {rel}")
            continue
        text = path.read_text(encoding="utf-8")
        for issue in workflow_rpc_execution_issues(text):
            failures.append(f"{label}/execution: {issue}")
        required_paths = (*REQUIRED_PATHS, *spec["own_paths"])
        for trigger in ("push", "pull_request"):
            try:
                filters = workflow_path_filters(text, trigger)
            except AssertionError as exc:
                failures.append(f"{label}/{trigger}: {exc}")
                continue
            for required in required_paths:
                if required not in filters:
                    failures.append(f"{label}/{trigger}: required path missing: {required}")
    if failures:
        print("client workflow contract trigger coverage FAILED:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1
    required_total = sum(len(REQUIRED_PATHS) + len(spec["own_paths"]) for spec in WORKFLOWS.values())
    execution_assertions = len(WORKFLOWS) * (len(REQUIRED_RPC_BUILD_MARKERS) + 1)
    print(f"client workflow contract and RPC execution coverage passed ({len(WORKFLOWS)} workflows x 2 triggers; "
          f"{required_total * 2} required path assertions and {execution_assertions} build/test command assertions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
