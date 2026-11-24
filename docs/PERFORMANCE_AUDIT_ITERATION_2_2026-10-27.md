# Performance audit — iteration 2: benchmark validity and measured ANN trade-off

**Date:** 2026-10-27
**Scope:** improve repeatability of memory vector measurements without changing retrieval ranking logic; add deterministic inventory and benchmark-methodology regression tests.

## Changes

- Added `scripts/audit_performance_inventory.py` and `tests/test_performance_inventory.py`. The inventory reports first-party source/test file counts and lines by component, the CTest test list and explicit timeouts, workflow test/build markers and largest source files. It labels itself as structural inventory—not a profiler.
- Fixed the inventory classifier ordering discovered in critical review: `src/asgend_tui.c` and `include/asgend_tui.h` must be grouped under `cli_tui` before the broad `src/` / `include/` rules.
- Registered `performance_inventory` in CTest.
- Reworked `scripts/benchmark_memory_vectors.py` to alternate exact-first / ANN-first order by repetition; add optional paired warm-ups; emit median, p95 and p99 samples for both modes; report paired speedup; document that timing includes process startup and embedding-endpoint latency.
- Added `tests/test_benchmark_memory_vectors.py` for alternating order, balance, percentile calculations and empty-sample validation, and registered it in CTest.
- Enhanced the 5,200-document synthetic ANN regression to run one warm-up per mode and five alternating measured pairs; it prints machine-readable timing and quality metrics without adding brittle absolute-time assertions.

## Measured result

Run command:

```sh
python3 tests/test_memory_ann.py /tmp/asgend-perf-build/asgend-memory
```

| Metric | Result |
|---|---:|
| Synthetic corpus | 5,200 vectors × 128 dimensions |
| Measured pairs | 5, after one warm-up per mode |
| Exact median end-to-end query | 237.904 ms |
| Exact p95 in this 5-sample run | 254.257 ms |
| ANN median end-to-end query | 49.483 ms |
| ANN p95 in this 5-sample run | 53.029 ms |
| Median paired speed-up | 4.831× |
| ANN candidate count | 251 / 5,200 (4.83%) |
| Path-based Recall@5 vs exact | 0.60 |
| Correct top result | yes |

The timing includes CLI process startup and a local mock embedding HTTP round trip; it is **not** pure search-kernel latency or representative of a remote embedding provider. p95/p99 on five samples are coarse diagnostics, not confidence-grade tails. The synthetic recall value of 0.60 makes the trade-off explicit: candidate reduction/speed alone is not sufficient, and representative relevance datasets are still required before tuning ANN thresholds.

The enhanced full test took 4.43 s wall with peak RSS reported by GNU time of 120,248 KiB. CTest target `memory_ann_smoke` passed; the two new performance test targets passed, 3/3 targeted CTest checks total. No absolute performance threshold was added to CI.

## Review result

The benchmark reduces fixed execution-order bias and reports paired latency/recall together. The old five-sample p95 fields are sample maxima and should not be interpreted as stable tail estimates; the repeat audit now suppresses p95 below 20 samples and p99 below 100. It still includes process and embedding transport costs, so it is best described as an **end-to-end local CLI benchmark**. The optional remote-endpoint benchmark has not been run because no authorized embedding endpoint credentials were provided. The benchmark script's actual external-endpoint path is therefore syntax/unit-tested but not production-executed here.

## Open actions carried forward

- Profile candidate deduplication (`find_hit`, graph/chunk merge paths) before changing data structures.
- Run scan/extraction and embedding-worker sweeps on multiple corpus sizes.
- Run Control Plane / PostgreSQL load tests and gather UI metrics on runners with the missing SDK/services.
- Gather at least five comparable benchmark samples before defining performance budgets.
