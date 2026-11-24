# Iteration 3 — adversarial review, correctness and final CI verification (2026-10-28)

## Review adjustments

- Confirmed the benchmark actually calls the C `db_list_tasks()` function and the separately compiled test-only candidate; not Python SQL latency.
- Ensured candidate indexes exist only in candidate fixture and are not added to production schema.
- Recorded actual SQLite EXPLAIN plans, mutex wait, prepare, step, C mapping, JSON serialization, dbstat size and hashes.
- Distinguished first call after `db_init` from OS cold-cache; OS cache is not flushed.
- Kept the 200-row API cap explicit and challenged the result with low-running/queued-heavy workload.
- Checked final raw JSON: 12 cells, 60 paired runs, 6,000 raw samples, 0 errors, ordered JSON equal in all paired runs.

## Final verification

Release configure/build passed. Full CTest passed **53/53 in 255.72 seconds**, and generated test metadata contains finite positive timeouts for all 53 tests (10–300 s). The C API smoke is included in that full run.

## Conclusion

Do not promote partial indexes; the candidate is workload dependent and regresses substantially when only four tasks are running. An external subagent was not launched because no subagent runner is available in this environment; this is a critical review iteration, not an independent agent score.
