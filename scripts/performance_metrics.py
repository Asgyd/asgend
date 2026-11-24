#!/usr/bin/env python3
"""Shared latency summary helpers for local performance diagnostics.

Tail percentiles are deliberately omitted for small samples. Values are exploratory
benchmarks, not service-level objectives or confidence intervals.
"""
from __future__ import annotations

import math
import statistics
from typing import Any

P95_MIN_SAMPLES = 20
P99_MIN_SAMPLES = 100


def nearest_rank_percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    if not math.isfinite(q) or not 0.0 <= q <= 1.0:
        raise ValueError("percentile q must be finite and in [0, 1]")
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(q * len(ordered)) - 1))
    return ordered[index]


def summarize_latency(values: list[float], *, decimals: int = 3) -> dict[str, Any]:
    if not values:
        raise ValueError("latency summary requires at least one sample")
    if any(not math.isfinite(value) or value < 0 for value in values):
        raise ValueError("latency samples must be finite and non-negative")
    count = len(values)
    p95_ready = count >= P95_MIN_SAMPLES
    p99_ready = count >= P99_MIN_SAMPLES
    rounded = lambda value: round(value, decimals)
    return {
        "samples": count,
        "min": rounded(min(values)),
        "mean": rounded(statistics.fmean(values)),
        "median": rounded(statistics.median(values)),
        "p50": rounded(nearest_rank_percentile(values, 0.50)),
        "p95": rounded(nearest_rank_percentile(values, 0.95)) if p95_ready else None,
        "p99": rounded(nearest_rank_percentile(values, 0.99)) if p99_ready else None,
        "max": rounded(max(values)),
        "tail_percentile_status": {
            "p95": "estimated" if p95_ready else f"not_reported_requires_{P95_MIN_SAMPLES}_samples",
            "p99": "estimated" if p99_ready else f"not_reported_requires_{P99_MIN_SAMPLES}_samples",
        },
    }


def path_recall_at_k(exact_paths: list[str], approximate_paths: list[str], k: int) -> float:
    """Compute path overlap against unique exact-result paths, robust to duplicates."""
    if k < 1:
        raise ValueError("k must be >= 1")
    exact = {path for path in exact_paths if isinstance(path, str) and path}
    approximate = {path for path in approximate_paths if isinstance(path, str) and path}
    denominator = min(k, len(exact))
    if denominator == 0:
        return 0.0
    return len(exact & approximate) / denominator
