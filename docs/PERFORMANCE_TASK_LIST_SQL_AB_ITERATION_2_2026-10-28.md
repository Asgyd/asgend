# Stage 28 — Iteration 2: direct C reference gate

- Added `tests/task_list_reference.c`, built from the real `src/db.c`, which opens each populated SQLite fixture and calls `db_list_tasks()` for all status filters and LIMIT values.
- Initial direct comparison found tied `queued` rows could be ordered differently even when the first returned rows matched. The current SQL had not specified a tertiary tie-break, so this was a real determinism gap rather than an A/B algorithm failure.
- Added an explicit `id ASC` tertiary order after `created_at ASC` in both `sql_all` and `sql_filter`, preserving status grouping, effective priority/aging, and LIMIT. The candidate indexes remain isolated.
- With current source compiled, direct C comparisons passed 32 combinations for each of 1k, 10k and 100k rows (96 combinations total) and matched all 5,694 returned C-reference row objects.
