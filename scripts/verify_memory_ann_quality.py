#!/usr/bin/env python3
"""Independent verifier for Stage 34 ANN benchmark JSON, judged labels, manifest and fixtures."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import tarfile
from pathlib import Path, PurePosixPath
from typing import Any


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def rank_metrics(ranked_paths: list[str], relevant_paths: set[str], k: int) -> dict[str, float]:
    top = ranked_paths[:k]
    hit_ranks = [position for position, path in enumerate(top, 1) if path in relevant_paths]
    hit_count = len(hit_ranks)
    dcg = sum(1.0 / math.log2(rank + 1.0) for rank in hit_ranks)
    ideal_hits = min(k, len(relevant_paths))
    ideal_dcg = sum(1.0 / math.log2(rank + 1.0) for rank in range(1, ideal_hits + 1))
    return {
        f"precision@{k}": hit_count / k,
        f"recall@{k}": hit_count / len(relevant_paths) if relevant_paths else 0.0,
        f"mrr@{k}": (1.0 / hit_ranks[0]) if hit_ranks else 0.0,
        f"ndcg@{k}": (dcg / ideal_dcg) if ideal_dcg else 0.0,
        f"hits@{k}": float(hit_count),
    }


def nearest_rank(values: list[float], q: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(q * len(ordered)) - 1)]


def finite_positive(values: Any, field: str) -> list[float]:
    if not isinstance(values, list) or not values:
        raise ValueError(f"{field} must be a non-empty array")
    parsed = [float(value) for value in values]
    if any(not math.isfinite(value) or value <= 0 for value in parsed):
        raise ValueError(f"{field} contains non-positive or non-finite values")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result-json", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--fixture-archive", type=Path)
    parser.add_argument("--output-json", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = json.loads(args.result_json.read_text(encoding="utf-8"))
        dataset = json.loads(args.dataset.read_text(encoding="utf-8"))
        manifest_file = json.loads(args.manifest.read_text(encoding="utf-8"))
        if result.get("status") != "complete":
            raise ValueError(f"benchmark status is not complete: {result.get('status')}")
        if result.get("synthetic_only") is not True or result.get("live_provider_used") is not False:
            raise ValueError("benchmark must be explicitly marked synthetic-only and no-live-provider")
        if result.get("dataset_id") != dataset.get("dataset_id"):
            raise ValueError("dataset ID mismatch")
        if result.get("dataset_sha256") != sha256_bytes(args.dataset.read_bytes()):
            raise ValueError("dataset SHA-256 mismatch")
        if result.get("binary_sha256") != sha256_bytes(args.binary.read_bytes()):
            raise ValueError("binary SHA-256 mismatch")
        if result.get("harness_sha256") != sha256_bytes(args.harness.read_bytes()):
            raise ValueError("benchmark harness SHA-256 mismatch")
        rows = manifest_file.get("rows")
        if not isinstance(rows, list) or len(rows) != result.get("document_vectors") or len(rows) != manifest_file.get("vector_count"):
            raise ValueError("manifest vector count mismatch")
        if manifest_file.get("dataset_id") != dataset.get("dataset_id"):
            raise ValueError("manifest dataset ID mismatch")
        if sha256_bytes(canonical_json(rows)) != result.get("corpus_manifest_sha256"):
            raise ValueError("DB-derived corpus manifest SHA-256 mismatch")
        path_to_label: dict[str, str] = {}
        seen_paths: set[str] = set()
        for row in rows:
            path = row.get("relative_path")
            if not isinstance(path, str) or not path or path in seen_paths:
                raise ValueError(f"invalid or duplicate path in corpus manifest: {path!r}")
            seen_paths.add(path)
            path_to_label[path] = str(row.get("label", ""))
            if row.get("model_id") != result.get("model_id") or row.get("dimensions") != result.get("dimensions"):
                raise ValueError(f"model or dimension mismatch for {path}")
            for key in ("source_sha256", "input_sha256", "vector_json_sha256"):
                digest = row.get(key)
                if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
                    raise ValueError(f"invalid {key} for {path}")
            signature = row.get("ann_signature")
            if not isinstance(signature, str) or len(signature) != 4 or any(c not in "0123456789abcdef" for c in signature):
                raise ValueError(f"missing/invalid ANN signature for {path}")
        expected_queries = dataset.get("queries", [])
        got_queries = result.get("queries", [])
        smoke = len(got_queries) == 2 and result.get("measured_pairs_per_query") == 1
        if len(got_queries) not in (len(expected_queries), 2):
            raise ValueError("unexpected result query count")
        if result.get("judged_query_count") != len(got_queries):
            raise ValueError("judged query count mismatch")
        repetitions = int(result.get("measured_pairs_per_query", 0))
        if repetitions < 1:
            raise ValueError("invalid repetition count")
        if result.get("corpus_manifest_rows") != len(rows) or result.get("indexed_vectors") != len(rows):
            raise ValueError("benchmark/indexed vector count mismatch")
        if result.get("ann_signatures_missing") != 0 or result.get("sqlite_integrity") != "ok":
            raise ValueError("ANN signatures or SQLite integrity check failed")
        all_modes: dict[str, dict[str, list[float]]] = {
            "exact": {"precision": [], "recall": [], "mrr": [], "ndcg": [], "latencies": []},
            "ann": {"precision": [], "recall": [], "mrr": [], "ndcg": [], "latencies": []},
        }
        candidate_counts: list[int] = []
        paired_query_speedups: list[float] = []
        overlap_values: list[float] = []
        query_checks: list[dict[str, Any]] = []
        for actual, expected in zip(got_queries, expected_queries):
            if actual.get("id") != expected.get("id") or actual.get("query") != expected.get("query"):
                raise ValueError(f"query identity mismatch for {expected.get('id')}")
            k = int(expected["k"])
            relevant = set(expected["relevant_paths"])
            if set(actual.get("relevant_paths", [])) != relevant:
                raise ValueError(f"judged paths mismatch for {expected['id']}")
            if not relevant.issubset(seen_paths):
                raise ValueError(f"judged relevant path missing from corpus for {expected['id']}")
            for path in relevant:
                if path_to_label[path] != expected["id"]:
                    raise ValueError(f"dataset label/path mismatch: {expected['id']} versus {path}")
            metrics_by_mode: dict[str, dict[str, float]] = {}
            latency_by_mode: dict[str, list[float]] = {}
            for mode in ("exact", "ann"):
                item = actual.get(mode)
                if not isinstance(item, dict):
                    raise ValueError(f"missing {mode} query payload for {expected['id']}")
                ranked = item.get("ranked_paths")
                if not isinstance(ranked, list) or not ranked or len(ranked) > k or any(path not in seen_paths for path in ranked) or len(ranked) != len(set(ranked)):
                    raise ValueError(f"invalid {mode} ranked paths for {expected['id']}")
                measured = rank_metrics(ranked, relevant, k)
                saved = item.get("metrics", {})
                for metric, value in measured.items():
                    if metric not in saved or abs(float(saved[metric]) - value) > 1e-6:
                        raise ValueError(f"{mode} {metric} mismatch for {expected['id']}")
                    key = metric.split("@", 1)[0]
                    if key in all_modes[mode]:
                        all_modes[mode][key].append(value)
                latency_values = finite_positive(item.get("raw_latency_samples_ms"), f"{expected['id']} {mode} latency")
                if len(latency_values) != repetitions:
                    raise ValueError(f"wrong {mode} sample count for {expected['id']}")
                latency_by_mode[mode] = latency_values
                all_modes[mode]["latencies"].extend(latency_values)
                latency_summary = item.get("latency_ms", {})
                if latency_summary.get("count") != repetitions or abs(float(latency_summary.get("median", -1)) - statistics.median(latency_values)) > 0.002:
                    raise ValueError(f"{mode} latency summary mismatch for {expected['id']}")
                if mode == "exact":
                    if item.get("vector_search") != "exact_cosine" or item.get("document_ann_used") is not False:
                        raise ValueError("exact mode indicator mismatch")
                else:
                    if item.get("vector_search") != "ann_lsh16" or item.get("document_ann_used") is not True:
                        raise ValueError("ANN mode indicator mismatch")
                    if item.get("document_vector_count") != len(rows):
                        raise ValueError("ANN corpus count mismatch")
                    per_counts = item.get("paired_candidate_counts")
                    if not isinstance(per_counts, list) or len(per_counts) != repetitions:
                        raise ValueError(f"candidate count sample mismatch for {expected['id']}")
                    candidate_counts.extend(int(count) for count in per_counts)
                    if any(int(count) < 32 or int(count) >= len(rows) for count in per_counts):
                        raise ValueError(f"invalid ANN candidate counts for {expected['id']}")
                metrics_by_mode[mode] = measured
            paired = [a / b for a, b in zip(latency_by_mode["exact"], latency_by_mode["ann"])]
            expected_speedup = statistics.median(paired)
            saved_speedup = float(actual["ann"].get("median_paired_exact_over_ann", 0))
            if abs(expected_speedup - saved_speedup) > 0.001:
                raise ValueError(f"paired speedup mismatch for {expected['id']}")
            paired_query_speedups.append(expected_speedup)
            exact_relevant = [path for path in actual["exact"]["ranked_paths"] if path in relevant][:k]
            ann_relevant = [path for path in actual["ann"]["ranked_paths"] if path in relevant][:k]
            overlap = len(set(exact_relevant) & set(ann_relevant)) / max(1, len(set(exact_relevant)))
            if abs(overlap - float(actual.get("ann_relevant_overlap_with_exact_at_k", -1))) > 1e-6:
                raise ValueError(f"exact/ANN relevant overlap mismatch for {expected['id']}")
            overlap_values.append(overlap)
            query_checks.append({"id": expected["id"], "exact_metrics": metrics_by_mode["exact"], "ann_metrics": metrics_by_mode["ann"], "candidate_counts": actual["ann"]["paired_candidate_counts"], "paired_speedup_median": expected_speedup})
        expected_requests = len(got_queries) * (int(result.get("warmup_pairs_per_query", -1)) + repetitions) * 2
        if result.get("embedding_requests_expected") != expected_requests or result.get("embedding_requests_observed") != expected_requests or result.get("request_errors"):
            raise ValueError("embedding request accounting mismatch")
        for metric_mode in ("exact", "ann"):
            for metric_name in ("precision", "recall", "mrr", "ndcg"):
                values = all_modes[metric_mode][metric_name]
                calculated = statistics.mean(values)
                saved = float(result["aggregate"][metric_mode][f"{metric_name}@5"])
                if abs(calculated - saved) > 1e-6:
                    raise ValueError(f"aggregate {metric_mode} {metric_name} mismatch")
        all_latency: dict[str, list[float]] = {mode: all_modes[mode]["latencies"] for mode in ("exact", "ann")}
        for mode in ("exact", "ann"):
            expected_med = statistics.median(all_latency[mode])
            expected_p95 = nearest_rank(all_latency[mode], .95)
            if abs(expected_med - float(result["aggregate"][f"median_{mode}_latency_ms"])) > 0.002:
                raise ValueError(f"aggregate {mode} median latency mismatch")
            if abs(expected_p95 - float(result["aggregate"][f"p95_{mode}_latency_ms"])) > 0.002:
                raise ValueError(f"aggregate {mode} p95 latency mismatch")
        median_candidate_ratio = statistics.median(count / len(rows) for count in candidate_counts)
        maximum_candidate_ratio = max(count / len(rows) for count in candidate_counts)
        min_candidates = min(candidate_counts)
        acceptance_actual = {
            "mean_exact_recall_at_5": statistics.mean(all_modes["exact"]["recall"]),
            "mean_ann_recall_at_5": statistics.mean(all_modes["ann"]["recall"]),
            "mean_ann_mrr_at_5": statistics.mean(all_modes["ann"]["mrr"]),
            "mean_ann_ndcg_at_5": statistics.mean(all_modes["ann"]["ndcg"]),
            "median_ann_candidate_ratio": median_candidate_ratio,
            "all_queries_minimum_ann_candidates": min_candidates,
            "all_queries_maximum_ann_candidate_ratio": maximum_candidate_ratio,
        }
        criteria = dataset["acceptance_criteria"]
        acceptance_check = {
            "mean_exact_recall_at_5": acceptance_actual["mean_exact_recall_at_5"] >= criteria["min_mean_exact_recall_at_k"],
            "mean_ann_recall_at_5": acceptance_actual["mean_ann_recall_at_5"] >= criteria["min_mean_ann_recall_at_k"],
            "mean_ann_mrr_at_5": acceptance_actual["mean_ann_mrr_at_5"] >= criteria["min_mean_ann_mrr_at_k"],
            "mean_ann_ndcg_at_5": acceptance_actual["mean_ann_ndcg_at_5"] >= criteria["min_mean_ann_ndcg_at_k"],
            "median_ann_candidate_ratio": median_candidate_ratio <= criteria["max_median_ann_candidate_ratio"],
            "all_queries_minimum_ann_candidates": min_candidates >= criteria["min_ann_candidates_per_query"],
            "all_queries_maximum_ann_candidate_ratio": maximum_candidate_ratio <= criteria["max_ann_candidates_per_query_ratio"],
            "sqlite_integrity": result.get("sqlite_integrity") == "ok",
            "request_accounting": result.get("embedding_requests_observed") == expected_requests,
        }
        stored_actual_keys = {
            "mean_exact_recall_at_5": "actual",
            "mean_ann_recall_at_5": "actual",
            "mean_ann_mrr_at_5": "actual",
            "mean_ann_ndcg_at_5": "actual",
            "median_ann_candidate_ratio": "actual",
            "all_queries_minimum_ann_candidates": "actual_minimum",
            "all_queries_maximum_ann_candidate_ratio": "actual_maximum",
        }
        for name, passed in acceptance_check.items():
            stored = result.get("acceptance", {}).get(name, {})
            if stored.get("pass") is not passed:
                raise ValueError(f"stored acceptance decision differs from independent result for {name}")
            key = stored_actual_keys.get(name)
            if key is not None and (stored.get(key) is None or abs(float(stored[key]) - float(acceptance_actual[name])) > 1e-6):
                raise ValueError(f"stored acceptance value differs from independent result for {name}")
            if not passed:
                raise ValueError(f"acceptance criterion failed: {name} actual={acceptance_actual.get(name)}")
        if result.get("acceptance", {}).get("all_queries_maximum_ann_candidate_ratio", {}).get("actual_maximum") is None:
            raise ValueError("maximum candidate ratio not reported")
        archive_summary = None
        if args.fixture_archive:
            expected_sources = {row["relative_path"]: row["source_sha256"] for row in rows}
            with tarfile.open(args.fixture_archive, "r:gz") as archive:
                members = archive.getmembers()
                for member in members:
                    parsed = PurePosixPath(member.name)
                    if parsed.is_absolute() or ".." in parsed.parts or not member.isfile():
                        raise ValueError(f"unsafe archive member: {member.name}")
                member_names = [member.name for member in members]
                if len(member_names) != len(set(member_names)):
                    raise ValueError("fixture archive contains duplicate member names")
                files = {member.name: member for member in members}
                source_members = [name for name in files if name.startswith("corpus/") and name.endswith(".md")]
                expected_source_members = {"corpus/seed.md"} | {"corpus/" + path for path in expected_sources}
                if set(source_members) != expected_source_members:
                    raise ValueError("fixture archive source member set differs from corpus manifest")
                if set(files) != expected_source_members | {"dataset/ann_quality_judged_dataset.json"}:
                    raise ValueError("fixture archive has unexpected or missing members")
                if len(source_members) != len(expected_sources) + 1:
                    raise ValueError(f"fixture archive markdown count mismatch: {len(source_members)}")
                for relative, digest in expected_sources.items():
                    name = "corpus/" + relative
                    member = files.get(name)
                    if member is None:
                        raise ValueError(f"fixture archive missing {relative}")
                    stream = archive.extractfile(member)
                    if stream is None or sha256_bytes(stream.read()) != digest:
                        raise ValueError(f"fixture source SHA-256 mismatch in archive: {relative}")
                dataset_member = files.get("dataset/ann_quality_judged_dataset.json")
                if dataset_member is None:
                    raise ValueError("fixture archive missing judged dataset")
                stream = archive.extractfile(dataset_member)
                if stream is None or sha256_bytes(stream.read()) != sha256_bytes(args.dataset.read_bytes()):
                    raise ValueError("fixture archive dataset SHA-256 mismatch")
                archive_summary = {"member_count": len(members), "markdown_files": len(source_members), "source_hashes_checked": len(expected_sources), "dataset_hash_matches": True}
        output = {
            "status": "verified", "independent": True, "result_json_sha256": sha256_bytes(args.result_json.read_bytes()),
            "dataset_sha256": sha256_bytes(args.dataset.read_bytes()), "binary_sha256": sha256_bytes(args.binary.read_bytes()),
            "harness_sha256": sha256_bytes(args.harness.read_bytes()), "manifest_rows": len(rows),
            "manifest_sha256": sha256_bytes(canonical_json(rows)), "query_count": len(got_queries), "smoke_subset": smoke,
            "measured_pairs_per_query": repetitions, "requests_verified": expected_requests,
            "aggregate_metrics_recalculated": {
                "exact": {f"{name}@5": statistics.mean(all_modes["exact"][name]) for name in ("precision", "recall", "mrr", "ndcg")},
                "ann": {f"{name}@5": statistics.mean(all_modes["ann"][name]) for name in ("precision", "recall", "mrr", "ndcg")},
            },
            "acceptance_actual_recalculated": acceptance_actual, "acceptance_pass": acceptance_check,
            "fixture_archive": archive_summary, "query_checks": query_checks,
        }
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(json.dumps(output, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps({"status": "verified", "queries": len(got_queries), "manifest_rows": len(rows), "requests": expected_requests,
                          "fixture_archive": archive_summary, "output": str(args.output_json)}, ensure_ascii=False))
        return 0
    except (OSError, json.JSONDecodeError, ValueError, KeyError, TypeError, tarfile.TarError) as exc:
        print(f"verify_memory_ann_quality: {exc}", file=__import__("sys").stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
