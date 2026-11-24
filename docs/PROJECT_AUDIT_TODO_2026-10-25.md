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
- [ ] Complete MCP sandbox parity tests, including Landlock allow/deny cases, startup failure cleanup and restart stress with concurrent tool requests.
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
