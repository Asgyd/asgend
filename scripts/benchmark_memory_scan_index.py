#!/usr/bin/env python3
"""Reproducible real-C ASGEND memory scan/index performance audit.

Runs 2 corpus sizes x 3 deterministic content profiles x 3 scan scenarios x 5 repeats
(90 timed scanner invocations). It measures elapsed time and Linux VmHWM via the
process runner; MiB/s is a *logical corpus-byte throughput ratio*, not physical I/O.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from statistics import median
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(SCRIPT_DIR))
from memory_scan_runner import run_command  # noqa: E402

SIZES = (1000, 10000)
PROFILES = ("small_text", "markdown", "mixed_size")
SCENARIOS = ("new_index", "no_op", "single_file_change")
REPEATS = 5
SCAN_LINE = re.compile(
    r"scan_id=(?P<scan_id>\d+)\s+status=(?P<status>\S+)\s+files=(?P<files>\d+)\s+"
    r"added=(?P<added>\d+)\s+modified=(?P<modified>\d+)\s+deleted=(?P<deleted>\d+)"
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def deterministic_payload(profile: str, index: int) -> bytes:
    if profile == "small_text":
        target_size = 512
        base = (f"ASGEND scan benchmark fixture\nprofile=small_text\nid={index:05d}\n"
                f"topic=memory-index\nstatus=stable\nrecord={index % 97}\n").encode()
    elif profile == "markdown":
        target_size = 2048
        base = ("---\n" f"fixture_id: {index:05d}\n" "kind: performance-audit\n"
                "tags: [memory, scan, markdown]\n" "---\n\n"
                f"# Benchmark Fixture {index:05d}\n\n"
                "This deterministic Markdown fixture is used to evaluate filesystem scanning and index bookkeeping.\n\n"
                "## Repeated context\n" f"Record {index} describes a stable document for a local index benchmark.\n").encode()
    elif profile == "mixed_size":
        if index % 10 == 0:
            target_size = 16384
        elif index % 5 == 0:
            target_size = 4096
        elif index % 2 == 0:
            target_size = 1024
        else:
            target_size = 256
        base = (f"ASGEND mixed-size fixture\nid={index:05d}\n".encode() +
                (b"memory-scan-index performance record; deterministic payload.\n" * 600))
    else:
        raise ValueError(f"unknown profile: {profile}")
    # Include the profile/id prefix and pad deterministically to a fixed target size.
    if len(base) >= target_size:
        return base[:target_size]
    filler = (f"payload={index:05d}; profile={profile}; stable benchmark fixture\n".encode())
    repeated = (filler * ((target_size - len(base)) // len(filler) + 1))[:target_size - len(base)]
    return base + repeated


def generate_corpus(root: Path, count: int, profile: str) -> tuple[list[dict[str, Any]], int]:
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    fixture_hashes: list[dict[str, Any]] = []
    total_bytes = 0
    for index in range(count):
        suffix = ".md" if profile == "markdown" else ".txt"
        rel = Path(f"doc_{index:05d}{suffix}")
        data = deterministic_payload(profile, index)
        dest = root / rel
        dest.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        fixture_hashes.append({"path": str(rel), "size_bytes": len(data), "sha256": digest})
        total_bytes += len(data)
    return fixture_hashes, total_bytes


def reset_index(root: Path) -> None:
    metadata = root / ".asgend_mem"
    if metadata.exists():
        shutil.rmtree(metadata)


def invoke_scan(binary: Path, root: Path, timeout_seconds: float) -> dict[str, Any]:
    result = run_command([str(binary), "scan", str(root)], timeout_seconds=timeout_seconds)
    if result["timed_out"]:
        raise RuntimeError(f"scanner timed out after {timeout_seconds}s for {root}: {result['stderr']}")
    if result["returncode"] != 0:
        raise RuntimeError(f"scanner failed rc={result['returncode']} for {root}: {result['stderr']} {result['stdout']}")
    match = SCAN_LINE.search(result["stdout"])
    if not match:
        raise RuntimeError(f"cannot parse scanner result for {root}: {result['stdout']!r}")
    parsed = {key: (value if key == "status" else int(value)) for key, value in match.groupdict().items()}
    if parsed["status"] != "complete":
        raise RuntimeError(f"incomplete scan for {root}: {parsed}")
    if result["peak_rss_kib"] is None:
        raise RuntimeError(f"Linux VmHWM was not observed for scanner PID {result['pid']} ({root})")
    return {**result, **parsed}


def run_benchmark(binary: Path, fixture_root: Path, timeout_seconds: float) -> dict[str, Any]:
    all_fixture_hashes: list[dict[str, Any]] = []
    corpus_meta: dict[tuple[int, str], dict[str, Any]] = {}
    samples: list[dict[str, Any]] = []

    for size in SIZES:
        for profile in PROFILES:
            corpus = fixture_root / f"n{size}" / profile
            hashes, logical_bytes = generate_corpus(corpus, size, profile)
            all_fixture_hashes.extend(
                {"corpus": f"n{size}/{profile}", **record} for record in hashes
            )
            corpus_meta[(size, profile)] = {
                "root": corpus,
                "logical_bytes": logical_bytes,
                "logical_mib": logical_bytes / (1024 * 1024),
            }

    for size in SIZES:
        for profile in PROFILES:
            meta = corpus_meta[(size, profile)]
            corpus = meta["root"]
            for scenario in SCENARIOS:
                for repeat in range(1, REPEATS + 1):
                    reset_index(corpus)
                    changed_path = None
                    setup_scan = None
                    changed_file = corpus / ("doc_00000.md" if profile == "markdown" else "doc_00000.txt")
                    if scenario == "single_file_change":
                        # Restore the exact original fixture before each repeat so the
                        # one-file delta and corpus byte count stay constant.
                        changed_file.write_bytes(deterministic_payload(profile, 0))
                        original_ns = time.time_ns()
                        os.utime(changed_file, ns=(original_ns, original_ns))
                    if scenario != "new_index":
                        setup_scan = invoke_scan(binary, corpus, timeout_seconds)
                    if scenario == "single_file_change":
                        updated = changed_file.read_bytes() + b"\nASGEND single-file mutation for stage31 benchmark.\n"
                        changed_file.write_bytes(updated)
                        # Force a timestamp newer than the setup scan on coarse/virtual filesystems.
                        future_ns = time.time_ns() + 2_000_000_000
                        os.utime(changed_file, ns=(future_ns, future_ns))
                        changed_path = changed_file.name

                    measured = invoke_scan(binary, corpus, timeout_seconds)
                    elapsed = measured["wall_seconds"]
                    files_seen = measured["files"]
                    sample = {
                        "size": size,
                        "profile": profile,
                        "scenario": scenario,
                        "repeat": repeat,
                        "wall_seconds": elapsed,
                        "files_seen": files_seen,
                        "files_per_second": files_seen / elapsed if elapsed > 0 else None,
                        "logical_corpus_bytes": meta["logical_bytes"],
                        "logical_mib_per_second": meta["logical_mib"] / elapsed if elapsed > 0 else None,
                        "peak_rss_kib": measured["peak_rss_kib"],
                        "peak_rss_source": measured["rss_source"],
                        "returncode": measured["returncode"],
                        "timed_out": measured["timed_out"],
                        "pipes_closed": measured["pipes_closed"],
                        "last_scan_id": measured["scan_id"],
                        "last_scan_files_seen": measured["files"],
                        "added": measured["added"],
                        "modified": measured["modified"],
                        "deleted": measured["deleted"],
                        "changed_file": changed_path,
                        "setup_scan_id": setup_scan["scan_id"] if setup_scan else None,
                    }
                    # Guard against the historical scan_run_count metadata bug: IDs and
                    # per-run files_seen are sourced from the scanner's actual last scan.
                    if sample["last_scan_id"] <= 0 or sample["last_scan_files_seen"] != size:
                        raise RuntimeError(f"invalid final scan metadata: {sample}")
                    if scenario == "new_index" and sample["last_scan_id"] != 1:
                        raise RuntimeError(f"new index should report last_scan_id=1: {sample}")
                    if scenario != "new_index" and sample["last_scan_id"] != 2:
                        raise RuntimeError(f"reset + setup + measured scan should report last_scan_id=2: {sample}")
                    if scenario == "single_file_change" and sample["modified"] != 1:
                        raise RuntimeError(f"one-file mutation must report modified=1: {sample}")
                    samples.append(sample)
                    print(f"[stage31] {len(samples):02d}/90 n={size} profile={profile} scenario={scenario} repeat={repeat} wall={elapsed:.3f}s rss={sample['peak_rss_kib']}KiB", flush=True)

    summaries = []
    for size in SIZES:
        for profile in PROFILES:
            for scenario in SCENARIOS:
                group = [s for s in samples if s["size"] == size and s["profile"] == profile and s["scenario"] == scenario]
                summaries.append({
                    "size": size,
                    "profile": profile,
                    "scenario": scenario,
                    "repeat_count": len(group),
                    "median_wall_seconds": median(s["wall_seconds"] for s in group),
                    "min_wall_seconds": min(s["wall_seconds"] for s in group),
                    "max_wall_seconds": max(s["wall_seconds"] for s in group),
                    "median_files_per_second": median(s["files_per_second"] for s in group),
                    "median_logical_mib_per_second": median(s["logical_mib_per_second"] for s in group),
                    "median_peak_rss_kib": median(s["peak_rss_kib"] for s in group),
                    "all_pipes_closed": all(s["pipes_closed"] for s in group),
                    "all_return_codes_zero": all(s["returncode"] == 0 for s in group),
                })

    return {
        "schema_version": 1,
        "stage": 31,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "benchmark": "ASGEND real C memory scan/index performance audit",
        "binary_path_at_run": str(binary),
        "binary_sha256": sha256_file(binary),
        "harness_sha256": sha256_file(Path(__file__).resolve()),
        "helper_sha256": sha256_file(Path(__file__).with_name("memory_scan_runner.py")),
        "environment": {
            "platform": platform.platform(),
            "kernel": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
            "page_cache_cleared": False,
            "cache_note": "Linux system page cache was not cleared; these are warm/ambient-cache measurements, not cold physical-I/O measurements.",
        },
        "methodology": {
            "corpus_sizes": list(SIZES),
            "profiles": list(PROFILES),
            "scenarios": list(SCENARIOS),
            "repeats_per_combination": REPEATS,
            "expected_measurements": 90,
            "completed_measurements": len(samples),
            "timed_command": "asgend-memory scan ROOT",
            "timeout_seconds_per_command": timeout_seconds,
            "peak_rss_measurement": "sample Linux /proc/<pid>/status VmHWM while scanner child is alive",
            "throughput_caveat": "logical_mib_per_second = total source-corpus bytes / elapsed time; not measured physical bytes read, especially for no-op scans.",
            "scenario_setup": {
                "new_index": "remove .asgend_mem before the timed run; timed scan builds a fresh index",
                "no_op": "untimed initial scan followed by timed unchanged scan",
                "single_file_change": "untimed initial scan, mutate exactly doc_00000, then time scan",
            },
        },
        "fixture_manifest": {
            "file_count": len(all_fixture_hashes),
            "total_source_bytes": sum(x["size_bytes"] for x in all_fixture_hashes),
            "sha256_by_file": all_fixture_hashes,
        },
        "summaries": summaries,
        "measurements": samples,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True, help="built asgend-memory C scanner binary")
    parser.add_argument("--fixture-root", type=Path, required=True, help="directory to create deterministic corpora under")
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=120.0)
    args = parser.parse_args()
    binary = args.binary.resolve()
    fixture_root = args.fixture_root.resolve()
    output = args.output_json.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"scanner binary is not executable: {binary}")
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    try:
        report = run_benchmark(binary, fixture_root, args.timeout_seconds)
        if report["methodology"]["completed_measurements"] != 90:
            raise RuntimeError(f"expected 90 measurements, got {report['methodology']['completed_measurements']}")
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"[stage31] wrote {output} with 90/90 measurements", flush=True)
        return 0
    except Exception as exc:
        print(f"[stage31] ERROR: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
