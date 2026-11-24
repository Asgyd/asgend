# Iteration 1 — actual C API baseline and timing instrumentation (2026-10-28)

## Объём

Added an isolated C benchmark target compiled from the actual `src/db.c`, keeping production instrumentation disabled. Baseline and candidate are invoked through C functions, not inferred from a Python SQL mirror. Instrumentation splits mutex wait, SQLite prepare, `sqlite3_step`, C-to-json-c mapping, and external serialization.

## Evidence

- 3 sizes × 2 concurrency levels × 5 paired runs = 30 paired runs for the initial distribution.
- Exact ordered JSON result matching in each paired run; raw JSON and SHA-256 metadata included.
- First pass showed a very large candidate gain at 100k rows; this is only a preliminary signal because status skew could dominate a limit-200 API.

## Critical note

The initial result was not accepted as production evidence. The first status distribution could permit the candidate to return 200 running tasks without reading the queued band. Iteration 2 expanded the benchmark with only four running tasks and requires the candidate to merge/scan queued rows.
