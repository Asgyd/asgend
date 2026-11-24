# ASGEND project audit TODO (2026-10-25)

Statuses apply to the source snapshot in the accompanying audit archive. `DONE` means the code/test or documentation item was changed and the listed local check passed; it does not imply all project-wide release gates passed.

## P0/P1 — correctness and safety

- [x] **CORE-15:** Serialize MCP startup/restart state, descriptors and protocol handshake with `MCPServer.mutex`; don't reject tool calls based on an unlocked pre-check of `started`.
  - Acceptance: Release build and `core_mcp_launcher_smoke`; regression submits an MCP task at the post-`tools/list` restart boundary.
  - Result: PASS.
- [x] **CORE-16:** Make SSE first-token/idle deadlines deterministic while libcurl waits on a silent socket; avoid indefinite task/worker cleanup and verify no test peer remains after timeout.
  - Acceptance: bounded idle-after-token test, cancellation test, full integration exit `0`.
  - Result: PASS after switching stream transfer to `curl_multi` with a bounded poll/deadline watchdog; standalone SSE fault suite and full integration exit `0`.
- [x] Run the complete registered CTest suite in one invocation with exit code and count recorded. **40/40 PASS**, 222.28 seconds, exit code 0; includes integration and standalone SSE watchdog.
- [~] MCP sandbox parity test coverage added in Stage 19: conditional Landlock allow/deny matrix, strict fail-closed check, failed-start workspace cleanup, and four simultaneous tool requests during a restart boundary. This container returns `ENOSYS` for Landlock, so the positive filesystem matrix must still run on a Landlock-capable host.
- [ ] Complete SQLite fault injection for all commit/bind/busy paths and concurrent process recovery.

## P2 — correctness, coverage and resilience

- [x] Add targeted restart-boundary regression to `tests/test_core_mcp_launcher.py`.
- [ ] Validate the core config contract across C/Python/Node/Flutter/Qt/Android and reject unknown or type-invalid fields consistently.
- [ ] Add property/fuzz tests for HTTP framing, MCP JSON-RPC/SSE parsing, artifact metadata and file/path boundaries.
- [ ] Verify memory indexing consistency after interrupted extraction, duplicate indexing and source-file changes.
- [ ] Exercise Control Plane pairing/revocation, RBAC, node disconnect/reconnect, cursor pagination and artifact expiry against PostgreSQL.
- [ ] Verify frontend/client API compatibility whenever core headers, `src/server.c`, `src/config_editor.c`, OpenAPI or RPC schemas change.
- [ ] Add Android `androidTest` coverage for rotation, process death, network errors, IME, accessibility and responsive layouts; build/install APK on API 37 emulator.
- [ ] Build and smoke-test Flutter and Qt clients with matching platform dependencies.
- [ ] Verify clean install/upgrade/uninstall for supported package formats in matching OS containers.

## P3 — maintainability/documentation

- [ ] Reconcile historical status files with current evidence without rewriting history; link the current audit report from README and release status.
- [ ] Keep module extraction as a separate refactor after correctness blockers are closed.
- [ ] Add one command that emits a machine-readable verification manifest (command, exit code, elapsed time, count, environment dependencies).

## Current blockers / unavailable checks

- Full integration and standalone `core_llm_sse_watchdog` pass; the combined current CTest suite is confirmed **40/40 PASS** (222.28 seconds, exit code 0) on the Stage 12 snapshot.
- Android SDK/Gradle, Flutter SDK, Qt toolchain, Docker and PostgreSQL are not installed in the current execution environment; their acceptance checks are NOT RUN here.
- No separate subagent launcher is available; local review passes are not represented as an independent agent assessment.


## Newly identified CI coverage gap

- [x] **CI-ANDROID-01 (P2):** Native Android, Flutter Android, and Qt workflow push/PR path filters now include core sources, public headers, Control Plane, OpenAPI/protocol contracts, contract tests, shared API documentation, and root CMake changes. Added `tests/test_client_workflow_contract_triggers.py` and registered it as CTest `client_workflow_contract_triggers`.
  - Acceptance: 3 workflows × 2 triggers each contain all 9 shared contract paths plus their client-specific paths; 70 guard assertions PASS. Clean Release build and full CTest **40/40 PASS**, exit code 0; see Stage 12 audit report.

## Stage 13 — API/OpenAPI/client route parity

- [x] **CONTRACT-01:** compare the runtime Control Plane method/path inventory bidirectionally with OpenAPI; reject duplicates and missing/extra operations. Result: 37 runtime routes == 37 OpenAPI operations.
- [x] **CONTRACT-02:** check each `Router.add` auth class against operation-level OpenAPI `security`. Result: 37/37 PASS.
- [x] **CONTRACT-03:** add request-bearing client path coverage checks for native Android, Flutter, Qt and Node Agent. Result: 49 discovered references, all documented.
- [x] **CONTRACT-04:** add negative static-segment matcher test and minimum path-extractor floors for each client.
- [x] Add shared contract-test dependency manifest and run the test suite from Control Plane, native Android, Flutter Android and Qt workflows.
- [ ] Confirm the updated workflows on hosted GitHub Actions after push/PR; local YAML/source validation is not hosted execution.
- [ ] Improve Qt scanner to infer each HTTP method from its request block; current Stage 13 check is intentionally path-only for Qt.

## Stage 13 recorded checks

- Contract suite: 35 tests, 34 PASS / 1 optional skip.
- Control Plane: 34 tests, 4 environment-dependent skips.
- Node Agent: 37/37 PASS.
- Native Android source verifier: PASS (not an APK/device build).
- Release readiness: READY; release-gate tests 6/6 PASS.
- Client workflow path guard: PASS, 70 required path assertions.

## Stage 14 — local Core JSON-line RPC

- [x] **RPC-DOC-01 (P2):** add sorted complete dispatch inventory to `docs/PROTOCOL.md`; exact parity with `server_handle_request()` — 33/33.
- [x] **RPC-CLIENT-01 (P2):** scan Android, Flutter, Qt and Node Agent local-RPC references and ensure all discovered static/dynamic commands are supported.
- [x] **RPC-TEST-01 (P2):** register `core_rpc_contract_inventory` in CTest; unit tests include malformed inventory/marker negative cases.
- [x] Add `tests/test_core_rpc_contract_inventory.py` to the native Android/Flutter/Qt `push` and `pull_request` path filters and run it in those workflows; trigger guard is now 76 assertions.
- [ ] Add per-operation request-schema/required-field checks and response-shape smoke tests for RPC operations; current Stage 14 check is operation inventory only.
- [ ] Add runtime client acceptance for dynamic model lifecycle actions on Qt/Flutter/Android; source-level strings do not prove interaction behavior.

## Stage 15 continuation — 2026-10-27

- [x] **RPC-REQ-01:** exercise required fields and invalid typed parameters against a live Core TCP JSON-line server. Targeted `core_rpc_runtime_contract` passes.
- [x] **RPC-RESP-01:** add live response-shape smoke tests for representative read-only APIs, task/workspace/artifact access, and plan CRUD/wait.
- [x] **RPC-DOC-02:** document required request fields and representative response keys for all 33 Core RPC operations in `docs/PROTOCOL.md`.
- [x] Register `tests/test_core_rpc_runtime_contract.py` as CTest `core_rpc_runtime_contract`.
- [ ] Add deterministic success-state fixtures for `cancel`, `answer` and `retry_task`, plus model start/stop and successful LSP diagnostics.
- [ ] Add runtime UI acceptance for dynamic model lifecycle actions on Qt/Flutter/Android; source scans are not sufficient evidence.
- [ ] Run hosted GitHub Actions for the changed root CTest and client workflows after push/PR.

Stage 15 evidence: fresh Release configure/build PASS; targeted `core_rpc_runtime_contract` PASS (1/1, 1.05 s); full CTest **42/42 PASS** (222.46 s, exit code 0) on the exact Stage 15 tree, including integration (131.63 s), SSE watchdog (39.64 s), and the new RPC runtime test.


## Stage 19 continuation — 2026-10-27

Stage 19 extends `core_mcp_launcher_smoke` with a real-child Landlock filesystem probe (allowed read, denied unrelated read, writable sandbox, denied writes outside the sandbox/read-only path, and denied execution from a read-only path), a startup-failure workspace-cleanup assertion, and four concurrent task requests across the post-restart handshake. Fresh Release configure/build and targeted test passed (1/1, 21.49 seconds). The current container's `landlock_create_ruleset` returns `ENOSYS`, so this run validates strict fail-closed and cleanup, but not the positive Landlock matrix. See `docs/PROJECT_AUDIT_STAGE_19_MCP_SANDBOX_PARITY.md`.
