#!/usr/bin/env python3
"""Independently verify Stage 31 raw benchmark rows and deterministic fixture hashes."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from collections import Counter
from pathlib import Path
from typing import Any


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", required=True, type=Path, help="Raw benchmark JSON")
    parser.add_argument("--fixture-root", required=True, type=Path, help="Root with n1000/ and n10000/")
    parser.add_argument("--repo-root", type=Path, help="Optional repository root for harness/helper checks")
    parser.add_argument("--binary", type=Path, help="Optional measured scanner binary for digest check")
    args = parser.parse_args()

    data: dict[str, Any] = json.loads(args.json.read_text(encoding="utf-8"))
    rows = data.get("measurements")
    if data.get("stage") != 31 or not isinstance(rows, list) or len(rows) != 90:
        fail(f"expected stage 31 and 90 measurements; got stage={data.get('stage')} rows={len(rows) if isinstance(rows, list) else 'invalid'}")

    group_counts = Counter((r.get("size"), r.get("profile"), r.get("scenario")) for r in rows)
    if len(group_counts) != 18 or any(count != 5 for count in group_counts.values()):
        fail("measurement rows are not 18 groups of five repeats")
    if set(r.get("repeat") for r in rows) != {1, 2, 3, 4, 5}:
        fail("repeat identifiers are not exactly 1..5")

    for i, row in enumerate(rows, 1):
        if row.get("returncode") != 0 or row.get("timed_out") is not False or row.get("pipes_closed") is not True:
            fail(f"measurement #{i} did not exit cleanly: rc={row.get('returncode')} timeout={row.get('timed_out')} pipes_closed={row.get('pipes_closed')}")
        if not isinstance(row.get("peak_rss_kib"), (int, float)) or row["peak_rss_kib"] <= 0 or row.get("peak_rss_source") != "linux_proc_vm_hwm":
            fail(f"measurement #{i} has invalid peak RSS evidence")
        expected_id = 1 if row.get("scenario") == "new_index" else 2
        if row.get("last_scan_id") != expected_id or row.get("last_scan_files_seen") != row.get("size"):
            fail(f"measurement #{i} has wrong scan ID/files seen")
        if row.get("scenario") == "single_file_change" and row.get("modified") != 1:
            fail(f"measurement #{i} did not detect exactly one modified file")
        for numeric in ("wall_seconds", "files_per_second", "logical_mib_per_second"):
            value = row.get(numeric)
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
                fail(f"measurement #{i} invalid {numeric}: {value}")

    manifest = data.get("fixture_manifest", {})
    fixtures = manifest.get("sha256_by_file", [])
    if len(fixtures) != 33000 or manifest.get("file_count") != 33000:
        fail(f"expected 33000 individual fixture hashes; got {len(fixtures)}")
    total_bytes = 0
    seen_paths: set[str] = set()
    for i, item in enumerate(fixtures, 1):
        corpus = str(item.get("corpus", ""))
        relative = str(item.get("path", ""))
        rel = Path(corpus) / relative
        if rel.is_absolute() or ".." in rel.parts:
            fail(f"unsafe fixture path at row {i}: {rel}")
        rel_text = rel.as_posix()
        if rel_text in seen_paths:
            fail(f"duplicate fixture path: {rel_text}")
        seen_paths.add(rel_text)
        path = args.fixture_root / rel
        if not path.is_file():
            fail(f"missing fixture file: {path}")
        size = path.stat().st_size
        digest = sha256(path)
        if size != item.get("size_bytes") or digest != item.get("sha256"):
            fail(f"fixture hash/size mismatch: {rel_text}")
        total_bytes += size
    if total_bytes != manifest.get("total_source_bytes"):
        fail(f"fixture total bytes mismatch: got {total_bytes} expected {manifest.get('total_source_bytes')}")

    checked_hashes = {}
    if args.binary:
        actual = sha256(args.binary)
        if actual != data.get("binary_sha256"):
            fail(f"binary SHA mismatch: {actual}")
        checked_hashes[str(args.binary)] = actual
    if args.repo_root:
        for key, relative in (("harness_sha256", "scripts/benchmark_memory_scan_index.py"), ("helper_sha256", "scripts/memory_scan_runner.py")):
            path = args.repo_root / relative
            actual = sha256(path)
            if actual != data.get(key):
                fail(f"{relative} SHA mismatch: {actual}")
            checked_hashes[str(path)] = actual

    result = {
        "status": "PASS",
        "stage": 31,
        "measurements": len(rows),
        "groups": len(group_counts),
        "repeats_per_group": 5,
        "fixtures_verified": len(fixtures),
        "fixture_bytes_verified": total_bytes,
        "all_processes_clean": True,
        "all_metadata_valid": True,
        "all_single_file_changes_exact": True,
        "checked_code_hashes": checked_hashes,
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
