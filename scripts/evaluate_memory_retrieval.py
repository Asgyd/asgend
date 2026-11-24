#!/usr/bin/env python3
"""Evaluate retrieval against a judged JSON dataset using standard ranking metrics.

Dataset shape:
{
  "version": 1,
  "queries": [
    {"query": "how to refresh token", "relevant_paths": ["docs/auth.md"], "k": 5,
     "filters": {"type": "documentation", "tag": "security", "path_prefix": "docs/",
                  "metadata": {"audience": "internal"}}}
  ]
}

Usage: python3 scripts/evaluate_memory_retrieval.py --cli ./build/asgend-memory \
       --root /path/to/indexed/project --dataset judged-queries.json [--hybrid]
       [--plan | --compare-plan]
By default the benchmark uses lexical-only mode to keep runs deterministic and offline.
--plan evaluates explicit query planning; --compare-plan runs the same judged set with and without
planning and reports paired metric deltas. Configure the planner through ASGEND_MEMORY_DECISION_*.
"""
from __future__ import annotations
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path
from typing import Any


def metrics(ranked_paths: list[str], relevant_paths: set[str], k: int) -> dict[str, float]:
    top = ranked_paths[:k]
    hits = [1 if path in relevant_paths else 0 for path in top]
    hit_count = sum(hits)
    reciprocal_rank = 0.0
    for rank, hit in enumerate(hits, start=1):
        if hit:
            reciprocal_rank = 1.0 / rank
            break
    dcg = sum(1.0 / math.log2(rank + 1) for rank, hit in enumerate(hits, start=1) if hit)
    ideal_hits = min(k, len(relevant_paths))
    idcg = sum(1.0 / math.log2(rank + 1) for rank in range(1, ideal_hits + 1))
    return {
        f"precision@{k}": hit_count / k if k else 0.0,
        f"recall@{k}": hit_count / len(relevant_paths) if relevant_paths else 0.0,
        f"mrr@{k}": reciprocal_rank,
        f"ndcg@{k}": dcg / idcg if idcg else 0.0,
        f"hits@{k}": float(hit_count),
    }


def run_query(cli: str, root: str, item: dict[str, Any], default_k: int, hybrid: bool, timeout: int,
              plan: bool = False) -> dict[str, Any]:
    query = item.get("query")
    relevant = item.get("relevant_paths")
    if not isinstance(query, str) or not query.strip():
        raise ValueError("each query entry requires a non-empty 'query'")
    if not isinstance(relevant, list) or not relevant or not all(isinstance(path, str) and path for path in relevant):
        raise ValueError(f"query {query!r} requires a non-empty relevant_paths list")
    k = item.get("k", default_k)
    if not isinstance(k, int) or not 1 <= k <= 100:
        raise ValueError(f"query {query!r} has invalid k (expected 1..100)")
    cmd = [cli, "query", root, query, "--limit", str(k)]
    if not hybrid:
        cmd.append("--lexical-only")
    if plan:
        cmd.append("--plan")
    filters = item.get("filters", {})
    if not isinstance(filters, dict):
        raise ValueError("filters must be a JSON object")
    for key, flag in (("type", "--type"), ("tag", "--tag"), ("path_prefix", "--path-prefix")):
        value = filters.get(key)
        if value is not None:
            if not isinstance(value, str) or not value:
                raise ValueError(f"filter {key} must be a non-empty string")
            cmd.extend([flag, value])
    metadata = filters.get("metadata", {})
    if not isinstance(metadata, dict):
        raise ValueError("filters.metadata must be a JSON object")
    if len(metadata) > 1:
        raise ValueError("the current query CLI accepts at most one --metadata KEY=VALUE filter per query")
    for key, value in metadata.items():
        if not isinstance(key, str) or not key or not isinstance(value, str) or not value:
            raise ValueError("metadata filters require non-empty string keys and values")
        cmd.extend(["--metadata", f"{key}={value}"])
    proc = subprocess.run(cmd, text=True, capture_output=True, timeout=timeout)
    if proc.returncode != 0:
        raise RuntimeError(f"query failed for {query!r} (exit {proc.returncode}): {proc.stderr.strip()}")
    body = json.loads(proc.stdout)
    ranked = [entry.get("path", "") for entry in body.get("results", [])]
    report = {"query": query, "k": k, "relevant_paths": relevant, "ranked_paths": ranked,
              "metrics": metrics(ranked, set(relevant), k), "mode": body.get("mode", "unknown")}
    if plan:
        plan_info = body.get("query_plan", {})
        report["query_plan"] = {
            "requested": bool(plan_info.get("requested", False)),
            "used": bool(plan_info.get("used", False)),
            "intent": plan_info.get("intent", ""),
            "rewrites": plan_info.get("rewrites", []),
            "fallback_reason": plan_info.get("fallback_reason", ""),
        }
    return report


def aggregate(reports: list[dict[str, Any]]) -> dict[str, float]:
    if not reports:
        return {}
    metric_names = list(reports[0]["metrics"])
    return {name: sum(report["metrics"][name] for report in reports) / len(reports) for name in metric_names}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, help="path to asgend-memory binary")
    parser.add_argument("--root", required=True, help="project root with an existing .asgend_mem index")
    parser.add_argument("--dataset", required=True, help="JSON file with judged queries")
    parser.add_argument("--k", type=int, default=5, help="default evaluation cutoff, 1..100")
    parser.add_argument("--hybrid", action="store_true", help="allow configured embedding API for hybrid retrieval")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--plan", action="store_true", help="evaluate explicit query planning")
    modes.add_argument("--compare-plan", action="store_true", help="run paired baseline/planned evaluation and report deltas")
    parser.add_argument("--timeout", type=int, default=120, help="timeout per query in seconds")
    args = parser.parse_args()
    if not 1 <= args.k <= 100 or args.timeout < 1:
        parser.error("--k must be 1..100 and --timeout must be positive")
    try:
        dataset_path = Path(args.dataset)
        dataset = json.loads(dataset_path.read_text(encoding="utf-8"))
        if not isinstance(dataset, dict) or dataset.get("version") != 1 or not isinstance(dataset.get("queries"), list) or not dataset["queries"]:
            raise ValueError("dataset must be an object with version=1 and a non-empty queries array")
        common = (args.cli, args.root, args.k, args.hybrid, args.timeout)
        if args.compare_plan:
            baseline = [run_query(*common[:2], query, *common[2:], plan=False) for query in dataset["queries"]]
            planned = [run_query(*common[:2], query, *common[2:], plan=True) for query in dataset["queries"]]
            baseline_aggregate = aggregate(baseline)
            planned_aggregate = aggregate(planned)
            delta = {key: planned_aggregate[key] - baseline_aggregate.get(key, 0.0) for key in planned_aggregate}
            output = {
                "status": "complete", "evaluation_mode": "query_plan_comparison",
                "dataset": str(dataset_path), "corpus_root": str(Path(args.root)),
                "query_count": len(dataset["queries"]),
                "retrieval_mode": "hybrid" if args.hybrid else "bm25",
                "baseline": {"aggregate": baseline_aggregate, "queries": baseline},
                "planned": {"aggregate": planned_aggregate, "queries": planned},
                "delta_planned_minus_baseline": delta,
                "interpretation": "Positive values indicate metric gains; negative values indicate regressions. This score reflects the configured planner and judged dataset only.",
            }
        else:
            reports = [run_query(args.cli, args.root, query, args.k, args.hybrid, args.timeout, plan=args.plan)
                       for query in dataset["queries"]]
            output = {"status": "complete", "dataset": str(dataset_path),
                      "corpus_root": str(Path(args.root)), "query_count": len(reports),
                      "retrieval_mode": "hybrid" if args.hybrid else "bm25",
                      "aggregate": aggregate(reports), "queries": reports}
            if args.plan:
                output["evaluation_mode"] = "query_plan"
    except (OSError, json.JSONDecodeError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(json.dumps({"status": "error", "error": str(exc)}, ensure_ascii=False, indent=2), file=sys.stderr)
        return 2
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
