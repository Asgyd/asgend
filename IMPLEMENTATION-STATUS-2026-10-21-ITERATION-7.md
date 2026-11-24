# ASGEND — Implementation Status, iteration 7

Date: 2026-10-21

## Delivered

Continued from `TODO.md`, focusing on the remaining WP6 query-plan quality evaluation item. Added a reproducible paired evaluation mode rather than claiming model quality based on a synthetic test.

- `scripts/evaluate_memory_retrieval.py` now accepts `--plan` to evaluate explicit query planning and `--compare-plan` to run the same judged queries with planner disabled and enabled.
- Paired output reports baseline metrics, planned metrics and `delta_planned_minus_baseline` for Precision@K, Recall@K, MRR@K, nDCG@K and hits@K; individual planned-query records include whether a plan was used, rewrites and fallback reason.
- Existing default evaluator output remains backward compatible for callers which do not pass the new flags.
- Added `tests/fixtures/query_plan_eval_dataset.json`, a small deterministic smoke fixture, and `tests/test_query_plan_eval.py`, which starts a loopback mock planner, constructs a disposable project corpus, runs the paired evaluator and verifies that metric deltas improve with known rewrites.
- Added CTest target `query_plan_eval_smoke`.
- Updated `TODO.md`, `README.md`, `docs/MEMORY_INDEX.md`, `docs/ROADMAP.md` and `docs/IMPLEMENTATION_PLAN.md`.

## Verification results

- CMake Release build: passed.
- Quick CTest suite excluding the long `integration` test: **16/16 passed**.
- API contract tests: **10/10 passed**.
- Control Plane tests: **37/37 passed**.
- Node-agent tests: **30 passed, 1 skipped** (31 total in this invocation).
- Retrieval evaluator syntax compilation: passed.
- New planner comparison smoke test: passed; mock planner called once per query, planned MRR@3 and Recall@3 exceeded baseline in the deterministic fixture, and SQLite remained healthy.

## Important limits

- The smoke corpus has curated synthetic relevance labels; it is not an independent, large human-judged dataset. Its metric improvements prove the paired evaluation pathway works, not that a real provider improves search quality.
- A large independent human-judged dataset, evaluator run against a configured live provider, and review of any regressions remain required before broad recommendation of `query --plan`.
- The long `integration` CTest was excluded from this run; it has no new pass result in this iteration.
- Docker Compose runtime, live PostgreSQL migration/backup-restore, Qt/Linux packaging, Flutter/Android build and full CP ↔ node-agent ↔ core acceptance remain open.
- Production readiness and ASGEND 1.0.0 acceptance are not claimed.
