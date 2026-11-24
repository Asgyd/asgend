# Task-list index write/read cost benchmark — 2026-10-28

## Purpose and decision

Compare the existing production task indexes (`idx_tasks_status_created`, `idx_tasks_queue_priority`) with the isolated Stage 28 candidate that adds two partial ordered indexes. This profile measures the trade-off among ordered-list read latency, seed insert/index-build maintenance, insert batches, status transitions, priority updates, delete+reinsert, and on-disk index/page usage. Candidate indexes exist only in disposable benchmark databases; `src/db.c` and production migrations were not changed by Stage 29.

**Decision:** do not promote the candidate partial indexes to production yet. They accelerate ordered reads at 10k/100k in this synthetic workload, but index storage increases and write-operation results are mixed and distribution-dependent. The measurements below justify an explicit product/workload decision and a follow-up C API/live workload profile, not an unconditional migration.

## Method

- Sizes: 1,000 / 10,000 / 100,000 task rows.
- Status distributions: balanced weighted synthetic distribution (not asserted to match production) and queued-heavy (70% queued).
- Five independent database pairs for each size/distribution; alternate which configuration is seeded first by run.
- Per configuration and scenario: 20 samples × 5 runs = 100 samples for each operation; p95/p99 follow `performance_metrics.py` thresholds.
- Operations: ordered task-list query, insert batch, queued/running status transition, priority update, delete+reinsert. For each config, writes use one SQLite writer and measure committed transactions. State-reset queries for status/priority measurements are outside the timed interval; insert cleanup is outside the measured insert interval.
- Candidate config adds `idx_ab_running_order` and `idx_ab_other_order` before seed insertion, so seed time includes maintaining those indexes. File size and `dbstat` index bytes are captured after `ANALYZE` and `wal_checkpoint(TRUNCATE)`. At the end of every paired database run, `PRAGMA integrity_check` and exact row-count invariants are checked.
- The benchmark is Python `sqlite3`/local filesystem. It does not include Core C mutexes or end-to-end TCP/JSON overhead and is not a production SLO. Per-operation timings include committed WAL transactions with `synchronous=FULL`; periodic WAL checkpoint cost is not included in those operation samples. Initial file/index size is measured after a checkpoint.

## Environment

- Timestamp (UTC): 2026-10-28T05:26:01.428300+00:00
- OS/kernel: Linux-6.18.55-x86_64-with-glibc2.41; arch: x86_64
- Python 3.13.5; SQLite 3.46.1; logical CPUs: 3
- cgroup: CPU quota 2.0 cores (`200000 100000`); memory cap 4,294,967,296 bytes
- `src/db.c` SHA-256: `d7cd34a63104d51f8d14bc0d436cddead96f5fa2c02a53559ca2f9d73d1f9929`
- benchmark script SHA-256: `5542a7686217442f06964a011bf1cf2169ce0ad0ba5fcf659317baab367127b4`
- Outcome: `passed=True`; no application errors were observed by this benchmark harness.

## Results

Ratios: list speedup = baseline median / candidate median (>1 means candidate faster); write overhead = (candidate median / baseline median - 1) (positive means candidate slower, negative means candidate median was faster in this run). A negative value is not interpreted as proof that adding indexes accelerates writes; it can reflect variability and cache effects.

| Rows | Distribution | Added index bytes | Index bytes overhead | DB file overhead | Seed insert median base→candidate (ms) | List read median base→candidate (ms) | List speedup | Insert batch overhead | Status transition overhead | Priority update overhead | Delete+reinsert overhead |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | balanced | 53,248 | 37.14% | 16.67% | 8.285 → 8.222 | 1.371 → 0.908 | 1.51× | -7.57% | -1.45% | -1.14% | -8.27% |
| 1,000 | queued_heavy | 28,672 | 20.59% | 9.09% | 7.142 → 8.538 | 1.430 → 1.285 | 1.11× | +4.98% | +13.79% | +3.00% | +5.48% |
| 10,000 | balanced | 413,696 | 30.42% | 13.74% | 75.513 → 85.206 | 4.088 → 0.642 | 6.37× | -2.82% | +15.93% | +38.78% | +17.23% |
| 10,000 | queued_heavy | 167,936 | 12.89% | 5.69% | 81.705 → 85.396 | 5.251 → 0.688 | 7.63× | -15.67% | +25.00% | +15.23% | +18.93% |
| 100,000 | balanced | 4,214,784 | 31.13% | 13.79% | 1200.034 → 1431.076 | 29.880 → 0.896 | 33.35× | +24.74% | +29.36% | +73.50% | +45.35% |
| 100,000 | queued_heavy | 1,703,936 | 13.09% | 5.78% | 1092.402 → 1227.144 | 39.309 → 0.877 | 44.82× | -7.64% | +33.41% | -49.42% | -16.65% |

### Tail latency at 100k rows

| Distribution | Operation | Base median / p95 / p99 (ms) | Candidate median / p95 / p99 (ms) |
|---|---|---:|---:|
| balanced | list_read | 29.880 / 49.335 / 86.913 | 0.896 / 2.139 / 3.346 |
| balanced | insert_batch | 2.563 / 6.106 / 8.694 | 3.197 / 6.327 / 8.868 |
| balanced | status_transition | 33.411 / 49.145 / 62.546 | 43.220 / 61.520 / 79.855 |
| balanced | priority_update | 6.944 / 10.934 / 13.960 | 12.048 / 18.188 / 24.420 |
| balanced | delete_reinsert | 11.075 / 21.454 / 25.907 | 16.097 / 35.042 / 48.432 |
| queued_heavy | list_read | 39.309 / 76.701 / 148.178 | 0.877 / 1.782 / 3.287 |
| queued_heavy | insert_batch | 2.800 / 5.567 / 9.902 | 2.586 / 10.088 / 15.749 |
| queued_heavy | status_transition | 28.171 / 54.691 / 58.880 | 37.582 / 53.067 / 76.974 |
| queued_heavy | priority_update | 16.630 / 29.018 / 38.781 | 8.411 / 12.342 / 17.599 |
| queued_heavy | delete_reinsert | 17.819 / 36.296 / 68.599 | 14.852 / 20.352 / 30.201 |

## Query plan and semantics

- The baseline full-list query uses the existing production indexes and computes the status rank / effective queued priority, resulting in a sort step in the current query plan.
- Candidate queries read separate status bands, leveraging the two isolated partial indexes for `running` and non-active/non-queued tasks. `queued` still needs the dynamic aging expression.
- For every size and distribution, all five paired baselines were checked for identical ordered rows between production-index SQL and candidate SQL (30 pairs total). All **60/60** post-write integrity and row-count checks passed.
- The benchmark was run on the schema/query mirror that checks the current `src/db.c` markers. It does not claim candidate query execution through the C API; Stage 28 separately validated result semantics against actual `db_list_tasks()`.

## Interpretation

- For 100k balanced data, candidate list-read median is ~33.5× faster; for 100k queued-heavy, ~44.8× faster. It comes with +4,214,784 / +1,703,936 bytes of index storage respectively, about +31.13% / +13.09% relative to baseline index storage.
- For 100k balanced data, candidate seed insertion median rises from 1,200.034 to 1,431.076 ms (~19.3% slower); status transitions median is +29.36%, priority updates +73.50%, insert batches +24.74%, and delete+reinsert +45.35%. This is a strong write-heavy trade-off.
- For queued-heavy 100k rows, seed insertion rises from 1,092.402 to 1,227.144 ms (~12.3%); status transitions are +33.41%, but insert/priority/delete medians reverse direction despite extra indexes. This pattern, plus some candidate p95 tails (for example insert batch 10.088 ms vs 5.567 ms baseline), points to noise/cache/layout sensitivity. It must not be interpreted as proof that adding indexes accelerates writes.
- At 1k rows the index storage cost is proportionally large (20.59–37.14% of current index bytes) and read benefit is small; at 10k the read path improves 6–7.6×, while status/priority/delete write operations frequently become slower.
- No production partial-index migration was added. Keep the candidate isolated until a benchmark through the actual C DB path, more representative state distribution, and write-focused evidence agree on a deployment trade-off.

## Files

- Raw measurements: `PERFORMANCE_TASK_LIST_INDEX_COST_2026-10-28.json`
- Harness: `scripts/benchmark_task_list_index_cost.py`
- Smoke test: `tests/test_task_list_index_cost.py`
- CTest name: `performance_task_list_index_cost_smoke`
