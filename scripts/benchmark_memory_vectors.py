#!/usr/bin/env python3
"""Compare exact cosine search with the persistent ANN path on an indexed project.

The measured latency is end-to-end CLI wall time: process startup, query embedding
endpoint latency, local search, and JSON output. This script deliberately alternates
exact/ANN execution order to reduce fixed-order warm-cache bias; it is not a pure
in-process search-kernel microbenchmark.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from performance_metrics import nearest_rank_percentile, path_recall_at_k, summarize_latency


def run_query(cli: str, root: str, query: str, top_k: int, exact: bool,
              env: dict[str, str]) -> tuple[dict[str, Any], float]:
    args = [cli, "query", root, query, "--limit", str(top_k), "--context-tokens", "100000"]
    if exact:
        args.append("--exact-vectors")
    start = time.perf_counter()
    proc = subprocess.run(args, text=True, capture_output=True, env=env, timeout=360)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    if proc.returncode != 0:
        raise RuntimeError(f"query failed ({proc.returncode}): {' '.join(args)}\n{proc.stderr.strip()}")
    try:
        result = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"query emitted invalid JSON: {exc}") from exc
    if not isinstance(result, dict):
        raise RuntimeError("query emitted a JSON value other than an object")
    return result, elapsed_ms


def query_order(repetition: int) -> tuple[tuple[str, bool], tuple[str, bool]]:
    """Alternate first-run mode across repetitions: bool is --exact-vectors."""
    if repetition % 2 == 0:
        return (("exact", True), ("ann", False))
    return (("ann", False), ("exact", True))


def percentile(values: list[float], q: float) -> float:
    return nearest_rank_percentile(values, q)


def latency_summary(values: list[float]) -> dict[str, Any]:
    return summarize_latency(values, decimals=3)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, help="Path to asgend-memory")
    parser.add_argument("--root", required=True, help="Project root containing .asgend_mem")
    parser.add_argument("--queries", required=True, help="JSON array of query strings or an object with a 'queries' array")
    parser.add_argument("--k", type=int, default=10, help="Top-K result count (1..100)")
    parser.add_argument("--repetitions", type=int, default=5, help="Paired exact/ANN measured repetitions per query (1..1000)")
    parser.add_argument("--warmup", type=int, default=1, help="Unmeasured paired warm-up repetitions per query (0..5)")
    args = parser.parse_args()
    if not 1 <= args.k <= 100 or not 1 <= args.repetitions <= 1000:
        parser.error("--k must be 1..100 and --repetitions must be 1..1000")
    if not 0 <= args.warmup <= 5:
        parser.error("--warmup must be 0..5")
    if not (os.environ.get("ASGEND_MEMORY_EMBEDDING_BASE_URL") and os.environ.get("ASGEND_MEMORY_EMBEDDING_MODEL")):
        parser.error("set ASGEND_MEMORY_EMBEDDING_BASE_URL and ASGEND_MEMORY_EMBEDDING_MODEL first")
    try:
        raw = json.loads(Path(args.queries).read_text(encoding="utf-8"))
        queries = raw.get("queries") if isinstance(raw, dict) else raw
        if not isinstance(queries, list) or not queries or any(not isinstance(q, str) or not q.strip() for q in queries):
            raise ValueError("dataset must be a non-empty array of non-empty query strings")
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        parser.error(f"invalid query dataset: {exc}")

    env = os.environ.copy()
    reports: list[dict[str, Any]] = []
    for query in queries:
        samples: dict[str, list[float]] = {"exact": [], "ann": []}
        results: dict[str, dict[str, Any]] = {}
        paired_speedups: list[float] = []

        # Warm each mode equally; alternate order for each warm-up too.
        for warmup_index in range(args.warmup):
            for mode, exact in query_order(warmup_index):
                result, _elapsed = run_query(args.cli, args.root, query, args.k, exact, env)
                results[mode] = result

        for repetition in range(args.repetitions):
            pair: dict[str, float] = {}
            for mode, exact in query_order(repetition):
                result, elapsed_ms = run_query(args.cli, args.root, query, args.k, exact, env)
                samples[mode].append(elapsed_ms)
                pair[mode] = elapsed_ms
                results[mode] = result
            if pair["ann"] > 0:
                paired_speedups.append(pair["exact"] / pair["ann"])

        exact_result = results.get("exact")
        ann_result = results.get("ann")
        if not exact_result or not ann_result:
            raise RuntimeError("query returned no benchmark result")
        if ann_result.get("vector_search") != "ann_lsh16" or not ann_result.get("document_ann_used") and not ann_result.get("chunk_ann_used"):
            raise RuntimeError("ANN did not activate. Run `asgend-memory ann-index ROOT`, ensure the selected model has >=5000 indexed vectors, and verify there are enough probe candidates.")
        exact_paths = [str(item.get("path", "")) for item in exact_result.get("results", []) if isinstance(item, dict)]
        ann_paths = [str(item.get("path", "")) for item in ann_result.get("results", []) if isinstance(item, dict)]
        recall = path_recall_at_k(exact_paths, ann_paths, args.k)
        exact_summary = latency_summary(samples["exact"])
        ann_summary = latency_summary(samples["ann"])
        paired_median_speedup = statistics.median(paired_speedups) if paired_speedups else None
        reports.append({
            "query": query,
            "exact_median_ms": exact_summary["median"],
            "ann_median_ms": ann_summary["median"],
            "exact_latency_ms": exact_summary,
            "ann_latency_ms": ann_summary,
            "median_paired_speedup": round(paired_median_speedup, 3) if paired_median_speedup is not None else None,
            "recall_at_k_by_document_path": round(recall, 4),
            "exact_vector_count": exact_result.get("document_vector_count", 0) + exact_result.get("chunk_vector_count", 0),
            "ann_candidates": ann_result.get("document_ann_candidates", 0) + ann_result.get("chunk_ann_candidates", 0),
            "ann_vector_count": ann_result.get("document_vector_count", 0) + ann_result.get("chunk_vector_count", 0),
            "ann_document_path_count": len(ann_paths),
        })

    speedups = [row["median_paired_speedup"] for row in reports if row["median_paired_speedup"] is not None]
    output = {
        "benchmark": "exact-cosine-vs-random-hyperplane-lsh16-radius4",
        "latency_scope": "end-to-end CLI wall time; includes process startup and embedding endpoint latency",
        "execution_order": "alternating exact-first and ANN-first by repetition; order-balanced warm-up",
        "k": args.k,
        "repetitions": args.repetitions,
        "warmup_repetitions": args.warmup,
        "queries": len(reports),
        "mean_recall_at_k_by_document_path": round(statistics.mean(row["recall_at_k_by_document_path"] for row in reports), 4),
        "median_paired_speedup": round(statistics.median(speedups), 3) if speedups else None,
        "results": reports,
        "notes": "This is a practical end-to-end comparison, not a search-kernel-only benchmark. Compare latency only on a stable endpoint and report Recall@K with latency. For CPU/search isolation, use a local deterministic embedding endpoint and identical host/corpus. Tail percentiles are omitted unless sample counts reach p95>=20 and p99>=100; they are still exploratory rather than a production SLO.",
    }
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired, ValueError) as exc:
        print(f"asgend-memory-benchmark-vectors: {exc}", file=sys.stderr)
        raise SystemExit(1)
