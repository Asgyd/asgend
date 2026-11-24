# Stage 17 — Client workflow trigger coverage for Core RPC tests

**Snapshot:** 2026-10-27, based on Stage 16.

## Finding

The three client-specific GitHub Actions workflows (native Android, Flutter Android and Qt) included `tests/test_core_rpc_contract_inventory.py` in their `push` and `pull_request` path filters, but omitted the newer `tests/test_core_rpc_runtime_contract.py` and `tests/test_core_rpc_state_transitions.py` tests. A change limited to either test could therefore fail to trigger client contract workflows, despite the tests checking shared Core RPC behavior.

The existing `tests/test_client_workflow_contract_triggers.py` guard checked the inventory test path but not the Stage 15 and Stage 16 runtime tests.

## Changes

- Added `tests/test_core_rpc_runtime_contract.py` and `tests/test_core_rpc_state_transitions.py` to both `push.paths` and `pull_request.paths` in `.github/workflows/native-android.yml`, `.github/workflows/flutter-android.yml`, and `.github/workflows/qt-client.yml`.
- Extended the existing trigger guard's required path list with both test files. This keeps the regression test small and avoids adding a new runtime dependency.
- Kept the existing workflow jobs and their execution behavior unchanged; this change only fixes which changes trigger those jobs.

## Verification

- `python3 tests/test_client_workflow_contract_triggers.py`: **PASS**, 3 workflows × 2 trigger types, 88 required path assertions (44 per trigger event).
- `python3 tests/test_core_rpc_contract_inventory.py .`: **PASS**, 4 tests.
- Parsed all 5 `.github/workflows/*.yml` files with PyYAML's `BaseLoader`: **PASS**; each has a top-level trigger declaration and a nonempty jobs mapping.
- `python3 scripts/check_release_readiness.py`: **READY**, candidate 1.0.0, required blockers 0.

## Scope and limits

- This locally verifies path-filter coverage and YAML parseability. It does not prove that GitHub Actions ran, that GitHub's workflow planner scheduled the jobs on a hosted runner, or that device-specific acceptance passed.
- Physical Android QA and live hosted Actions evidence remain open acceptance items. Qt UI feature gaps and other post-1.0 backlog items remain tracked in `TODO.md`.
