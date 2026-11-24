#!/usr/bin/env python3
"""Small bounded-concurrency HTTP smoke/load probe for a running Control Plane.

The probe intentionally records only status codes, latencies and exception
classes. It never logs bearer tokens, response bodies, task data or full URLs.
The result is a diagnostic smoke test, not a production capacity certification.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import os
import sys
import time
from collections import Counter
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlsplit
from urllib.request import Request, urlopen

from performance_metrics import summarize_latency


def _validate_base_url(base_url: str) -> str:
    if any(ord(ch) < 32 or ch.isspace() for ch in base_url):
        raise ValueError("base URL must not contain whitespace or control characters")
    parts = urlsplit(base_url)
    if parts.scheme not in {"http", "https"} or not parts.hostname:
        raise ValueError("base URL must be an absolute http(s) URL")
    if parts.username or parts.password or parts.query or parts.fragment:
        raise ValueError("base URL must not include credentials, query or fragment")
    if parts.scheme == "http" and parts.hostname not in {"127.0.0.1", "::1", "localhost"}:
        raise ValueError("plain HTTP is permitted only for loopback smoke tests")
    return base_url.rstrip("/")


def _latency_summary(values: list[float]) -> dict:
    """Summarize latency without presenting tiny-sample maxima as tail percentiles."""
    summary = summarize_latency(values, decimals=2)
    # Retain the existing load-probe wire keys: p50 is the nearest-rank p50.
    return {key: summary[key] for key in ("samples", "min", "mean", "p50", "p95", "p99", "max", "tail_percentile_status")}


def run_load_probe(*, base_url: str, workspace: str, token: str, requests: int = 200,
                   workers: int = 16, timeout: float = 5.0,
                   max_error_rate: float = 0.0) -> dict:
    """Run concurrent authenticated task-list requests and return a JSON-safe summary."""
    base_url = _validate_base_url(base_url)
    if not workspace or len(workspace) > 200:
        raise ValueError("workspace must be a non-empty id of at most 200 characters")
    if (not token or len(token) > 4096 or
            any(ord(ch) < 32 or ord(ch) == 127 for ch in token)):
        raise ValueError("token must be non-empty, bounded and free of control characters")
    if isinstance(requests, bool) or not isinstance(requests, int) or requests < 1 or requests > 100_000:
        raise ValueError("requests must be an integer between 1 and 100000")
    if isinstance(workers, bool) or not isinstance(workers, int) or workers < 1 or workers > 256:
        raise ValueError("workers must be an integer between 1 and 256")
    if not math.isfinite(timeout) or timeout <= 0 or timeout > 120:
        raise ValueError("timeout must be finite, greater than 0 and at most 120 seconds")
    if not 0.0 <= max_error_rate <= 1.0:
        raise ValueError("max_error_rate must be between 0 and 1")

    endpoint = f"{base_url}/v1/workspaces/{quote(workspace, safe='')}/tasks"

    def one_request(_index: int) -> tuple[int | None, float, str | None]:
        req = Request(endpoint, headers={"Authorization": f"Bearer {token}", "Accept": "application/json"})
        started = time.perf_counter()
        try:
            with urlopen(req, timeout=timeout) as response:
                # Bound client memory even if the target workspace has a very
                # large task listing. The context manager closes over-limit bodies.
                body = response.read(2 * 1024 * 1024 + 1)
                if len(body) > 2 * 1024 * 1024:
                    return None, (time.perf_counter() - started) * 1000, "ResponseTooLarge"
                return response.status, (time.perf_counter() - started) * 1000, None
        except HTTPError as exc:
            # Do not read or report response bodies; they might contain user data.
            try:
                exc.close()
            except Exception:
                pass
            return int(exc.code), (time.perf_counter() - started) * 1000, None
        except (URLError, TimeoutError, OSError) as exc:
            return None, (time.perf_counter() - started) * 1000, type(exc).__name__

    started = time.perf_counter()
    status_counts: Counter[str] = Counter()
    error_types: Counter[str] = Counter()
    latencies: list[float] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(workers, requests)) as pool:
        for status, latency, error_type in pool.map(one_request, range(requests)):
            latencies.append(latency)
            if status is None:
                status_counts["transport_error"] += 1
                error_types[error_type or "UnknownError"] += 1
            else:
                status_counts[str(status)] += 1
    elapsed = time.perf_counter() - started
    failures = requests - status_counts.get("200", 0)
    error_rate = failures / requests
    return {
        "target_host": urlsplit(base_url).hostname,
        "endpoint": "GET /v1/workspaces/{workspace}/tasks",
        "requests": requests,
        "workers": min(workers, requests),
        "elapsed_seconds": round(elapsed, 4),
        "requests_per_second": round(requests / elapsed, 2) if elapsed > 0 else 0.0,
        "status_counts": dict(sorted(status_counts.items())),
        "transport_error_types": dict(sorted(error_types.items())),
        "error_count": failures,
        "error_rate": round(error_rate, 6),
        "latency_ms": _latency_summary(latencies),
        "passed": error_rate <= max_error_rate,
        "note": "smoke/load diagnostic only; not a production capacity certification",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", required=True, help="Control Plane origin; HTTPS required except loopback")
    parser.add_argument("--workspace", required=True, help="workspace id used by the test token")
    parser.add_argument("--token-env", default="ASGEND_CP_LOAD_TEST_TOKEN",
                        help="environment variable holding the test user token (default: %(default)s)")
    parser.add_argument("--requests", type=int, default=200)
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--max-error-rate", type=float, default=0.0,
                        help="allowed failed request fraction, from 0 to 1")
    args = parser.parse_args(argv)
    token = os.environ.get(args.token_env, "")
    if not token:
        parser.error(f"environment variable {args.token_env} is unset")
    try:
        result = run_load_probe(base_url=args.base_url, workspace=args.workspace, token=token,
                                requests=args.requests, workers=args.workers, timeout=args.timeout,
                                max_error_rate=args.max_error_rate)
    except ValueError as exc:
        parser.error(str(exc))
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
