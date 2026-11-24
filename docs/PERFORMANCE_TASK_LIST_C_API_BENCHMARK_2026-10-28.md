# Task-list C API benchmark — полный результат Stage 30

Сырые samples и все конфигурационные детали находятся в `PERFORMANCE_TASK_LIST_C_API_BENCHMARK_2026-10-28.json`. Основной вывод: candidate partial indexes нельзя включать по результатам одного благоприятного статуса; для large `low_running_4` candidate существенно медленнее. Полный анализ, разложение времени, query plans, размеры DB, hashes и ограничения см. `PERFORMANCE_AUDIT_STAGE_30_TASK_LIST_C_API_2026-10-28.md`.

## Все 12 ячеек

| Rows | Distribution (running count) | Concurrency | API median baseline / candidate (ms) | API p95 baseline / candidate (ms) | API p99 baseline / candidate (ms) | Median speedup | Throughput ratio |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1,000 | 10% running (103) | 1 | 1.090 / 0.708 | 1.485 / 0.991 | 1.700 / 1.720 | 1.54× | 1.39× |
| 1,000 | 10% running (103) | 4 | 4.092 / 2.643 | 9.178 / 4.632 | 14.294 / 7.024 | 1.55× | 1.56× |
| 1,000 | 4 running (4) | 1 | 1.043 / 0.833 | 1.279 / 1.040 | 1.428 / 2.323 | 1.25× | 1.17× |
| 1,000 | 4 running (4) | 4 | 4.136 / 3.174 | 9.723 / 6.138 | 16.811 / 11.823 | 1.30× | 1.28× |
| 10,000 | 10% running (1,003) | 1 | 3.372 / 0.358 | 4.293 / 0.523 | 5.490 / 0.572 | 9.42× | 5.69× |
| 10,000 | 10% running (1,003) | 4 | 13.491 / 1.034 | 17.005 / 4.917 | 25.994 / 7.548 | 13.05× | 6.62× |
| 10,000 | 4 running (4) | 1 | 3.469 / 2.668 | 3.958 / 3.524 | 4.147 / 4.286 | 1.30× | 1.24× |
| 10,000 | 4 running (4) | 4 | 13.952 / 11.398 | 17.215 / 22.727 | 19.089 / 28.670 | 1.22× | 1.20× |
| 100,000 | 10% running (10,003) | 1 | 25.494 / 0.391 | 28.536 / 0.578 | 29.763 / 0.774 | 65.20× | 37.55× |
| 100,000 | 10% running (10,003) | 4 | 98.153 / 1.406 | 127.976 / 4.433 | 175.028 / 5.519 | 69.81× | 45.99× |
| 100,000 | 4 running (4) | 1 | 26.449 / 54.551 | 29.946 / 65.682 | 41.706 / 75.094 | 0.48× | 0.50× |
| 100,000 | 4 running (4) | 4 | 109.780 / 202.642 | 153.494 / 241.707 | 205.536 / 366.415 | 0.54× | 0.55× |

## API timing breakdown — key large case

For 100k rows / `low_running_4` / concurrency 4 (medians, ms):

| Component | Production baseline | Candidate |
|---|---:|---:|
| API wall (includes lock and mapping) | 109.780 | 202.642 |
| Mutex wait | 82.362 | 151.353 |
| SQLite prepare | 0.113 | 0.137 |
| SQLite step | 26.905 | 49.692 |
| C JSON mapping | 0.245 | 0.250 |
| JSON serialization outside API | 0.164 | 0.176 |

Note: component timers overlap conceptually with API wall and should not be summed as independent stages. Mutex wait reflects concurrent workers waiting on the same `Runtime.db_mutex`. `first_call_after_init` is not an OS cold-cache measurement.

## Provenance note

The JSON field `helper_source_sha256` hashes `tests/task_list_c_api_benchmark.c` (the C harness). The hash of `include/db_perf_timing.h` is recorded separately as `timing_header_sha256_supplementary`; that supplemental header hash was attached during final archive review, while benchmark samples and the benchmark-script hash are from the recorded run.
