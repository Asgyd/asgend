#!/usr/bin/env python3
"""Collect a deterministic source/test/workflow inventory for ASGEND performance reviews.

This is structural evidence, not a profiler: it does not claim latency, throughput,
CPU, memory, or production capacity. Runtime measurements must be recorded separately.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any

SOURCE_SUFFIXES = {
    ".c", ".h", ".cc", ".cpp", ".cxx", ".java", ".py", ".kt", ".kts",
    ".dart", ".js", ".ts", ".html", ".css",
}

# These files affect build configuration, runtime behavior, platform packaging, or
# CI, but are intentionally reported separately from executable source LOC. Keeping
# them separate prevents manifests/fixtures from inflating the source-code metric.
CONFIG_SUFFIXES = {
    ".xml", ".gradle", ".properties", ".yml", ".yaml", ".cmake", ".sh",
    ".sql", ".toml", ".ini", ".conf",
}
CONFIG_SPECIAL_NAMES = {"CMakeLists.txt", ".dockerignore", ".gitignore"}
STRUCTURED_DATA_SUFFIXES = {".json"}
AUDIT_REPORT_PREFIXES = ("PERFORMANCE_", "PROJECT_AUDIT_", "CI_TEST_AUDIT_")
EXCLUDED_DIRS = {
    ".git", "build", "build-ci-audit", "build-qt", "build-core-ci", "__pycache__",
    ".dart_tool", ".gradle", ".venv", "venv", "node_modules", "target", "out",
    "cmake-build-debug", "cmake-build-release",
}


def component_for(rel: Path) -> str:
    p = rel.as_posix()
    # Classify shared TUI interfaces before the broad src/include rules below.
    if p in {"clients/asgend_cli.c", "src/asgend_tui.c", "include/asgend_tui.h"}:
        return "cli_tui"
    if p.startswith("src/memory/") or p == "clients/asgend-memory.c":
        return "memory_index_retrieval"
    if p.startswith("src/") or p.startswith("include/"):
        return "core_runtime_rpc"
    if p.startswith("server/asgend_cp/") or p.startswith("server/tests/"):
        return "control_plane_and_server_tests"
    if p.startswith("node_agent/"):
        return "node_agent"
    if p.startswith("clients/asgend_android/"):
        return "native_android"
    if p.startswith("clients/asgend_flutter/"):
        return "flutter_client"
    if p.startswith("clients/asgend_qt/"):
        return "qt_client"
    if p.startswith("clients/asgend_cli/"):
        return "cli_tui"
    if p.startswith("ui/"):
        return "web_ui"
    if (p == "CMakeLists.txt" or p.startswith("cmake/") or p.startswith("scripts/") or
            p.startswith(".github/workflows/") or p.startswith("packaging/")):
        return "build_ci_packaging"
    if p.startswith("config/"):
        return "runtime_configuration"
    if p.startswith("tests/") or p.startswith("contracts/"):
        return "tests_and_contracts"
    return "other"


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def collect(root: Path) -> dict[str, Any]:
    if not (root / "CMakeLists.txt").is_file():
        raise ValueError(f"{root} does not look like an ASGEND root (CMakeLists.txt missing)")
    source_files: list[tuple[Path, int, int]] = []
    component: dict[str, dict[str, int]] = {}
    config_by_extension: dict[str, dict[str, int]] = {}
    config_by_component: dict[str, dict[str, int]] = {}
    config_files_total = 0
    config_lines_total = 0
    data_by_extension: dict[str, dict[str, int]] = {}
    data_by_component: dict[str, dict[str, int]] = {}
    data_files_total = 0
    data_lines_total = 0
    for path in sorted(root.rglob("*")):
        if not path.is_file() or any(part in EXCLUDED_DIRS for part in path.parts):
            continue
        rel = path.relative_to(root)
        # Avoid counting generated/vendor files as first-party source.
        if any(part in {"vendor", "third_party", "generated"} for part in rel.parts):
            continue
        suffix = path.suffix.lower()
        # Machine-readable audit outputs live in docs/ and must not feed back
        # into the project/config inventory, otherwise each rerun counts prior
        # audit reports (including the previous inventory snapshot) as project data.
        is_audit_report_json = (
            len(rel.parts) > 1 and rel.parts[0] == "docs" and suffix == ".json"
            and path.name.startswith(AUDIT_REPORT_PREFIXES)
        )
        if is_audit_report_json:
            continue
        is_gradle_kotlin_dsl = path.name.endswith(".gradle.kts")
        is_source = suffix in SOURCE_SUFFIXES and not is_gradle_kotlin_dsl
        is_config = suffix in CONFIG_SUFFIXES or path.name in CONFIG_SPECIAL_NAMES or is_gradle_kotlin_dsl
        is_structured_data = suffix in STRUCTURED_DATA_SUFFIXES
        if not is_source and not is_config and not is_structured_data:
            continue
        text = read_text(path)
        line_count = len(text.splitlines())
        bucket = component_for(rel)
        if is_source:
            item = component.setdefault(bucket, {"files": 0, "lines": 0})
            item["files"] += 1
            item["lines"] += line_count
            source_files.append((rel, line_count, path.stat().st_size))
        if is_config:
            config_files_total += 1
            config_lines_total += line_count
            ext_key = (f"<{path.name}>" if path.name in CONFIG_SPECIAL_NAMES else
                       ".gradle.kts" if is_gradle_kotlin_dsl else suffix)
            ext_item = config_by_extension.setdefault(ext_key, {"files": 0, "lines": 0})
            ext_item["files"] += 1
            ext_item["lines"] += line_count
            component_item = config_by_component.setdefault(bucket, {"files": 0, "lines": 0})
            component_item["files"] += 1
            component_item["lines"] += line_count
        if is_structured_data:
            data_files_total += 1
            data_lines_total += line_count
            ext_item = data_by_extension.setdefault(suffix, {"files": 0, "lines": 0})
            ext_item["files"] += 1
            ext_item["lines"] += line_count
            component_item = data_by_component.setdefault(bucket, {"files": 0, "lines": 0})
            component_item["files"] += 1
            component_item["lines"] += line_count

    cmake = read_text(root / "CMakeLists.txt")
    tests = re.findall(r"add_test\s*\(\s*NAME\s+([^\s)]+)", cmake)
    test_timeouts: dict[str, int] = {}
    for match in re.finditer(
        r'set_tests_properties\(\s*"?([^"\s)]+)"?\s+PROPERTIES\s+TIMEOUT\s+(\d+)', cmake
    ):
        # A loop variable is not a literal test ID. The default timeout is
        # represented separately below and expanded over registered tests.
        if not match.group(1).startswith("${"):
            test_timeouts[match.group(1)] = int(match.group(2))
    default_timeout_match = re.search(
        r'set_tests_properties\(\s*"?\$\{ASGEND_TEST_NAME\}"?\s+PROPERTIES\s+TIMEOUT\s+(\d+)', cmake)
    default_timeout = int(default_timeout_match.group(1)) if default_timeout_match else None
    effective_timeouts = {
        name: test_timeouts.get(name, default_timeout)
        for name in tests
    }

    workflows: dict[str, dict[str, Any]] = {}
    workflow_dir = root / ".github" / "workflows"
    if workflow_dir.is_dir():
        for path in sorted(workflow_dir.glob("*.y*ml")):
            text = read_text(path)
            # Count job IDs only under the top-level `jobs:` section. Counting all
            # two-space mapping keys accidentally treats `push`, `permissions`, etc.
            # as jobs and creates a misleading inventory metric.
            in_jobs = False
            job_count = 0
            for line in text.splitlines():
                if re.match(r"^jobs:\s*$", line):
                    in_jobs = True
                    continue
                if in_jobs and line and not line[0].isspace():
                    break
                if in_jobs and re.match(r"^  [A-Za-z0-9_-]+:\s*$", line):
                    job_count += 1
            workflows[path.name] = {
                "job_count": job_count,
                "timeouts_minutes": [int(x) for x in re.findall(r"timeout-minutes:\s*(\d+)", text)],
                "has_ctest": "ctest " in text,
                "has_flutter_test": "flutter test" in text,
                "has_gradle_test": bool(re.search(r"gradle .*test(Debug|Release)UnitTest", text)),
                "has_qt_ctest": "QT_QPA_PLATFORM" in text and "ctest " in text,
            }

    hot_files = sorted(
        ({"path": rel.as_posix(), "lines": lines, "bytes": size} for rel, lines, size in source_files),
        key=lambda x: (x["lines"], x["bytes"], x["path"]), reverse=True
    )[:20]
    return {
        "schema_version": 2,
        "scope": "structural inventory only; not a performance profiler",
        "source_extensions_included": sorted(SOURCE_SUFFIXES),
        "source_files_total": len(source_files),
        "source_lines_total": sum(lines for _, lines, _ in source_files),
        "components": dict(sorted(component.items())),
        "configuration_inventory": {
            "scope": "build/runtime/platform/CI manifests and scripts; reported separately from source LOC and structured data",
            "files_total": config_files_total,
            "lines_total": config_lines_total,
            "by_extension": dict(sorted(config_by_extension.items())),
            "by_component": dict(sorted(config_by_component.items())),
        },
        "structured_data_inventory": {
            "scope": "JSON fixtures, manifests, reports, and other structured data; not counted as source LOC or config",
            "files_total": data_files_total,
            "lines_total": data_lines_total,
            "by_extension": dict(sorted(data_by_extension.items())),
            "by_component": dict(sorted(data_by_component.items())),
        },
        "largest_source_files": hot_files,
        "ctest": {
            "registered_tests": len(tests),
            "test_names": tests,
            "explicit_timeouts": test_timeouts,
            "default_timeout_seconds": default_timeout,
            "effective_timeouts_seconds": effective_timeouts,
            "tests_without_explicit_timeout": sorted(set(tests) - set(test_timeouts)),
            "tests_without_effective_timeout": sorted(name for name, timeout in effective_timeouts.items() if timeout is None),
        },
        "workflows": workflows,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, help="write JSON to this path instead of stdout")
    args = parser.parse_args()
    try:
        result = collect(args.root.resolve())
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    output = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    else:
        print(output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
