# SQLite и artifacts benchmark — Stage 27

Профиль запуска: `scripts/benchmark_sqlite_artifacts.py /path/to/agent-orchestrator . --output docs/PERFORMANCE_SQLITE_ARTIFACTS_BENCHMARK_2026-10-28.json`.

## Методика

- SQLite 3.46.1, WAL + `synchronous=FULL` + foreign keys + 5s busy timeout; schema/indexes и hot SQL shape зеркалируют `src/db.c`.
- Размеры: 1 000 / 10 000 / 100 000 task rows, плюс по одному исходному event на task и 6 session messages на session.
- Профили: read-heavy 90% read / 10% write; write-heavy 20% read / 80% write. Concurrency 1 и 4, пять повторов, 100 операций на worker/run, пять warm-up reads на worker.
- Латентность операции — wall time отдельного вызова; throughput — successful operations / total wall time профилей, warmup и подготовка соединений исключены из замеряемого интервала.
- `BEGIN IMMEDIATE` elapsed — proxy lock-acquisition wait; Python stdlib sqlite3 не предоставляет native busy-handler counters. SQLite exceptions считаются errors.
- Каждый workload включает task-by-id, list all, list queued, event tail, session context и write transaction (update task + insert event).
- После нагрузки проверяются row counts, `PRAGMA integrity_check`, rollback atomicity через injected trigger. Main DB file size измеряется после `wal_checkpoint(TRUNCATE)`.
- Artifact operations идут через реальный Core TCP RPC: `artifact_list` на 100/1 000/1 200 файлов и `workspace_read_bytes` для 4 KiB/64 KiB/1 MiB; содержимое читаемых файлов верифицируется SHA-256.

## Размеры данных, seed time и SQLite file

| Task rows | Seed (с) | SQLite main file после checkpoint (MiB) | Event rows после workload | Integrity | Atomicity |
|---:|---:|---:|---:|---|---|
| 1,000 | 0.016 | 0.54 | 3,209 | ok | PASS |
| 10,000 | 0.166 | 4.87 | 12,267 | ok | PASS |
| 100,000 | 1.889 | 40.55 | 102,258 | ok | PASS |

## SQLite workload latency и throughput

`Latency` — median / p95 / p99 в ms; значения хвостовых percentiles автоматически не публикуются, когда samples < policy minimum. Throughput — ops/s, измеренный в окне выполнения параллельных workers.

| Rows | Profile | Concurrency | Total latency median/p95/p99 (ms) | `task_list_all` median/p95 (ms) | `task_list_queued` median/p95 (ms) | `write_transaction` median/p95/p99 (ms) | Throughput (ops/s) | Errors |
|---:|---|---:|---|---|---|---|---:|---:|
| 1,000 | read_heavy | 1 | 0.026 / 0.620 / 1.001 | 0.401 / 0.750 | 0.215 / 0.365 | 0.469 / 0.783 / — | 5102.0 | 0 |
| 1,000 | read_heavy | 4 | 0.136 / 2.142 / 4.589 | 0.585 / 2.721 | 0.379 / 1.909 | 0.935 / 8.909 / 19.541 | 6211.2 | 0 |
| 1,000 | write_heavy | 1 | 0.186 / 0.547 / 0.863 | 0.473 / — | 0.282 / — | 0.198 / 0.542 / 1.285 | 4201.7 | 0 |
| 1,000 | write_heavy | 4 | 0.223 / 0.556 / 8.780 | 0.445 / 0.826 | 0.252 / 0.463 | 0.227 / 0.551 / 19.017 | 3472.2 | 0 |
| 10,000 | read_heavy | 1 | 0.032 / 2.293 / 3.599 | 2.124 / 3.599 | 0.669 / 1.010 | 0.509 / 1.112 / — | 1592.4 | 0 |
| 10,000 | read_heavy | 4 | 0.118 / 5.579 / 11.923 | 3.886 / 9.574 | 1.193 / 4.050 | 0.658 / 4.982 / 52.161 | 2401.0 | 0 |
| 10,000 | write_heavy | 1 | 0.213 / 0.895 / 2.942 | 2.729 / — | 0.766 / — | 0.221 / 0.579 / 1.866 | 2824.9 | 0 |
| 10,000 | write_heavy | 4 | 0.221 / 3.044 / 18.785 | 2.854 / 9.982 | 0.827 / 3.754 | 0.226 / 2.207 / 19.373 | 2317.5 | 0 |
| 100,000 | read_heavy | 1 | 0.050 / 22.223 / 23.929 | 21.616 / 23.977 | 5.138 / 7.504 | 0.700 / 1.229 / — | 192.6 | 0 |
| 100,000 | read_heavy | 4 | 0.098 / 62.482 / 95.682 | 37.528 / 96.586 | 8.643 / 43.132 | 0.800 / 6.229 / 33.540 | 285.3 | 0 |
| 100,000 | write_heavy | 1 | 0.280 / 6.811 / 27.374 | 25.279 / — | 5.918 / — | 0.292 / 0.965 / 2.216 | 656.2 | 0 |
| 100,000 | write_heavy | 4 | 0.319 / 20.173 / 42.634 | 33.001 / 56.883 | 7.806 / 19.640 | 0.325 / 3.704 / 34.264 | 966.7 | 0 |

## Lock wait / commit distributions

| Rows | Profile | Concurrency | `BEGIN IMMEDIATE` wait median/p95 (ms) | COMMIT median/p95 (ms) |
|---:|---|---:|---|---|
| 1,000 | read_heavy | 1 | 0.003 / 0.007 | 0.447 / 0.729 |
| 1,000 | read_heavy | 4 | 0.014 / 8.300 | 0.461 / 1.448 |
| 1,000 | write_heavy | 1 | 0.003 / 0.006 | 0.180 / 0.499 |
| 1,000 | write_heavy | 4 | 0.003 / 0.007 | 0.203 / 0.425 |
| 10,000 | read_heavy | 1 | 0.004 / 0.008 | 0.478 / 1.066 |
| 10,000 | read_heavy | 4 | 0.006 / 3.341 | 0.521 / 2.815 |
| 10,000 | write_heavy | 1 | 0.003 / 0.006 | 0.198 / 0.540 |
| 10,000 | write_heavy | 4 | 0.004 / 0.035 | 0.196 / 0.514 |
| 100,000 | read_heavy | 1 | 0.006 / 0.017 | 0.644 / 1.161 |
| 100,000 | read_heavy | 4 | 0.011 / 0.228 | 0.707 / 6.085 |
| 100,000 | write_heavy | 1 | 0.004 / 0.013 | 0.253 / 0.881 |
| 100,000 | write_heavy | 4 | 0.005 / 0.249 | 0.284 / 1.887 |

## EXPLAIN QUERY PLAN (100k rows; same plan shape observed at 1k/10k)

| Query | Plan | Assertion/interpretation |
|---|---|---|
| `session_context` | `CO-ROUTINE (subquery-1); SEARCH session_messages USING INDEX idx_session_messages (session_id=?); SCAN (subquery-1); USE TEMP B-TREE FOR ORDER BY` | PASS — `idx_session_messages` used; bounded context subquery then temp sort |
| `task_by_primary_key` | `SEARCH tasks USING INDEX sqlite_autoindex_tasks_1 (id=?)` | PASS — primary-key index used |
| `task_events_tail` | `SEARCH task_events USING INDEX idx_task_events_task (task_id=? AND event_id>?)` | PASS — `idx_task_events_task` used |
| `task_list_all` | `SCAN tasks; USE TEMP B-TREE FOR ORDER BY` | HOTSPOT — full table scan + temporary B-tree sort |
| `task_list_queued` | `SEARCH tasks USING INDEX idx_tasks_queue_priority (status=?); USE TEMP B-TREE FOR ORDER BY` | Index used for status, but effective-priority ORDER BY still uses a temporary B-tree |

## Real Core artifact RPC

| Operation | Input size | Concurrency | Samples | Median (ms) | p95 (ms) | Aggregate MiB/s | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| `artifact_list` | 100 files (100 returned; truncated=false) | 1 | 5 | 0.858 | — | — | 0 |
| `artifact_list` | 100 files (100 returned; truncated=false) | 4 | 20 | 1.774 | 3.643 | — | 0 |
| `artifact_list` | 1000 files (1000 returned; truncated=false) | 1 | 5 | 10.372 | — | — | 0 |
| `artifact_list` | 1000 files (1000 returned; truncated=false) | 4 | 20 | 13.962 | 20.344 | — | 0 |
| `artifact_list` | 1200 files (1000 returned; truncated=true) | 1 | 5 | 13.399 | — | — | 0 |
| `artifact_list` | 1200 files (1000 returned; truncated=true) | 4 | 20 | 12.251 | 16.376 | — | 0 |
| `workspace_read_bytes` | 4,096 bytes | 1 | 5 | 0.319 | — | 5.502 | 0 |
| `workspace_read_bytes` | 4,096 bytes | 4 | 20 | 0.599 | 1.891 | 9.587 | 0 |
| `workspace_read_bytes` | 65,536 bytes | 1 | 5 | 0.589 | — | 61.566 | 0 |
| `workspace_read_bytes` | 65,536 bytes | 4 | 20 | 1.475 | 2.231 | 84.821 | 0 |
| `workspace_read_bytes` | 1,048,576 bytes | 1 | 5 | 9.650 | — | 76.089 | 0 |
| `workspace_read_bytes` | 1,048,576 bytes | 4 | 20 | 26.764 | 40.577 | 94.263 | 0 |

## Query-plan observations and recommendation

- `task_list_all` evaluates a CASE-based status/priority order expression and sorts results via a temporary B-tree after scanning tasks. Query-list median at 100k grows from about 0.4 ms (1k rows) to roughly 22 ms (100k rows) for concurrency=1; with concurrency=4 the tail exceeds 100 ms.
- `task_list_queued` uses `idx_tasks_queue_priority` to narrow on status, but computed effective priority still causes temporary sorting. The index alone does not remove sort cost.
- Event tail and session context use their expected lookup indexes; no index should be added to those paths based on this profile.
- Concurrent writes have zero SQLite errors, but p99/maximum transaction latency contains occasional spikes. The measured `BEGIN IMMEDIATE` wait and `COMMIT` distributions are separate cohorts and do not form a causal decomposition; use a lower-level SQLite profiler/checkpoint trace before a durability/index change.
- Artifact listing reaches the explicit 1,000 item cap when the workspace contains 1,200 files; the reply sets `truncated=true`. This is contract behavior, not necessarily a performance bug.
- The read-throughput metric includes concurrent Core RPC wall time and client decode/hash verification; do not compare it to raw storage bandwidth.

## Reproducibility metadata

- Start: `2026-10-28T01:49:56.051242+00:00`; finish: `2026-10-28T01:50:15.674198+00:00`; elapsed: 19.623 s.
- Platform: `Linux-6.18.55-x86_64-with-glibc2.41`; GCC: `cc (Debian 14.2.0-19) 14.2.0`; CMake: `cmake version 3.31.6`; SQLite `3.46.1`; Python `3.13.5`.
- Cgroup limit: 2.0 CPU cores, 4 GiB RAM.
- Raw data and SHA-256 hashes of the Core binary, `src/db.c`, `src/artifacts.c`, and benchmark script are saved in the sibling JSON report.
- Workload uses Python sqlite3 against a schema and SQL shapes mirrored from `src/db.c`; it does not include C mutex or JSON encoder cost. Artifact operations use the real Core TCP API on a disposable local workspace. No production server, provider, or credentials were used.
