# Project audit — Stage 12: client CI trigger coverage

Date: 2026-10-25
Status: **implemented and locally verified**. Hosted workflow execution requires a pushed commit/PR.

## Scope

Ensure core/API contract changes schedule native Android, Flutter Android, and Qt workflow validation.

## Changes

- Updated `push.paths` and `pull_request.paths` in `.github/workflows/native-android.yml`, `.github/workflows/flutter-android.yml`, and `.github/workflows/qt-client.yml`.
- Shared paths: `src/**`, `include/**`, `server/**`, `contracts/**`, `tests/contracts/**`, `docs/API_CONTRACT.md`, `docs/PROTOCOL.md`, `docs/REMOTE_CONTROL_PLANE.md`, and root `CMakeLists.txt`.
- Preserved each workflow's existing client-specific filters.
- Added `tests/test_client_workflow_contract_triggers.py` (Python standard-library only) and registered CTest `client_workflow_contract_triggers`. The test checks shared contract paths plus client/workflow-specific paths for both `push` and `pull_request`.

## Verification evidence

| Check | Result | Evidence |
|---|---|---|
| Clean CMake Release configure/build | PASS | GCC 14.2; all targets built and linked |
| Workflow YAML syntax | PASS | All 3 edited workflow files parse as YAML |
| Source-contract path guard | PASS | 3 workflows × 2 triggers; 70 required path assertions |
| Guard mutation test | PASS | Removing a required path from a copied workflow makes the guard fail |
| Python syntax | PASS | `python3 -m py_compile tests/test_client_workflow_contract_triggers.py` |
| Full registered CTest | **40/40 PASS** | 222.28 s, exit code 0; includes integration and SSE watchdog |

The full CTest output is captured in the audit execution log for this stage; its exact result is 40/40 and exit code 0. The `integration` test passed in 132.73 s and `core_llm_sse_watchdog` passed in 39.73 s in the same invocation.

## Review passes and corrections

1. **Trigger consistency:** visually and mechanically verified shared paths in both event filters for all three workflows. Removed a stray blank line in the Flutter PR list.
2. **Guard effectiveness:** expanded the test to protect workflow-specific paths as well as shared paths; clarified its pass message to report 70 assertions (35 for push and 35 for PR). A negative mutation test confirms a removed required path is detected.
3. **Evidence/package integrity:** compared final docs and CMake test registration against the run results; verified build artifacts are excluded from the source archive and that the incremental patch replays to the exact final text tree.

These are three local review passes, not an independent external/subagent assessment. No hosted GitHub Actions run is claimed from this local archive.

## Remaining limitation

The source-level CI trigger gap is closed. Actual hosted execution still needs a pushed commit/PR; the local path guard does not itself run Gradle/Android SDK, Flutter, or Qt toolchains.
