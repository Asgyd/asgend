#!/usr/bin/env python3
"""Render Stage 31 Markdown directly from the latest raw benchmark JSON.

All medians and validation statements are recomputed from measurements[], never copied
from the report's precomputed summaries[] field, to prevent stale report values.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any


def render(data: dict[str, Any]) -> str:
    samples = data.get("measurements")
    method = data.get("methodology", {})
    if not isinstance(samples, list) or not samples:
        raise ValueError("raw measurements[] is missing or empty")
    if len(samples) != method.get("expected_measurements") or len(samples) != 90:
        raise ValueError(f"expected 90 raw measurements, got {len(samples)}")

    failures = []
    expected_groups = defaultdict(list)
    for i, sample in enumerate(samples, 1):
        key = (sample.get("size"), sample.get("profile"), sample.get("scenario"))
        expected_groups[key].append(sample)
        if sample.get("returncode") != 0 or sample.get("timed_out") is not False or sample.get("pipes_closed") is not True:
            failures.append(f"sample {i}: process/pipe failure")
        if not isinstance(sample.get("peak_rss_kib"), int) or sample["peak_rss_kib"] <= 0:
            failures.append(f"sample {i}: invalid peak RSS")
        if not math.isfinite(sample.get("wall_seconds", float("nan"))) or sample["wall_seconds"] <= 0:
            failures.append(f"sample {i}: invalid wall time")
        if sample.get("last_scan_files_seen") != sample.get("size"):
            failures.append(f"sample {i}: last_scan_files_seen mismatch")
        expected_id = 1 if sample.get("scenario") == "new_index" else 2
        if sample.get("last_scan_id") != expected_id:
            failures.append(f"sample {i}: last_scan_id mismatch")
        if sample.get("scenario") == "single_file_change" and sample.get("modified") != 1:
            failures.append(f"sample {i}: one-file change was not detected exactly once")
    if failures:
        raise ValueError("raw measurement validation failed: " + "; ".join(failures[:12]))
    if len(expected_groups) != 18 or any(len(group) != 5 for group in expected_groups.values()):
        raise ValueError("expected all 18 size/profile/scenario groups with five repeats each")

    lines = [
        "# ASGEND Stage 31 — Memory Scan/Index Performance Benchmark",
        "",
        f"- Generated UTC: `{data.get('generated_utc', 'unknown')}`",
        f"- Real scanner binary SHA-256: `{data.get('binary_sha256', 'missing')}`",
        f"- Harness SHA-256: `{data.get('harness_sha256', 'missing')}`",
        f"- Runner helper SHA-256: `{data.get('helper_sha256', 'missing')}`",
        f"- Fixture files hashed: **{data.get('fixture_manifest', {}).get('file_count', 'unknown')}**",
        f"- Measurement completion: **{len(samples)}/90**",
        "",
        "## Method",
        "",
        "The timed operation is the actual C executable `asgend-memory scan ROOT`, not a mock or Python filesystem walker. Each corpus is deterministic and its every source file is listed with SHA-256 in the JSON fixture manifest. A fresh-index sample removes `.asgend_mem` before timing. A no-op sample performs an untimed initial scan before the timed unchanged scan. A single-file-change sample performs an untimed initial scan, restores `doc_00000` to the original fixture, modifies that one file, then times the scan.",
        "",
        "Wall time is measured around the scanner process. Peak RSS is sampled from Linux `/proc/<pid>/status` `VmHWM`. The runner uses its own process group, drains both pipes, and kills the group on timeout. The timeout regression is registered in CTest separately.",
        "",
        "**Cache/throughput limitation:** Linux page cache was not cleared. These timings are not cold physical-I/O measurements. `logical MiB/s` is total original source-corpus bytes divided by elapsed wall time; it is a normalized logical-throughput ratio, not measured disk bytes read. For no-op scans it must not be interpreted as actual read bandwidth.",
        "",
        "## Recomputed results",
        "",
        "All values below are recomputed from the raw `measurements[]` array in the JSON file supplied to this renderer. Median wall time is in seconds, files/s counts the scanner-reported `files`, logical MiB/s uses the fixture-corpus byte total, and peak RSS is median of per-process `VmHWM` samples.",
        "",
        "| Files | Profile | Scenario | Runs | Median wall (s) | Range (s) | Median files/s | Median logical MiB/s | Median peak RSS (KiB) |",
        "|---:|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for key in sorted(expected_groups, key=lambda k: (k[0], k[1], k[2])):
        group = expected_groups[key]
        walls = [s["wall_seconds"] for s in group]
        rates = [s["files_per_second"] for s in group]
        logical_rates = [s["logical_mib_per_second"] for s in group]
        rss = [s["peak_rss_kib"] for s in group]
        lines.append(
            f"| {key[0]:,} | `{key[1]}` | `{key[2]}` | {len(group)} | "
            f"{statistics.median(walls):.3f} | {min(walls):.3f}–{max(walls):.3f} | "
            f"{statistics.median(rates):.1f} | {statistics.median(logical_rates):.2f} | {statistics.median(rss):.0f} |"
        )

    lines.extend([
        "",
        "## Validation findings",
        "",
        f"- All 90 timed scanner invocations returned exit code 0, did not time out, and closed both pipes: **{all(s['returncode'] == 0 and s['timed_out'] is False and s['pipes_closed'] is True for s in samples)}**.",
        f"- Every measurement observed a positive Linux `VmHWM`: **{all(isinstance(s['peak_rss_kib'], int) and s['peak_rss_kib'] > 0 and s['peak_rss_source'] == 'linux_proc_vm_hwm' for s in samples)}**.",
        f"- `last_scan_id` matched 1 for fresh-index samples and 2 for setup-plus-measurement samples: **{all(s['last_scan_id'] == (1 if s['scenario'] == 'new_index' else 2) for s in samples)}**.",
        f"- `last_scan_files_seen` matched corpus size in every row: **{all(s['last_scan_files_seen'] == s['size'] for s in samples)}**.",
        f"- Every single-file-change sample detected exactly one modified file: **{all(s['modified'] == 1 for s in samples if s['scenario'] == 'single_file_change')}**.",
        "- No claim is made about cold-cache physical I/O or production deployment performance.",
        "",
        "## Raw source of truth",
        "",
        "The JSON file next to this report is authoritative for all sample values, individual fixture hashes, environment facts, and the exact scanner/harness/helper hashes. Re-render this Markdown after any JSON change; do not manually edit numeric results.",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_json", type=Path)
    parser.add_argument("output_md", type=Path)
    args = parser.parse_args()
    data = json.loads(args.input_json.read_text(encoding="utf-8"))
    text = render(data)
    args.output_md.parent.mkdir(parents=True, exist_ok=True)
    args.output_md.write_text(text, encoding="utf-8")
    print(f"rendered {args.output_md} from {args.input_json}; raw measurements={len(data['measurements'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
