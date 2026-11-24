# ASGEND project audit — iteration 3 (2026-10-25)

## Scope

This pass continues the cumulative audit after the Stage 10/11 fixes. It covers cross-client CI triggers and re-runs whole-project verification on the resulting source snapshot. Historical per-stage reports are preserved as historical evidence.

## Finding CI-ANDROID-01 — shared API changes could skip client workflows (P2)

### Evidence

The native Android workflow path filters watched the Android module and Android docs. Flutter and Qt filters were similarly scoped mostly to their own client trees. Core RPC handlers, public headers, Control Plane endpoints, protocol/OpenAPI schema, shared contract tests, or root CMake changes could therefore alter an API contract without scheduling a client validation job.

### Change

Added the following patterns to both `push.paths` and `pull_request.paths` in `.github/workflows/native-android.yml`, `.github/workflows/flutter-android.yml`, and `.github/workflows/qt-client.yml`:

- `src/**`, `include/**`, `server/**`, `contracts/**`, `tests/contracts/**`
- `docs/API_CONTRACT.md`, `docs/PROTOCOL.md`, `docs/REMOTE_CONTROL_PLANE.md`
- root `CMakeLists.txt`

Added `tests/test_client_workflow_contract_triggers.py` and CTest `client_workflow_contract_triggers`. The guard validates both CI event filters, both shared-contract paths and client-specific trigger paths.

## Verification

- Static path-filter guard: PASS for all 3 workflows × 2 event types; it asserts 70 required path entries and was mutation-checked.
- Python compile: PASS.
- All 3 changed workflow YAML files parse successfully.
- Clean Release configure/build: PASS.
- Unified CTest: **40/40 PASS**, 222.28 seconds, exit code 0; integration 132.73 seconds and SSE watchdog 39.73 seconds also passed in the same run.
- Hosted GitHub Actions execution cannot be triggered from the local archive; it remains dependent on push/PR execution.

## Review conclusion

The source-level gap is closed by the workflow filters and regression test. A workflow path-filter test verifies configuration text but is not a substitute for a real hosted Actions run. Three local review passes were completed: trigger consistency, negative-path guard effectiveness, and evidence/package reproducibility. No independent external-agent rating is claimed.
