# Stage 16 — Core RPC state transitions and runtime fixtures

**Snapshot:** 2026-10-27, cumulative Stage 15 source plus deterministic state-transition coverage.

## Finding

Stage 15 verified required request fields and representative response shapes, but did not directly prove that successful state-changing RPCs cause the intended durable transition. Some behavior already existed in the long integration test; a smaller dedicated CTest target makes the key paths faster to rerun and more diagnostic on failure.

## Changes

- Added `tests/test_core_rpc_state_transitions.py` and registered `core_rpc_state_transitions` in CTest with a 75-second timeout.
- Used the local deterministic OpenAI-compatible mock to cancel an actively streaming task, and checked its terminal `cancelled` state plus persistent cancellation/status events.
- Created a real `waiting_input` task, answered it via RPC, asserted the response requeued it, and verified the durable `user_answer` event contains the selected answer and option ID before successful completion.
- Seeded one failed task in the isolated SQLite test database, redrove it via `retry_task`, checked the `task_redriven` event and the one-attempt budget increase, then confirmed a completed task is no longer eligible for dead-letter retry.
- Added a temporary managed-model fixture and exercised `model_start`, `model_stop`, `model_disable`, and `model_enable` through Core RPC, validating observable runtime/configuration state.
- Exercised `lsp_diagnostics` against the repository's mock LSP JSON-RPC process and asserted a diagnostic message and source location are returned.
- All stateful fixtures use a temporary directory, local mock endpoints, and a throwaway database; no production config or data is mutated.

## Verification

- Fresh CMake Release configure/build: **PASS** with GCC 14.2 and the installed `json-c`, libcurl, SQLite, OpenSSL Crypto and Zlib dependencies.
- Targeted CTest `core_rpc_state_transitions`: **1/1 PASS** (6.28 seconds).
- Complete CTest suite: **43/43 PASS** in **228.09 seconds**, exit code `0`. The same run passed `integration` (130.24 seconds), `core_llm_sse_watchdog` (39.61 seconds), the Stage 15 runtime contract and the new Stage 16 state-transition target (6.62 seconds).
- The complete CTest output is preserved in `PROJECT_AUDIT_STAGE_16_CTEST_LOG_2026-10-27.txt`.

## Scope and limits

- The state fixture validates Core RPC behavior using local deterministic mocks, not an actual provider service or production managed model.
- It proves an LSP response from the mock protocol peer, not compatibility with every language server or language extension.
- Hosted GitHub Actions and device-level runtime behavior for Android, Flutter, and Qt remain separate acceptance gates.
- Core TCP/Unix JSON-line RPC remains distinct from the browser HTTP API and Control Plane HTTPS/OpenAPI contract.
