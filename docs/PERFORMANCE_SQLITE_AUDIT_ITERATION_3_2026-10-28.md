# Performance audit iteration 3 — independent critical review of evidence

## Review checks

- Re-ran smoke after final profiler edits and ensured it exercises real Core artifact RPC rather than only filesystem helper code.
- Verified `artifact_list` limit 1,000 at 1,200 files and that response metadata hashes correspond to the file bytes.
- Verified query-plan assertion failures are fatal for required indexes; full-task-list sort cost is recorded as a finding rather than hidden as test success.
- Verified the benchmark stores no token or artifact contents, and attaches SHA-256 for Core binary, `src/db.c`, `src/artifacts.c`, and the profiler source.
- Confirmed small-sample p95/p99 follow shared metrics policy and no raw p99 is fabricated for fewer than 100 samples.

## Final result

The final full benchmark is the JSON sidecar. SQLite error count is zero; artifact RPC error count is zero; integrity check and atomicity checks pass at all three database sizes. No production SQL/index change was performed because a proposed list optimization must first pass exact ordered-result equivalence across priority-aging, ties, and all statuses. The final Release tree passed the complete CTest suite **50/50 in 255.69 seconds (exit 0)**. The SQLite/artifact smoke is test #8 and passed in 1.75 seconds; all 50 tests have finite positive configured timeouts (10–300 seconds), recorded in `PERFORMANCE_STAGE_27_CTEST_TIMEOUTS_2026-10-28.json`.

An external subagent runtime is not available in this environment, so no external-agent score is claimed; three separate critical review passes were performed and the identified measurement issues were corrected before final measurements were retained.
