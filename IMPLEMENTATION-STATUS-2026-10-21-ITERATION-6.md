# ASGEND — Implementation Status, iteration 6

Date: 2026-10-21

## Delivered

Implemented the next unchecked WP6 item from `TODO.md`: opt-in decision-model query planning via `asgend-memory query ROOT QUERY --plan`.

### Implementation details

- Added `src/memory/plan.c` and wired it into the `asgend_memory` CMake target.
- Added public query options for planner endpoint/model/key and CLI environment support through `ASGEND_MEMORY_DECISION_BASE_URL`, `ASGEND_MEMORY_DECISION_MODEL`, and optional `ASGEND_MEMORY_DECISION_API_KEY`.
- `--plan` is explicitly opt-in. Without the flag there is no planner request and the output shape remains unchanged.
- The planner input contains only the query plus a fixed allowlist of intents. Retrieved documents, excerpts, snippets, embeddings, and metadata are not sent to the planner.
- The response must be strict JSON with exactly `intent`, `reason`, `rewrites`, and `filters`; filters must contain exactly `type`, `tag`, and `path_prefix`. Extra/missing keys, markdown, control characters, excessive rewrites, invalid intents, path traversal and unsupported values reject the complete plan.
- Rewrites are bounded to three strings of at most 256 bytes and supplement, rather than replace, the original BM25 query. Model intent can only select fixed local graph weights/families.
- Explicit CLI filters take precedence over planner-suggested filters.
- Remote endpoints require HTTPS; cleartext HTTP is only allowed for loopback. Redirects are disabled, API responses are capped at 128 KiB, and the timeout uses the query timeout setting.
- Model configuration, transport, HTTP, response and schema errors fall back to ordinary local search. `query_plan.fallback_reason` reports why the plan was not used.
- Added `tests/test_memory_query_plan.py` covering rewrites/filter behavior, user-filter precedence, strict JSON/extra-key rejection, malicious path rejection, missing configuration, remote plain-HTTP rejection, query-only payload and document-content non-disclosure.
- Updated `TODO.md`, `README.md`, `docs/MEMORY_INDEX.md`, `docs/ROADMAP.md`, and `docs/IMPLEMENTATION_PLAN.md`.

## Verification results

- CMake Release build: passed, no compiler warnings in the final build log.
- Quick CTest suite excluding the long `integration` test: **15/15 passed**.
- Contract tests: **31 passed, 1 skipped**.
- Control Plane tests: **10/10 passed**.
- Node-agent tests: **37/37 passed**.
- The new query-planner test under AddressSanitizer + UBSan: passed, with leak detection enabled and no sanitizer reports.
- Python syntax compilation for `tests/test_memory_query_plan.py`: passed.

## Not yet verified / remaining

- The long `integration` CTest was excluded from this quick regression run; it has no new pass result in this iteration.
- A larger human-judged corpus is still required to evaluate whether query planning improves Precision/Recall/MRR without excessive narrowing. Until that review, `--plan` stays opt-in.
- Automatic agent retrieval remains local-only and does not invoke this planner.
- Docker Compose runtime, a real PostgreSQL migration/backup-restore drill, Qt runtime/package builds, Android SDK/Flutter build and full Control Plane ↔ node-agent ↔ core acceptance are still open.
- ASGEND 1.0.0 production acceptance is not claimed.
