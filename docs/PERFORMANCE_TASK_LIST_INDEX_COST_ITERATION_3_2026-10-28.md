# Performance index-cost audit — Iteration 3 / critical review (2026-10-28)

## Corrections validated

- Captured actual cgroup CPU/RAM caps and included source/harness SHA-256 values; the final JSON hash matches the current benchmark file.
- WAL is checkpointed before database file size is measured; `dbstat` index bytes are reported separately from whole DB file size.
- p95/p99 follow the shared policy (p95 only at 20+ samples, p99 at 100+); seed insertion has only five samples, so tail percentiles are explicitly suppressed.
- The smoke exercises 20 samples across both status distributions and checks all five operations, including delete/reinsert offset boundaries. Final full CTest on the completed tree passed **52/52 in 277.32 s** (exit 0); the focused smoke passed in 2.49 s, and all 52 generated timeout values are positive and finite.

## Remaining caveats

The candidate's read gains are substantial on large synthetic corpora, but write medians sometimes reverse direction by distribution. The final profile additionally checks `PRAGMA integrity_check=ok` and exact task row counts after every benchmark database run (60/60 passed). This must not be recast as proof that adding indexes accelerates writes; it is evidence of timing/cache sensitivity. Candidate indexes remain isolated pending actual C DB API timing and representative production data. Per-operation samples include WAL commit cost with `synchronous=FULL`; periodic checkpoint cost is excluded, while initial file size is measured after checkpoint.
