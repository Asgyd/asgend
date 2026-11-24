# Performance audit iteration 1 — SQLite/artifacts schema and query-plan review

## Scope

Compared `src/db.c` tables/indexes and high-frequency query shapes against the performance profiler. Focused on task primary-key lookups, list-all/list-queued, task event pagination, session context, and artifact-list/read RPC.

## Result

- Confirmed lookup-index use for `tasks.id`, `task_events(task_id,event_id)`, and `session_messages(session_id,message_id)`.
- Found a P1 candidate: `task_list_all` does a table scan and temp B-tree sort. `task_list_queued` finds candidates via `idx_tasks_queue_priority` but also temp-sorts by dynamic effective priority.
- Added mandatory checks for DB row counts, injected rollback atomicity and `PRAGMA integrity_check` at all three cardinalities.
- No production SQL/index was changed: plan findings require an equivalence-safe A/B optimization instead of an unproven index.

## Files

`scripts/benchmark_sqlite_artifacts.py`, `tests/test_benchmark_sqlite_artifacts.py`, and `CMakeLists.txt` contain profiler and smoke registration. Detailed plan output is in `PERFORMANCE_SQLITE_ARTIFACTS_BENCHMARK_2026-10-28.json`.
