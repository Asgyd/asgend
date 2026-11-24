# ASGEND project audit — cumulative report (2026-10-27)

## Executive status

Stage 19 continues the cross-project audit by adding MCP sandbox policy and lifecycle stress coverage. Stage 18 remains the client-contract baseline for Qt HTTP method parity, while Stages 14–16 establish the Core RPC inventory, live request/response checks and state-transition fixtures. This report distinguishes local source/build/test evidence from hosted CI and platform acceptance.

## Latest results — Stage 19

- Stage 19 extends MCP sandbox parity coverage: the launcher target now contains a real child probe for Landlock allow/deny behavior, verifies fail-closed startup when Landlock is unavailable, verifies workspace cleanup after startup/exec failure, and submits four tasks concurrently across the supervised restart boundary with four Core workers.
- Fresh Release configure/build: PASS (GCC 14.2; 41 Ninja build steps). Targeted `core_mcp_launcher_smoke`: **1/1 PASS**, 21.49 seconds.
- Host limitation: `landlock_create_ruleset` (Linux syscall 444) returns `ENOSYS` in this container. The target therefore executed the strict fail-closed and cleanup branch; its positive filesystem allow/deny matrix is implemented but was not run here, and still needs a Landlock-capable Linux runner.
- Fresh Release full CTest: **43/43 PASS in 248.74 seconds**, exit code `0`. Integration passed in 134.33 seconds, SSE watchdog in 39.99 seconds, and `core_mcp_launcher_smoke` in 20.74 seconds. The full log is `docs/PROJECT_AUDIT_STAGE_19_CTEST_LOG_2026-10-27.txt`.
- Landlock limitation remains explicit: this container returns `ENOSYS` for syscall 444. Strict fail-closed and sandbox cleanup passed, but the positive allow/deny filesystem matrix still needs a Landlock-capable Linux runner.
- See `docs/PROJECT_AUDIT_STAGE_19_MCP_SANDBOX_PARITY.md` for scope and limits.

## Latest results — Stage 18

- Stage 18 closes the previously open Qt HTTP-method inference item: the shared client/OpenAPI test now infers the Qt adapter's default and explicitly assigned verbs, expands dynamic `answer`/`cancel`/`retry` request paths, and accounts for the ticket-authorized artifact download GET.
- Full shared contract suite: **36 tests, 35 PASS / 1 optional skip**. The Qt checker requires all 16 current HTTP method/path branches and checks them against OpenAPI.
- This is static source-to-contract verification. Qt native UI/network acceptance and hosted GitHub Actions remain unverified.
- Stage 17 path-filter checks and Stage 16 full CTest results remain historical results as recorded below; Stage 18 changes the Python contract scanner and its tests only.

## Latest results — Stage 17

- Stage 16 runtime verification remains the latest full native CTest run: **43/43 PASS**, 228.09 seconds, exit code `0`; it included `integration` (130.24 seconds) and `core_llm_sse_watchdog` (39.61 seconds).
- Core TCP/Unix RPC dispatcher vs `docs/PROTOCOL.md`: **33/33 exact**, sorted and unique (Stage 14 inventory test; rerun in Stage 17, all 4 inventory assertions passed).
- Stage 15 runtime request/response contract: previously passed in the full Stage 16 CTest run.
- Stage 16 live state-transition target: previously passed targeted (1/1 in 6.28 seconds) and in full CTest (6.62 seconds).
- Stage 17 CI path-filter audit found and fixed omitted Stage 15/16 runtime-test paths in native Android, Flutter Android, and Qt `push`/`pull_request` filters. Trigger guard now passes **88 required path assertions** (44 per trigger event); up from 76 in Stage 16.
- All 5 GitHub Actions workflow YAML files parse successfully via local PyYAML validation, and `scripts/check_release_readiness.py` reports **READY** with 0 required blockers.
- Hosted GitHub Actions scheduling/execution and client device acceptance remain unverified by these local checks.

## Stage 15 full verification — historical baseline

The fresh Release build completed, then one complete CTest invocation passed **42/42** tests in **222.46 seconds** (exit code `0`). The Stage 15 target passed in 0.83 seconds in this complete run. Full integration passed in 131.63 seconds and the SSE watchdog passed in 39.64 seconds. The per-test output is preserved in [`PROJECT_AUDIT_STAGE_15_CTEST_LOG_2026-10-27.txt`](PROJECT_AUDIT_STAGE_15_CTEST_LOG_2026-10-27.txt); it is a local run, not an independent hosted CI run.

## Stage 15 — Core RPC semantics

`tests/test_core_rpc_runtime_contract.py` launches an isolated core and calls the real TCP JSON-line listener. It verifies structured error responses for missing required fields and invalid integer-typed parameters, and verifies representative success shapes for liveness, health, listing/configuration/model/resource/MCP APIs, task submission/status/events, plan CRUD/wait and workspace/artifact reads/writes. All test mutations are isolated to temporary state; the plan entry is deleted during cleanup.

`docs/PROTOCOL.md` now describes required request fields and representative response fields for all 33 dispatch operations. The test is registered in CTest as `core_rpc_runtime_contract` with a 45-second timeout.

### Stage 15 scope at that snapshot

This test is not exhaustive acceptance of every operation's success branch. At the Stage 15 snapshot, deterministic state-transition fixtures for cancel/answer/retry, model lifecycle starts/stops, and successful LSP diagnostics were open; Stage 16 adds local mock-backed coverage for those Core RPC paths. Source scans are not proof of dynamic client UI behavior, and no hosted Actions or device-level test execution is claimed.

## Stage 16 — Core RPC state transitions

`docs/PROJECT_AUDIT_STAGE_16_CORE_RPC_STATE_TRANSITIONS.md` records the new `core_rpc_state_transitions` CTest target and successful transition checks. The test uses an isolated temporary database, deterministic local LLM stream and managed-model processes, and a mock LSP JSON-RPC peer. It verifies persistent task events and statuses as well as model runtime state. The complete 43-test local CTest log is available in `docs/PROJECT_AUDIT_STAGE_16_CTEST_LOG_2026-10-27.txt`.

### Limits

The new success-path assertions use deterministic local mocks. They do not establish hosted GitHub Actions execution, actual device UI acceptance, production-provider interoperability, or compatibility with every managed model and language server.

## Stage 17 — client workflow trigger coverage

`docs/PROJECT_AUDIT_STAGE_17_CI_TRIGGER_COVERAGE.md` records a workflow gap found after Stage 16: three client workflows triggered for the Core RPC inventory test but not for the newer runtime contract and state-transition tests. Those two test paths are now included under both `push` and `pull_request` for native Android, Flutter Android and Qt. The trigger guard now checks 88 required paths across six event/workflow combinations. The guard, four Core RPC inventory assertions, YAML parsing of all five workflows, and release-readiness check pass locally. This is configuration/source evidence only; it is not a hosted Actions run.
