# ASGEND project audit — cumulative report (2026-10-26)

## Executive status

The cross-project audit continuation has completed Stage 14 on top of Stage 13. Stage 12 closes the client CI path-filter gap. Stage 13 adds bidirectional runtime/OpenAPI route parity, runtime-auth/OpenAPI security parity, endpoint path coverage for four HTTP clients, and shared contract-test execution in the client workflows. Stage 14 adds an exact inventory of the local Core JSON-line RPC dispatcher, maps client operation references, updates `docs/PROTOCOL.md`, and runs the new check in CTest and the native Android/Flutter/Qt workflows.

This is not a blanket release sign-off. The local results below are specific to the Stage 13 snapshot and scope. Hosted GitHub Actions and external/toolchain acceptance are not inferred from static checks.

## Results (latest snapshot: Stage 14)

- Runtime Control Plane routes vs OpenAPI: **37/37 exact**, no missing or extra operations, no duplicate method/path pair.
- Core TCP/Unix RPC dispatch vs `docs/PROTOCOL.md`: **33/33 exact**, sorted and unique; native Android 33, Flutter 15, Qt 19, Node Agent 9 discovered supported operations.
- Runtime auth class vs operation-level security: **37/37 PASS**.
- Discovered client endpoint references: **49/49 mapped** to OpenAPI (native Android 14, Flutter 12, Qt 14, Node Agent 9). Qt scanning verifies paths only because its request verb is set separately in some functions.
- Contract suite: **35 tests; 34 PASS, 1 optional SKIP** (`openapi-spec-validator` unavailable).
- Core RPC inventory contract: **4/4 PASS**; full CTest **41/41 PASS** in 227.53 seconds, exit code `0` on Stage 14.
- Control Plane unit/security tests: **34 tests, 4 environment-dependent SKIP**.
- Node Agent suite: **37/37 PASS**.
- Client workflow trigger guard: PASS (3 workflows × 2 events, **76** required path assertions including the new Core RPC inventory test source).
- Native Android source verifier: PASS; this is not a Gradle/APK/device test.
- Release readiness check: `READY`, zero required blockers in the repository manifest; release gate test suite **6/6 PASS**.
- Control Plane, native Android, Flutter Android and Qt workflow YAML parses successfully.

## Stages in the current continuation

- Stage 10 — MCP lifecycle race regression.
- Stage 11 — SSE deadline/cancellation watchdog.
- Stage 12 — client CI trigger path coverage.
- Stage 13 — runtime/OpenAPI/client endpoint contract parity and shared CI execution.
- Stage 14 — local Core TCP/Unix JSON-line RPC documentation/client operation inventory parity, CTest and client workflow execution.

The Stage 12 combined CTest was recorded as **40/40 PASS** on that snapshot. Stage 13 changes the contract test source, workflow steps and documentation, not C runtime code; the complete CTest was not rerun for Stage 13. The prior CTest result must therefore remain attributed to Stage 12 rather than presented as a new Stage 13 result.

## Open items

1. Run hosted GitHub Actions for the touched Control Plane/native Android/Flutter/Qt workflows.
2. Infer HTTP verbs for Qt endpoint references in the static scanner rather than checking path coverage only.
3. Continue remaining cross-project TODOs: full MCP Landlock parity matrix, full SQLite fault matrix, interrupted memory-index recovery/property fuzzing, PostgreSQL-backed acceptance where services are available, Android instrumented/device acceptance, Flutter/Qt packaging acceptance and supported-architecture matrix.
4. A separate subagent launcher is unavailable in this environment; three local review passes were performed, but no independent agent score is claimed.

## Stage 14 — local Core JSON-line RPC parity

Stage 14 adds an exact machine-checked inventory for the local Core TCP/Unix JSON-line RPC protocol, separate from the Stage 13 Control Plane HTTPS/OpenAPI audit. `docs/PROTOCOL.md` now lists all **33** operations in a sorted inventory. `tests/test_core_rpc_contract_inventory.py` compares that set with `server_handle_request()` and verifies that operation literals and dynamic model lifecycle operations found in native Android, Flutter, Qt and Node Agent sources are supported. The test is registered as CTest `core_rpc_contract_inventory`.

- Standalone inventory tests: **4/4 PASS**.
- Current-stage build and CTest outcomes are to be read from the Stage 14 verification table below; the Stage 12 40/40 result is historical and must not be reused as a Stage 14 result.
- The check catches documentation/dispatch drift, not all runtime argument/schema errors. Local Core RPC remains distinct from local browser HTTP and remote Control Plane HTTPS.
- Three local review passes were performed; no independent external-agent rating is claimed because a subagent launcher is unavailable.


## Latest full verification — Stage 14

The Stage 14 final full CTest run is **41/41 PASS** (227.53 seconds, exit code `0`), after a fresh Release build. This supersedes Stage 12’s historical 40/40 snapshot for the current cumulative tree. The new `core_rpc_contract_inventory` passes 4/4 standalone assertions; client workflow guard passes with 76 required path assertions. Hosted Actions and platform-only acceptance remain unrun.
