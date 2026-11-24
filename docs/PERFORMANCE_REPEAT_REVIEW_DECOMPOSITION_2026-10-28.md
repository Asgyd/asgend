# Декомпозиция повторного ревью производительности ASGEND — 2026-10-28

## Статусы доказательств

- **Измерено локально:** reproducible timings на disposable environment с описанным workload.
- **Проверено функционально/статически:** тесты корректности есть, но они не являются capacity benchmark.
- **Не измерено в целевой среде:** необходимы live service, production-like data, pinned SDK/device или credentials.

## Матрица компонентов

| Подсистема | Свежие доказательства | Что ещё неизвестно | Статус / следующий шаг |
|---|---|---|---|
| Core RPC/task lifecycle | 5 runs × concurrency 1/4/16; 100 samples на 9 операций/уровень; p50/p95/p99; 0/2700 ошибок | Причины latency amplification при concurrency=16; поведение под более крупным DB и remote clients | Local P1 baseline закрыт; далее профилировать DB/CPU |
| Core RPC protocol/contract | RPC inventory/runtime/state-transition; CTest | Конкурентная saturation и реальный сетевой RTT | Функционально проверено + локально измерено |
| SQLite/artifacts | Stage 27: 15k SQLite samples, actual Core artifact RPC, integrity/atomicity. Stage 28: 1k/10k/100k task list, direct C `db_list_tasks()` equivalence, query plans and 5-run A/B at concurrency 1/4 | Live production distribution; write/storage cost of proposed partial indexes; Python SQL timings vs C RPC tail | **Local P1 profiling + SQL correctness gate closed**; candidate partial-index promotion remains blocked pending write/storage and distribution tests |
| Memory scan/index | Stage 31: real C scanner, 90 samples, 33,000 fixture hashes/size checks; fresh index/no-op/one-file change at 1k/10k across three file profiles | Cache was not cleared, so cold physical I/O is unmeasured; production-scale/longer-running workload still unknown | Local profile + 54/54 CTest PASS; cache caveat retained; archives and SHA-256 sidecars verified |
| PDF/OOXML/text extraction | Форматные smoke-тесты | Размерные strata, throughput, RSS, corrupt/timeout distribution | Не измерено; P1 |
| Embedding pipeline | Mock-backed functional tests | Worker sweep 1/2/4/8, retries, throttling и provider latency | Частично; P1 |
| Vector search / ANN | Синтетический exact-vs-ANN baseline, Recall@K и repeated latency | Labelled relevance dataset, sweep corpus/candidate budget, MRR/nDCG | Synthetic only; P1 quality set |
| Graph/chunk/rerank | Functional tests, source inspection | Growth curve для dedup/merge/rerank | Статический риск; P3 после профиля |
| MCP/LSP | Lifecycle/restart tests | Spawn/handshake/discovery/invoke/teardown split; contention curve | Частично; P2 |
| LLM/SSE | Deterministic SSE faults/watchdog | First-token/inter-token/complete latency и backpressure с разрешённым provider | Provider не измерен; P1/P2 |
| Control Plane/HTTP | Mock load harness и security/redaction checks | Live service request distributions и payload sizes | Mock-only; P1 |
| PostgreSQL | Contract/acceptance tests | Live pool wait, SQL plans, row locks, representative data | Не измерено; P1 |
| Node Agent | E2E/contract tests | Fleet-size curve, poll/heartbeat/reconnect storm | Не измерено; P2 |
| Native Android | Source/CI coverage | Pinned emulator/device startup, jank, memory, network-per-active-minute | Не измерено; P2 |
| Flutter | Static checks/contracts | Profile mode 100/1,000 tasks, frame/raster time, polling rate | Не измерено; P2 |
| Qt | Static/client tests | QElapsedTimer/Qt Test paint/model-update/memory profiles | Не измерено; P2 |
| CLI/TUI/Web | Functional/stream tests | Redraw and sustained streaming under slow consumer | Не измерено; P2 |
| Build/CI/packaging | Release configure/build; Stage 26 49/49, Stage 27 50/50, Stage 28 51/51, Stage 29 52/52 CTest; all 52 Stage 29 timeouts finite; write/index-cost smoke registered in CTest | Five clean-build distributions, hosted artifact/run IDs | Локально проверено; P2 |

## Интерпретация свежего Core RPC профиля

- `submit` p99: 4,847 ms at concurrency 1 → 61,539 ms at concurrency 16 (~12,7×).
- `events` p99: 2,211 → 42,141 ms (~19,1×); `answer` p99: 3,781 → 45,878 ms (~12,1×).
- `submit_to_terminal` median: 154 ms / 310 ms / 2 390 ms at concurrency 1/4/16.
- Агрегированный submit-to-terminal throughput: 3,393 / 7,243 / 7,107 tasks/s. Между concurrency 4 и 16 прироста нет, latency растёт резко.
- Все числа относятся к локальному Core + local mock LLM, CPU cgroup quota около 2 cores и memory cap 4 GiB. Они не определяют production bottleneck сами по себе.

## Качество аудита

Три прохода для данного пункта исправили последовательное ожидание состояния, шум ожидаемых socket reset и некорректную отметку времени старта run. Сырые samples остаются доступны в JSON для независимого пересчёта.

## SQLite/artifacts Stage 27 (2026-10-28)

- 15,000 SQLite operations across 1k/10k/100k task rows, read-heavy/write-heavy profiles, concurrency 1/4 and five runs. SQLite errors 0; integrity 3/3; rollback atomicity 3/3.
- Full `task_list_all` query: `SCAN tasks` + temporary B-tree sort. At 100k rows / concurrency 4, median 37.528 ms and p95 96.586 ms in read-heavy profile. `task_list_queued` uses `idx_tasks_queue_priority` but still temp-sorts effective priority.
- Concurrent 100k write-heavy / concurrency 4: transaction median/p95/p99 0.325/3.704/34.264 ms; observed `BEGIN IMMEDIATE` p95 proxy 0.249 ms and COMMIT p95 1.887 ms. These cohorts do not causally explain the tail and should not be summed.
- Artifact operations measured against actual local Core TCP RPC (100/1,000/1,200 files, and 4 KiB/64 KiB/1 MiB reads), no errors; list cap at 1,000 is correct.
- Stage 28 closed the task-list SQL A/B correctness gate; next is write/storage cost measurement for candidate indexes, then memory scan/index profiling.


## Task-list SQL A/B Stage 28 (2026-10-28)

- The actual C `db_list_tasks()` is called through `tests/task_list_reference.c`. For each of 1k/10k/100k task rows, the baseline SQL mirror and isolated candidate were compared against the C output for `all` + seven status filters and four limits (32 cases per DB; 96 total). All ordered public fields matched; 5,694 C output rows were checked per comparison family.
- The production ordering now has explicit `id ASC` tertiary tie-break after status rank/effective priority and `created_at ASC`, removing unspecified ordering for exact ties. Dynamic aging, status priority and LIMIT are unchanged.
- Isolated candidate uses partial indexes for running/other status bands; queued still temp-sorts due dynamic effective priority. At 100k rows it is substantially faster in this synthetic profile, but at concurrency 4 candidate median is slower at 1k and 10k. Do not move candidate indexes into production before write/storage and skewed status distribution are profiled.

## Task-list index write/storage cost — Stage 29 (2026-10-28)

- Candidate partial indexes improve list-read median on large synthetic sets: 33.35× at 100k balanced and 44.82× at 100k queued-heavy.
- Index bytes increase by 4,214,784 (+31.13% of baseline index storage) for balanced 100k and 1,703,936 (+13.09%) for queued-heavy 100k. Whole DB file size increases by about 13.8% and 5.8%, respectively.
- At 100k balanced, seed insertion increases from 1,134.773 to 1,425.393 ms; status transitions +22.94%, priority updates +64.05%, delete+reinsert +43.16% median. Queued-heavy write medians contain sign reversals despite added indexes; raw percentile spread indicates timing/cache sensitivity and should not be interpreted as indexes accelerating writes.
- Five independent DB pairs per size/distribution and 100 samples per operation/config; p95/p99 follow sample-count rules. All 60 post-write integrity/row-count checks passed. Benchmark uses Python sqlite3 and one writer, not the C API or a live production database. Candidate indexes stay in the disposable benchmark only.


## Task-list C API — Stage 30 (2026-10-28)

| Area | Evidence | Finding | Decision / next step |
|---|---|---|---|
| Actual C API latency | `db_list_tasks()` in C with benchmark-only instrumentation; 6,000 raw samples | `db_mutex`, SQL step and mapping measured separately | Keep this separate from Python sqlite3 figures; no hard CI threshold yet |
| High-running distribution | 100k rows, 10,003 running; concurrency 1/4 | Candidate API median 25.494→0.391 ms and 98.153→1.406 ms | Large but distribution-specific win; not generalizable |
| Low-running/large queued | 100k rows, exactly four running; concurrency 1/4 | Candidate 26.449→54.551 ms and 109.780→202.642 ms; throughput about 0.50×/0.55× | Candidate is not production-ready; queued aging order remains bottleneck |
| Index storage | 100k rows, checkpoint + VACUUM + dbstat | Candidate adds 3.90 MB (+13.44% file) or 3.38 MB (+11.65%) | Account for storage and future write/index maintenance before migration |
| Correctness | 60 paired runs / 12 config cells | All ordered JSON outputs equal; 0 errors | Candidate stays test-only; expand correctness to real workload distributions |
| Build/CTest | Release build; 53/53 CTest PASS in 255.72 s | All 53 tests have positive finite timeouts (10–300s) | Local evidence complete; hosted/device tests remain a separate requirement |

The benchmark is synthetic local C API, no OS cache flush and no live CP/database/network traffic. `first_call_after_init` is not called cold-cache. External subagent execution is unavailable; three critical review passes were recorded.


## Memory scan/index — Stage 31 (2026-10-28)

- Real Release C `asgend-memory scan ROOT`, not a mock: 90 timed samples (2 sizes × 3 content profiles × 3 scenarios × 5 repetitions). Each result includes wall time, files/s, normalized logical MiB/s and peak RSS via Linux `VmHWM`.
- Full scan of 10,000 files: median 10.649 s (`small_text`), 11.111 s (`markdown`) and 11.177 s (`mixed_size`). No-op medians: 3.009 s, 3.207 s and 3.344 s respectively. One-file-change medians: 3.722 s, 3.011 s and 3.892 s respectively. Read medians together with the raw ranges in `PERFORMANCE_MEMORY_SCAN_INDEX_BENCHMARK_2026-10-28.md`; do not infer a cache-independent ranking from the small spread.
- All 33,000 fixture files were independently rehashed and size-checked: 56,320,000 source bytes; metadata validated for all runs; one-file changes each reported exactly one modified file.
- No page-cache drop was attempted: results are ambient-cache process timings, not cold physical I/O. Logical MiB/s is corpus-bytes / wall time, not disk bandwidth. No production scan/index algorithm was changed.
- Timeout process-tree regression is registered in CTest. Final Release CTest: 54/54 PASS, 0 failures, exit 0 in 270.30 s; all timeout values positive/finite and no targeted process remained. Stage-specific deliverable: `asgend-performance-memory-scan-index-audit-stage-31.tar.gz`; updated cumulative source: `asgend-performance-audit-cumulative-stage-31.tar.gz`. Their adjacent `.sha256` sidecars were verified after a final archive rebuild.
