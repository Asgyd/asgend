# Stage 29 — Task-list index write/maintenance cost (2026-10-28)

## Status

**Local benchmark completed; candidate partial indexes remain experimental.** Fresh Release build succeeds. The focused CTest `performance_task_list_index_cost_smoke` passed **1/1 in 2.49 s** in the final full run. Full CTest passed **52/52 in 277.32 s, exit code 0**. All 52 generated timeout values are positive and finite.

## Work performed

- Added `scripts/benchmark_task_list_index_cost.py`, which compares current production indexes with the isolated Stage 28 partial-index candidate.
- Covers 1k/10k/100k rows and balanced versus queued-heavy (70% queued) status distributions.
- Five independent DB pairs per size/distribution and 20 samples per operation/run yield 100 raw samples per operation/configuration. Operations: ordered-list read, insert batch, status transition, priority update, delete+reinsert. Seed insertion is reported separately with five observations and no misleading tail percentiles.
- Captures index bytes from SQLite `dbstat`, database file size after WAL checkpoint, seed time, write transaction latency, query plans, Python/SQLite/OS, cgroup CPU/RAM caps and source/harness hashes. After write workloads, all 60 database integrity and exact row-count checks pass.
- Added `tests/test_task_list_index_cost.py` and registered `performance_task_list_index_cost_smoke` in CTest. The smoke deliberately runs 20 samples so p95 is available and p99 remains omitted (<100 samples); it also covers both distributions and all write operation types.
- Production SQL/schema was not changed in this stage; partial indexes remain only in disposable benchmark databases. Stage 28's stable `id ASC` tie-break remains the only production behavior change related to this workstream.

## Key results

See `PERFORMANCE_TASK_LIST_INDEX_COST_BENCHMARK_2026-10-28.md` and the raw JSON. Main results at 100k rows:

- Balanced distribution: candidate list-read median 29.880 → 0.896 ms (33.35×), added index storage 4,214,784 bytes (+31.13% relative to existing index storage), seed insertion 1,200.034 → 1,431.076 ms, status transition +29.36%, priority update +73.50%, insert batch +24.74%, delete+reinsert +45.35% median.
- Queued-heavy distribution: candidate list-read median 39.309 → 0.877 ms (44.82×), added index storage 1,703,936 bytes (+13.09%), seed insertion 1,092.402 → 1,227.144 ms. Write-operation medians are mixed, including anomalies where candidate appears faster; e.g. candidate insert-batch p95 is 10.088 ms vs 5.567 ms baseline. Raw distributions must be used before interpretation.
- Correctness: all five paired databases per size/distribution returned identical ordered results for baseline/candidate SQL; 30/30 ordered-result checks passed. After all write operations, 60/60 integrity checks returned `ok` and row counts matched. Stage 28 direct C reference already covers API semantic equivalence; Stage 29's timing path is the Python sqlite3 mirror, not C API latency.

## Decision

Do not promote the candidate partial indexes into production yet. They improve ordered-list reads at larger sizes, but increase index storage and have measurable write-maintenance costs. Some skewed-distribution write medians reverse direction, which points to cache/layout/noise sensitivity and means medians alone are insufficient for a migration decision. No absolute CI performance threshold is added.

## Limitations

- Local synthetic data, Python 3.13.5 / SQLite 3.46.1 on Linux x86-64, cgroup CPU quota 2 cores and 4 GiB RAM.
- Write measurements use one SQLite writer; they do not claim multi-writer throughput (SQLite serializes writes).
- Timings include commit cost under WAL + `synchronous=FULL`, but exclude periodic WAL checkpoint cost from operation samples. They do not include C `db_mutex`, JSON serialization or Core TCP overhead. Production/live service performance is not measured.
- Candidate SQL uses the Stage 28 mirrored query implementation; Stage 28 has the direct C `db_list_tasks()` correctness harness.

## Next item

Stage 30: run the same A/B via a benchmark path that measures actual C API latency and compare cache-cold/warm behavior; only then decide whether to maintain the candidate or close it as a documented non-adoption. Continue afterward with memory scan/index profiling.
