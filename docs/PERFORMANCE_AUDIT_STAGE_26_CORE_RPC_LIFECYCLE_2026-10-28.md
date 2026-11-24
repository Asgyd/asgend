# Stage 26 — Core RPC / Task Lifecycle Performance Audit

**Дата:** 28 октября 2026 г.  
**Статус пункта:** завершён в границах локального диагностического baseline.  
**Следующий приоритет:** измерения SQLite/artifacts с ростом объёма данных.

## Краткий результат

- 2 700 raw samples: 9 методов/сценариев × concurrency 1/4/16 × 100 образцов на сочетание.
- 15 независимых запусков Core с отдельной временной SQLite-базой: 5 повторов на каждый уровень concurrency.
- 0 ошибочных образцов; все операции завершились ожидаемыми состояниями.
- Полный CTest: 49/49 PASS за 250,11 секунды; все тесты имеют конечный положительный timeout.
- Release-сборка прошла; harness зарегистрирован в CTest через `performance_core_rpc_lifecycle_smoke`.

## Ключевые измерения

| Операция | Concurrency 1 p50/p95/p99 (ms) | Concurrency 4 p50/p95/p99 (ms) | Concurrency 16 p50/p95/p99 (ms) |
|---|---:|---:|---:|
| `ping` | 0,115 / 0,289 / 0,334 | 0,452 / 1,127 / 2,016 | 1,062 / 2,366 / 3,069 |
| `submit` | 1,823 / 3,256 / 4,847 | 6,982 / 16,088 / 24,072 | 18,968 / 48,459 / 61,539 |
| `status` | 0,717 / 2,037 / 2,975 | 3,273 / 8,438 / 13,525 | 9,523 / 30,693 / 35,169 |
| `list_tasks` | 0,825 / 3,140 / 4,800 | 3,475 / 8,280 / 9,060 | 10,662 / 28,928 / 32,110 |
| `events` | 0,267 / 1,354 / 2,211 | 4,638 / 13,834 / 18,810 | 8,000 / 34,238 / 42,141 |
| `answer` | 1,334 / 2,700 / 3,781 | 4,185 / 10,742 / 14,418 | 11,034 / 35,276 / 45,878 |
| `cancel` | 0,761 / 1,232 / 1,684 | 1,978 / 4,262 / 7,121 | 11,007 / 25,225 / 36,058 |
| `retry_task` | 1,123 / 2,442 / 2,920 | 3,491 / 9,015 / 12,744 | 9,767 / 31,528 / 34,099 |
| `submit_to_terminal` | 154,164 / 208,629 / 2 943,807 | 310,259 / 1 639,328 / 1 741,658 | 2 389,547 / 2 759,491 / 2 797,657 |

Each operation/level contains 100 observations pooled from five independent runs, so p95/p99 are descriptive estimates rather than stable service-level guarantees. `submit_to_terminal` includes polling time and should not be compared directly with the latency of an individual RPC call.

## Interpretation

1. `submit` p99 rises from 4,847 ms at concurrency 1 to 61,539 ms at concurrency 16 (~12.7×). `events` rises from 2,211 to 42,141 ms (~19.1×); `answer` rises from 3,781 to 45,878 ms (~12.1×).
2. `submit_to_terminal` median is 154 ms / 310 ms / 2 390 ms at concurrency 1/4/16. Aggregate throughput is 3,393 / 7,243 / 7,107 completed tasks/s; the 4-to-16 client increase does not improve throughput in this workload while latency increases substantially.
3. The data suggests local saturation, but it does **not** identify the bottleneck. SQLite lock/transaction contention, worker scheduling, socket/thread overhead, and queue synchronization remain hypotheses until instrumented.
4. The host reports 3 logical CPUs, but cgroup CPU quota is `200000 100000` (about 2 cores); cgroup memory limit is 4 GiB. A local mock LLM and disposable SQLite DB are used; remote providers, live PostgreSQL/Control Plane, and real client devices are excluded.

## Reproducibility and data integrity

The raw JSON records all 2 700 samples, 15 run records, monotonic start/end timestamps, per-run result counts and environment metadata. Script and Release binary SHA-256 values match the artifacts used during the measurement. No auth token or RPC response bodies are stored in the benchmark report. All 49 CTest entries have finite positive timeout values.

## Artifacts

- Raw data and aggregate latency tables: `PERFORMANCE_CORE_RPC_LIFECYCLE_BENCHMARK_2026-10-28.json` and `.md`.
- Harness: `../scripts/benchmark_core_rpc_lifecycle.py`; smoke test: `../tests/test_benchmark_core_rpc_lifecycle.py`.
- Full CTest log: `PERFORMANCE_REPEAT_CTEST_LOG_2026-10-28.txt`.
- Three review iterations: `PERFORMANCE_RPC_AUDIT_ITERATION_{1,2,3}_2026-10-28.md`.
- Overall audit, decomposition and TODO: `PERFORMANCE_REPEAT_AUDIT_FINAL_2026-10-28.md`, `PERFORMANCE_REPEAT_REVIEW_DECOMPOSITION_2026-10-28.md`, `PERFORMANCE_REPEAT_TODO_2026-10-28.md`.

## Acceptance

- [x] Reproducible run matrix and raw samples.
- [x] No benchmark request errors; prepared lifecycle transitions validated.
- [x] Smoke registered in CTest.
- [x] Full CTest passes and timeout metadata checked.
- [x] Three documented critical-review iterations.
- [ ] Production-capacity conclusions — explicitly out of scope; needs a production-like runner and real dependency topology.
- [ ] SQLite/artifacts root-cause profiling — next P1 item.
