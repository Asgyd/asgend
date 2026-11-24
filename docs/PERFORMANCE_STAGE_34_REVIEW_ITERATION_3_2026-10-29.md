# Stage 34 Critical Review — Iteration 3: Lifecycle / Test Integration

**Focus:** timeout behavior, local mock teardown, database consistency, CTest integration, claims.

**Checks:**
- Every `asgend-memory scan`, `ann-index`, exact query and ANN query subprocess has a finite timeout.
- Local `ThreadingHTTPServer` shutdown is in `finally`; the server thread is joined and a still-live thread fails the benchmark.
- Unexpected query input/model creates a mock error; request counts must exactly match expected warm-up plus measured queries.
- SQLite `PRAGMA integrity_check` is checked before and after indexing, all 5,200 vectors must have an ANN signature, and the ANN index command's algorithm/count/status are checked.
- Added CTest smoke for the benchmark and an ordered verifier test using CTest fixtures; both have explicit finite timeout properties.
- The report distinguishes synthetic-only measurement from live-provider or user-quality evidence and leaves production defaults unchanged.

**Final gate passed:** full Release CTest completed **58/58 PASS**, 0 failures, exit marker `0`, 274.73 seconds. The registered timeout inventory contains 58/58 finite positive limits (10–300 seconds), including the two new ANN quality tests at 120 and 30 seconds. Post-run `/proc` inspection found zero matching test/ASGEND processes.
