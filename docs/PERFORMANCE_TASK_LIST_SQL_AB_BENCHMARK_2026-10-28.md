# Task-list SQL A/B — подробный benchmark отчёт

## Команда

```bash
python3 scripts/benchmark_task_list_ab.py \
  --sizes 1000,10000,100000 --runs 5 \
  --operations-per-worker 10 --concurrencies 1,4 \
  --limit 200 --aging-seconds 300 \
  --reference-binary build/test-task-list-reference \
  --output docs/PERFORMANCE_TASK_LIST_SQL_AB_2026-10-28.json
```

Команда выше описывает актуальные параметры; в итоговом архиве JSON содержит фактические параметры, timestamps, environment, hashes и все raw latency samples.

## Что именно измеряется

Обе SQL-стратегии выполняются через Python `sqlite3` на одной disposable базе: это изолированное сравнение форм запросов и планов. Перед измерением каждый worker выполняет один warm-up query, затем стартует через barrier. Baseline и candidate чередуют порядок между независимыми runs. Измеренная latency включает вызов Python и SQLite statement execution, но не C JSON serialization или TCP/RPC overhead.

- **Baseline:** один SELECT по всей таблице, сортировка `status rank`, `effective_priority DESC`, `created_at ASC`, `id ASC`, затем `LIMIT`.
- **Candidate:** sequential top-N по status bands: `running`, `queued`, остальные; каждый band запрашивает только оставшийся LIMIT. Для `running` и остальных статусов используются две partial indexes только в benchmark DB. Queued tasks сортируются по динамическому effective priority.
- **Reference gate:** каждый query result baseline и candidate сравнивается с фактическим JSON от C функции `db_list_tasks()` в `test-task-list-reference`. На каждом размере — 32 filter/limit комбинации; 96 в целом. Всего с C reference сопоставлено 5 694 row objects, без расхождений.

## Результаты агрегации

| Rows | Concurrency | Samples/method | Baseline median (ms) | Candidate median (ms) | Baseline p95/p99 (ms) | Candidate p95/p99 (ms) | Median ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 1 | 50 | 1.454 | 0.860 | 3.573 / — | 1.147 / — | 1.691× |
| 1,000 | 4 | 200 | 7.301 | 8.353 | 23.081 / 28.080 | 22.501 / 31.743 | 0.874× |
| 10,000 | 1 | 50 | 4.203 | 0.541 | 5.634 / — | 0.714 / — | 7.769× |
| 10,000 | 4 | 200 | 9.327 | 10.238 | 27.617 / 36.871 | 29.051 / 37.776 | 0.911× |
| 100,000 | 1 | 50 | 28.738 | 0.580 | 32.414 / — | 1.105 / — | 49.548× |
| 100,000 | 4 | 200 | 77.728 | 8.038 | 101.028 / 124.609 | 27.814 / 36.445 | 9.670× |

Tail percentiles are omitted when aggregate samples are below their minimum (p95 ≥20; p99 ≥100). At concurrency 1, five runs × ten operations produce 50 samples; p99 is intentionally omitted.

## Correctness and determinism

- Each status filter (`queued`, `running`, `waiting_input`, `completed`, `completed_without_model`, `failed`, `cancelled`) and `all` is checked at limits 1, 10, 50 and 200 (or size for smaller fixture sizes).
- Fixture contains identical priorities and timestamps, repeated effective priority aging buckets, younger/older queued entries and all statuses.
- The direct C reference comparison passed all 96 combinations. The C SQL previously had no explicit tertiary tie-break for rows with equal `effective_priority` and `created_at`; Stage 28 now appends `id ASC` to make this order deterministic across query plans.
- There is no candidate-index schema migration in production.

## Interpretation / limits

Candidate provides a large win for 100k because the fixture has at least LIMIT running rows. This is a best case for banded retrieval. At small corpus sizes and four concurrent clients, candidate median can be slower because it performs multiple bounded queries and the Python/SQLite overhead competes for two cgroup CPU cores. The workload is intentionally deterministic and diagnostic; live status distributions, task writers, SQLite mutex contention, C JSON conversion and service response overhead still need profiling.

Do not promote the indexes until write-heavy throughput, index/storage overhead, cold-cache behavior and several representative status distributions are measured. This benchmark does not qualify an absolute CI latency threshold.
