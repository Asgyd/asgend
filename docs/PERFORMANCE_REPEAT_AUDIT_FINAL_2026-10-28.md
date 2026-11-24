# Итоговый повторный аудит производительности ASGEND — 2026-10-28

## Статус текущего пункта — Stage 30

Stage 27 закрыл SQLite/artifacts baseline. Stage 28 добавил изолированный A/B benchmark `scripts/benchmark_task_list_ab.py`, C reference harness `tests/task_list_reference.c` и CTest `performance_task_list_sql_ab_smoke`. Полный Release-tree CTest Stage 28 прошёл **51/51 PASS за 277,28 с**, код завершения 0; все 51 тест имеют конечный положительный timeout. Candidate partial indexes пока остаются только в benchmark DB.

## Проверенные результаты Stage 26 — Core RPC baseline

- Полный CTest Stage 26: **49/49 PASS**, 250,11 с, код завершения 0. Stage 27 после добавления SQLite/artifact smoke зарегистрировал 50 тестов и прошёл 50/50.
- Все 50 зарегистрированных тестов Stage 27 имеют конечный положительный timeout по generated CTest metadata.
- Stage 26 Release configure/build прошли; для Stage 27 свежий configure/build также завершился успешно. Эти времена сборки не трактуются как производственный performance SLA.
- RPC load benchmark: 5 независимых runs × concurrency 1/4/16 × 20 samples/method/run; 100 samples на каждый метод/уровень; 9 методов и **2 700 raw samples**; 0 ошибок.
- Окружение: Linux x86-64, GCC 14.2.0, CMake 3.31.6, Python 3.13.5; cgroup CPU quota около 2 cores, RAM cgroup limit 4 GiB.

## Главные выводы по Core RPC

1. При concurrency 16 растут хвостовые задержки RPC: `submit` p99 4,847 → 61,539 ms, `events` 2,211 → 42,141 ms, `answer` 3,781 → 45,878 ms между concurrency 1 и 16.
2. `submit_to_terminal` median: 154 ms / 310 ms / 2 390 ms на concurrency 1/4/16. Агрегированный throughput: 3,393 / 7,243 / 7,107 tasks/s. На этом профиле throughput почти не увеличивается при переходе с 4 на 16 клиентов, при резком росте latency.
3. Ноль ошибок в тестовом workload; все prepared answer/cancel/retry transitions завершились ожидаемым состоянием.
4. Это указывает на saturation при данном локальном workload, но не локализует первопричину. Нужны профили CPU/SQLite lock contention/query plans; код оптимизировать до измерения конкретного hotspot не следует.

## Границы доказательств

Используется Core TCP server с локальным deterministic mock LLM и disposable SQLite, без remote provider, live Control Plane/PostgreSQL и реальных Android/Flutter/Qt устройств. `submit_to_terminal` включает polling. Результат не является production capacity certification и не обосновывает абсолютный ms CI gate.

## Три итерации ревью

- Итерация 1 нашла искусственную задержку от последовательного ожидания завершения задач и noisy socket reset tracebacks при cancel.
- Итерация 2 ввела concurrent bounded polling, точные проверки терминальных состояний и redaction raw report.
- Итерация 3 исправила timestamp run start, включила compiler/CMake/cgroup metadata и повторила полный набор на окончательной версии harness. Полный CTest после интеграции прошёл 49/49.

Task-list SQL A/B Stage 28 завершён с прямым C reference: 96 комбинаций status-filter/limit на 1k/10k/100k прошли полное сравнение baseline и candidate с `db_list_tasks()`. В production `db_list_tasks()` добавлен явный `id ASC` tertiary tie-break, чтобы результат на точных ties не зависел от query plan; dynamic aging, status ordering, created_at и LIMIT сохранены. Candidate partial indexes не перенесены в production, поскольку при concurrency=4 для размеров 1k/10k ускорения не дали и write/storage цена не измерена.

## Артефакты

- Детальные таблицы: `PERFORMANCE_CORE_RPC_LIFECYCLE_BENCHMARK_2026-10-28.md`.
- Raw measurements: `PERFORMANCE_CORE_RPC_LIFECYCLE_BENCHMARK_2026-10-28.json`.
- Декомпозиция компонентов: `PERFORMANCE_REPEAT_REVIEW_DECOMPOSITION_2026-10-28.md`.
- Следующие задачи: `PERFORMANCE_REPEAT_TODO_2026-10-28.md`.
- Три отчёта: `PERFORMANCE_RPC_AUDIT_ITERATION_{1,2,3}_2026-10-28.md`.
- Test log: `PERFORMANCE_REPEAT_CTEST_LOG_2026-10-28.txt`.

## Stage 27 — SQLite/artifacts profiling (2026-10-28)

- Added `scripts/benchmark_sqlite_artifacts.py` and CTest smoke `performance_sqlite_artifacts_smoke`. Full local profile: 15,000 SQLite operations; 1k/10k/100k rows; read-heavy/write-heavy; concurrency 1/4; five repeats. SQLite error count 0, integrity and rollback checks 3/3.
- Actual Core TCP artifact operations (`artifact_list` and `workspace_read_bytes`) completed with zero errors; artifact hashes/sizes were checked and the 1,000-item cap behaved as specified.
- Fresh findings: `task_list_all` scans all tasks and uses a temporary B-tree for order. At 100k rows/concurrency 4 it measured 37.528 ms median and 96.586 ms p95 in read-heavy workload. The queued list uses `idx_tasks_queue_priority` for status filtering but still temp-sorts effective priority.
- 100k write-heavy / concurrency 4 transaction median/p95/p99 was 0.325/3.704/34.264 ms; `BEGIN IMMEDIATE` wait proxy p95 0.249 ms and COMMIT p95 1.887 ms. Not a causal breakdown; no SQLite errors observed.
- SQLite microbenchmark uses Python sqlite3 with schema/hot SQL mirrored from `src/db.c`; it does not include C mutex/serialization. Artifact half uses real local Core TCP RPC. These are local diagnostic measures, not production SLOs.
- На момент закрытия Stage 27 следующий P1 был task-list SQL A/B; он выполнен в Stage 28, см. раздел ниже.
- Full Release-tree CTest after Stage 27: **50/50 PASS in 255.69 s, exit code 0**; test #8 `performance_sqlite_artifacts_smoke` passed in 1.75 s. All 50 tests have a positive finite configured timeout from 10 to 300 seconds.
- Stage 27 CTest evidence: `PERFORMANCE_STAGE_27_CTEST_LOG_2026-10-28.txt`, `PERFORMANCE_STAGE_27_CTEST_TIMEOUTS_2026-10-28.json`, and `PERFORMANCE_STAGE_27_CTEST_SHOW_ONLY_2026-10-28.json`.


## Stage 28 — Task-list SQL A/B (2026-10-28)

- Direct C reference calls actual `db_list_tasks()` on the same populated SQLite fixture. Baseline and candidate ordered public rows match for all/each status and limits 1/10/50/200 on each 1k/10k/100k database (96 combinations total; 5,694 reference row objects).
- Only production behavior change is an explicit `id ASC` tertiary tie-break after current ordering keys. The optimized multi-band candidate and its two partial indexes remain isolated.
- Measured ratio baseline median / candidate median: concurrency 1 = 1.69× / 7.77× / 49.55× at 1k/10k/100k; concurrency 4 = 0.87× / 0.91× / 9.67×. Candidate is workload-dependent; do not promote partial indexes before write/storage and status-skew measurements. Python sqlite3 benchmark latency is not equivalent to C RPC latency.
- Detailed method/results: `PERFORMANCE_AUDIT_STAGE_28_TASK_LIST_SQL_AB_2026-10-28.md`, raw samples: `PERFORMANCE_TASK_LIST_SQL_AB_2026-10-28.json`. Three review iterations: `PERFORMANCE_TASK_LIST_SQL_AB_ITERATION_{1,2,3}_2026-10-28.md`.

- Полный CTest после Stage 28: **51/51 PASS in 277.28 s, exit code 0**; `performance_task_list_sql_ab_smoke` passed in 2.66 s. Все 51 timeout подтверждены generated CTest metadata.
- Stage 28 CTest evidence: `PERFORMANCE_STAGE_28_CTEST_LOG_2026-10-28.txt` и `PERFORMANCE_STAGE_28_CTEST_TIMEOUTS_2026-10-28.json`. Generated metadata подтвердили 51/51 positive finite timeouts.


## Stage 29 — Task-list index write/storage cost (2026-10-28)

- Added `scripts/benchmark_task_list_index_cost.py` and CTest `performance_task_list_index_cost_smoke`. Profile: 1k/10k/100k rows; balanced and queued-heavy distribution; five independent DB pairs per cell; 20 samples per operation/run, resulting in 100 raw samples per operation/config. Operations: ordered list read, insert batch, status transition, priority update, delete+reinsert; seed insert and index bytes are also captured.
- Environment: Python 3.13.5, SQLite 3.46.1, Linux x86-64, cgroup CPU quota 2 cores, memory cap 4 GiB. Final harness hash and `src/db.c` hash are saved in JSON.
- At 100k balanced: list read 29.880 → 0.896 ms median (33.35×); extra index bytes 4,214,784 (+31.13% over existing index bytes); seed insertion 1,200.034 → 1,431.076 ms; status transition +29.36%, priority update +73.50%, insert batch +24.74%, delete+reinsert +45.35% median.
- At 100k queued-heavy: list read 39.309 → 0.877 ms (44.82×); extra index bytes 1,703,936 (+13.09%); seed insertion 1,092.402 → 1,227.144 ms. Some write medians reverse direction under queued-heavy, so results show timing/cache sensitivity and do not support promoting partial indexes based on median alone.
- Five paired result-equivalence checks per size/distribution (30 total) passed. After each write workload, 60/60 `PRAGMA integrity_check` and task row-count invariants passed. Candidate partial indexes remain disposable only; no production schema migration was added. The profile is a Python sqlite3 mirror with one writer and does not measure C mutex, JSON serialization or Core TCP latency.
- Full CTest after Stage 29 final harness: **52/52 PASS in 277.32 s, exit code 0**; `performance_task_list_index_cost_smoke` passed in 2.49 s. Generated CTest metadata confirms 52/52 positive finite timeouts.
- Raw benchmark: `PERFORMANCE_TASK_LIST_INDEX_COST_2026-10-28.json`; analysis: `PERFORMANCE_TASK_LIST_INDEX_COST_BENCHMARK_2026-10-28.md`; three iterative reports: `PERFORMANCE_TASK_LIST_INDEX_COST_ITERATION_{1,2,3}_2026-10-28.md`.
- Next P1: actual C API timing/lock-wait decomposition, then memory scan/index profile; do not promote candidate indexes yet.


## Stage 30 — Task-list C API A/B and timing decomposition (2026-10-28)

- Added a dedicated C API benchmark target compiled from the actual `src/db.c`; production instrumentation stays off and candidate partial indexes exist only in the test fixture. Timing separates API wall time, `Runtime.db_mutex` wait, SQL prepare, `sqlite3_step`, C JSON mapping and post-API serialization.
- Re-profiled two task-status distributions (`fixture_10pct_running` with 10,003 running tasks at 100k rows and `low_running_4` with exactly four) across 1k/10k/100k, concurrency 1/4. 12 cells, 60 paired runs, 6,000 samples, 0 errors; all paired ordered JSON responses match.
- In the 10% running fixture, 100k API median improves 65.20×/69.81× (concurrency 1/4) because at least 200 running tasks fill the public limit. In the more demanding four-running fixture the candidate regresses to 54.551 vs 26.449 ms (1 client) and 202.642 vs 109.780 ms (4 clients). This is decisive evidence not to promote candidate indexes yet.
- Candidate extra DB bytes after checkpoint+VACUUM at 100k: 3,895,296 bytes for the 10% running fixture (+13.44% full DB size) and 3,375,104 bytes for four running tasks (+11.65%). Queued-band still uses a temporary B-tree for computed aging order.
- Final Release build and full CTest: **53/53 PASS in 255.72 s**, exit code 0; generated metadata confirms 53 positive finite test timeouts. Full log: `PERFORMANCE_STAGE_30_CTEST_LOG_2026-10-28.txt`; metadata: `PERFORMANCE_STAGE_30_CTEST_TIMEOUTS_2026-10-28.json`.
- Reproducible raw results and hashes: `PERFORMANCE_TASK_LIST_C_API_BENCHMARK_2026-10-28.json`; analysis: `PERFORMANCE_AUDIT_STAGE_30_TASK_LIST_C_API_2026-10-28.md`; three iterations: `PERFORMANCE_TASK_LIST_C_API_ITERATION_{1,2,3}_2026-10-28.md`.
- Next P1: design and prove an aging-aware queued-list optimization; then memory scan/index profiling. Candidate partial indexes remain benchmark-only.
