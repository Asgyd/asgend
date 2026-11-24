# Stage 15 — Core RPC request and response semantics

**Snapshot:** 2026-10-27, cumulative Stage 14 source plus runtime contract tests.

## Finding

Stage 14 made the Core TCP/Unix JSON-line RPC dispatch operation inventory exact, but operation presence alone did not verify that required request fields were enforced consistently or that key response fields retained their expected JSON types.

## Changes

- Added `tests/test_core_rpc_runtime_contract.py`, which starts an isolated core instance and sends requests over the actual TCP JSON-line listener.
- Covered required-field and type-invalid failures for task-scoped actions, workspace access, configuration editing, model lifecycle actions, plan mutations, and optional typed query parameters.
- Exercised response shapes for liveness, health, agents/tasks/dead letters, configuration/model/resource/MCP views, plan wait/list/CRUD, task status/events, workspace list/read/write/chunk/byte-read, artifacts and submission.
- Kept test mutations confined to a temporary config/database, a generated task workspace, and a temporary global plan entry that is removed in cleanup.
- Added a CTest target `core_rpc_runtime_contract` with a 45-second timeout.
- Documented required fields and representative success response keys for all 33 RPC operations in `docs/PROTOCOL.md`.

## Verification

- Fresh CMake Release configure/build: **PASS** with GCC 14.2 and the installed `json-c`, libcurl, SQLite, OpenSSL Crypto and Zlib dependencies.
- Targeted CTest `core_rpc_runtime_contract`: **1/1 PASS** (1.05 seconds).
- Complete CTest suite on the Stage 15 tree: **42/42 PASS**, 222.46 seconds, exit code `0`; includes `integration` (131.63 seconds), `core_llm_sse_watchdog` (39.64 seconds), and the new RPC runtime contract target.

## Scope and limits

- This is live TCP runtime coverage of representative request requirements and response shapes, not exhaustive behavioral verification for all 33 operations.
- State-dependent transitions (`cancel`, `answer`, `retry_task`) and environment-dependent model/LSP actions are checked for required inputs, but their successful transitions still need dedicated deterministic fixtures.
- The test does not assert that hosted GitHub Actions executed, or that Qt/Flutter/Android runtime UI actions behave correctly on devices.
- Core JSON-line RPC remains distinct from the browser HTTP API and Control Plane HTTPS/OpenAPI contract.

## Follow-up

Continue with deterministic state-transition fixtures for `cancel`/`answer`/`retry_task`, model lifecycle success and LSP diagnostics. Keep dynamic model-action UI acceptance as a platform gate, not a claim based on source scans.
