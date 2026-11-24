# Performance index-cost audit — Iteration 1 (2026-10-28)

## Result

Established the index-maintenance cost matrix and implemented a focused benchmark harness. The first full run exposed a test-harness defect: `delete_reinsert` calculated OFFSET from the whole task table while selecting only `completed` rows. That run was rejected and not used as audit evidence.

## Correction

Changed the offset to be bounded by the selected `completed` bucket size. The smoke was extended to 20 samples per operation on both balanced and queued-heavy distributions; the previous failure class now executes in the smoke.

## Evidence carried forward

Only the subsequent full run with `passed=true` and the final harness hash is used in the result report. No production indexes were changed.
