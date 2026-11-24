# Приоритетный TODO производительности ASGEND — обновлён 2026-10-28

Правило закрытия: каждый performance-пункт требует команды запуска, версии/хеша окружения, размеров корпуса и concurrency, повторов, raw latency distribution, error rate и отдельного критерия качества. Зелёный smoke без нагрузочных измерений пункт не закрывает.

## P1 — обязательные измерения

- [x] **Core RPC/task lifecycle local baseline:** `ping/submit/status/list_tasks/events/answer/cancel/retry_task/submit_to_terminal`, concurrency 1/4/16, пять независимых runs, 100 samples на method/level, p50/p95/p99, error/timeout rate, submit-to-terminal, raw JSON. Результат: 0/2700 ошибок. Ограничение: local Core + deterministic mock LLM, cgroup CPU quota ~2 cores. См. `PERFORMANCE_CORE_RPC_LIFECYCLE_BENCHMARK_2026-10-28.md`.
- [x] **SQLite/artifacts local profile:** корпуса 1k/10k/100k rows; read-heavy/write-heavy, concurrency 1/4, 5 повторов, 15 000 SQLite samples; transaction/`BEGIN IMMEDIATE`/commit latency, `EXPLAIN QUERY PLAN`, index assertions, integrity + rollback atomicity 3/3. Реальный Core TCP `artifact_list` и `workspace_read_bytes`: 0 ошибок; hash/size проверены, cap 1 000 учтён. Это mirrored Python sqlite3 workload плюс локальный Core, не production DB benchmark. См. `PERFORMANCE_AUDIT_STAGE_27_SQLITE_ARTIFACTS_2026-10-28.md` и raw JSON.
- [x] **Task-list SQL/index A/B:** isolated candidate сравнен с реальной C `db_list_tasks()` для всех статусов и LIMIT на 1k/10k/100k; 96 комбинаций, 5 694 строковых объектов C reference — полное совпадение baseline/candidate. Профиль: пять runs, concurrency 1/4, warm-up, raw samples и query plans. Candidate partial indexes в production не перенесены: на concurrency=4 candidate медленнее для 1k/10k и write/storage overhead новых индексов не измерен. Production `db_list_tasks()` получил только явный `id ASC` tertiary tie-break, чтобы сделать порядок строк с одинаковыми priority/effective-priority и `created_at` детерминированным. См. `PERFORMANCE_AUDIT_STAGE_28_TASK_LIST_SQL_AB_2026-10-28.md` и raw JSON.
- [x] **Memory scan/index (Stage 31):** real C scanner, 1k/10k files × `small_text`/`markdown`/`mixed_size` × fresh-index/no-op/one-file change, five repetitions per cell (90/90). All 33,000 fixture hashes and sizes verified; 54-test final CTest and package digests are recorded in the Stage 31 final section below. **Limit:** page cache was not cleared; not cold physical-I/O evidence.
- [ ] **Extractors:** Markdown/text/PDF/OOXML с малым/типовым/крупным входом; files/s, MiB/s, peak RSS, timeout/error distribution.
- [ ] **Embedding pipeline:** worker sweep 1/2/4/8 на deterministic mock, затем отдельный разрешённый provider run; throughput, p95, retry/throttling, DB contention.
- [ ] **ANN quality/performance:** размеченный relevance dataset; sweep corpus sizes/candidate budget; latency + Recall@K/MRR/nDCG. Не вводить CI performance gate до agreed recall target.
- [ ] **Live Control Plane/PostgreSQL:** disposable live stack, concurrency 1/8/32/64, tasks/events/cancel/retry/artifacts; p50/p95/p99, response size, pool wait, SQL plans, row locks, error rate. Mock не засчитывать.

## P2 — client/runtime

- [ ] MCP/LSP: spawn/handshake/discovery/invoke/teardown, cold/warm и worker contention.
- [ ] LLM/SSE: first-token/inter-token/completion, slow reader, cancel/error; provider отдельно.
- [ ] Node Agent: fleet curve, poll/heartbeat, retries/reconnect storm, recovery.
- [ ] Native Android: pinned device/emulator, startup, frame/jank, memory/network.
- [ ] Flutter: profile mode на 100/1,000 tasks, frame/raster, polling, inactive/backoff.
- [ ] Qt: QElapsedTimer/Qt Test для больших task/artifact lists, repaint/model update/memory.
- [ ] CLI/TUI/Web: sustained stream, ANSI redraw, slow consumer и reconnect.

## P2 — governance

- [ ] Для каждого benchmark хранить runner kernel, CPU quota, RAM, compiler/dependencies, commit SHA, corpus hash, command line.
- [ ] На стабильном runner хранить минимум 5 сопоставимых clean runs и все raw samples.
- [ ] CI artifacts + trend comparison; не ставить absolute ms hard-fail без измеренной variance и утверждённого tolerance.

## P3 — оптимизация после профилирования

- [ ] Профилировать `src/memory/embedding.c::find_hit` и graph/chunk dedup на росте candidate set; менять алгоритм только при доказанном ускорении и эквивалентной выдаче.
- [ ] Изменять SQL indexes только на основе `EXPLAIN QUERY PLAN` и representative workload.
- [ ] Менять Flutter polling только после UX/staleness и reconnect анализа.


## Stage 29 — index write/maintenance cost (выполнено локально)

- [x] Candidate partial-index write/storage profile completed: 1k/10k/100k rows, `balanced` and `queued_heavy` distributions, five DB pairs/cell, 100 raw samples per operation/config. Includes list-read, insert batch, status transition, priority update, delete+reinsert, seed insert and index/database storage after WAL checkpoint.
- [x] Raw JSON: `PERFORMANCE_TASK_LIST_INDEX_COST_2026-10-28.json`; full interpretation: `PERFORMANCE_TASK_LIST_INDEX_COST_BENCHMARK_2026-10-28.md`. Candidate indexes remain isolated; no production migration was added.
- [x] Added `performance_task_list_index_cost_smoke`, covering 20 samples per operation and both distributions; p95 is available at 20 samples, p99 remains hidden until 100 samples.
- [x] Final full CTest after harness/integrity changes: **52/52 PASS in 277.32 s, exit 0**; all 52 CTest timeout values are positive and finite. See `PERFORMANCE_STAGE_29_CTEST_LOG_2026-10-28.txt` and `PERFORMANCE_STAGE_29_CTEST_TIMEOUTS_2026-10-28.json`.
- [x] **C API A/B performance (Stage 30):** actual C `db_list_tasks()` vs test-only candidate with mutex wait, prepare, `sqlite3_step`, C JSON mapping and post-API serialization timing. 12 cells, 60 paired runs, 6,000 samples, 0 errors; ordered JSON equal in all 60 pairs. Candidate partial indexes remain out of production because 100k/low-running slows 2.06× at concurrency 1 and 1.85× at concurrency 4. See `PERFORMANCE_AUDIT_STAGE_30_TASK_LIST_C_API_2026-10-28.md` and raw JSON.

## Stage 28 — закрыто в текущем цикле

- [x] Проверены все 32 комбинации status filter/limit на каждом из 1k/10k/100k через прямой C reference `db_list_tasks()`; baseline/candidate полные упорядоченные ответы совпали на 96 комбинациях.
- [x] Candidate SQL использует partial ordered indexes только на disposable БД; их migration/write/storage цена оставлена открытой до отдельного профиля.
- [x] В production SQL добавлен стабильный `id ASC` tertiary tie-break после текущих sort keys; текущий status order, dynamic aging, `created_at ASC` и LIMIT сохранены.
- [x] A/B baseline: median ratio baseline/candidate при concurrency 1 — 1,69× / 7,77× / 49,55× для 1k/10k/100k; при concurrency 4 — 0,87× / 0,91× / 9,67×. Это диагностические SQL timings Python sqlite3, не production C latency; абсолютный CI threshold не вводился.
- [x] `performance_task_list_sql_ab_smoke` зарегистрирован в CTest; C reference helper собирается с реальным `src/db.c`.

## Закрыто в предыдущих циклах

- [x] Свежий повторный полный CTest Stage 26: 49/49 PASS, 250,11 с, exit 0.
- [x] Stage 27 SQLite/artifact profile: 15 000 SQLite samples, 0 SQLite errors, integrity 3/3, rollback atomicity 3/3; actual Core artifact RPC errors 0.
- [x] `EXPLAIN QUERY PLAN` фиксирует `SCAN tasks` + `USE TEMP B-TREE FOR ORDER BY` для полного списка; queued list использует status index, но ещё сортирует через temp B-tree.
- [x] Stage 27: все 50 тестов имели положительный конечный timeout; CTest 50/50 PASS за 255,69 с. Журнал — `PERFORMANCE_STAGE_27_CTEST_LOG_2026-10-28.txt`, timeout metadata — `PERFORMANCE_STAGE_27_CTEST_TIMEOUTS_2026-10-28.json`.
- [x] Stage 28: **51/51 PASS за 277,28 с**, exit 0; все 51 timeout конечные и положительные. Журнал — `PERFORMANCE_STAGE_28_CTEST_LOG_2026-10-28.txt`, timeout metadata — `PERFORMANCE_STAGE_28_CTEST_TIMEOUTS_2026-10-28.json`.
- [x] Release configure/build прошёл; CMake/CTest target добавлен для SQLite/artifacts benchmark smoke.
- [x] 15 независимых Core RPC benchmark run-ов завершены; 2 700 raw samples сохранены.
- [x] Benchmark отчёт не содержит credentials или RPC response bodies; binary/harness hashes и cgroup limits включены.
- [x] Три ревью-итерации проведены и задокументированы; исправлены latency-harness и run metadata.


## Stage 30 — task-list C API A/B (completed locally)

- [x] Instrumented actual C API with benchmark-only timing for `Runtime.db_mutex`, SQLite prepare/step, JSON object mapping and post-API serialization. Production instrumentation stays compiled out; no production partial indexes were added.
- [x] Expanded status profiles to `fixture_10pct_running` and `low_running_4`, across 1k/10k/100k rows and concurrency 1/4: 12 cells, 60 paired runs, 6,000 raw samples, zero errors, exact ordered JSON equivalence in all pairs.
- [x] Measured DB/index bytes after checkpoint + VACUUM and recorded `EXPLAIN QUERY PLAN`, source/binary hashes, compiler/SQLite versions and cgroup caps.
- [x] Full Release-tree CTest: **53/53 PASS in 255.72 s**; all 53 positive finite timeouts confirmed in generated CTest metadata.
- [ ] **Optimize queued-band ordering:** candidate wins heavily when at least 200 running tasks fill the public limit but is 2.06× slower at 100k with four running tasks, concurrency 1. Need an aging-aware strategy plus C-reference equivalence proof before re-evaluation.
- [x] **Memory scan/index profiling (Stage 31):** 90/90 real C-scanner measurements and all fixture hashes verified; see `PERFORMANCE_AUDIT_STAGE_31_MEMORY_SCAN_INDEX_2026-10-28.md`. No cold-I/O claim (page cache left as-is).


## Stage 31 — Memory scan/index (completed locally)

- [x] 90 real C-scanner samples across 18 size/profile/scenario cells, five repeats each; no mock scan implementation.
- [x] Raw JSON renderer recalculates medians/ranges from sample rows; independent verifier confirms 33,000 fixture SHA-256/size records, 56,320,000 bytes, scan ID/file-count semantics and one-file-change detection.
- [x] Timeout regression registered as `performance_memory_scan_timeout_guard`; pre-run registry has 54 tests and every timeout is positive and finite (10–300 s).
- [x] Final post-change Release CTest **54/54 PASS**, zero failures/timeouts/skips, exit code 0, 270.30 s. All 54 timeout values positive and finite (10–300 s); no lingering target process found. Evidence: `PERFORMANCE_STAGE_31_CTEST_LOG_2026-10-28.txt`, `PERFORMANCE_STAGE_31_CTEST_TIMEOUTS_2026-10-28.json`, `PERFORMANCE_STAGE_31_PROCESS_CLEANUP_2026-10-28.json`.
- [x] Stage-specific audit bundle and updated cumulative project archive created; both tar files read successfully, contain no unsafe paths, and their SHA-256 sidecars were verified.
- [ ] Follow-up remains: extractor size/format performance, embedding worker sweep/provider latency, labelled ANN relevance, and live Control Plane/PostgreSQL load.
