# Project audit — iteration 2 (2026-10-26)

## Scope

Second sequential local audit iteration: local C Core JSON-line RPC inventory, protocol documentation, client operation references, and CTest integration. This follows the Stage 13 Control Plane HTTPS/OpenAPI parity review; the protocols are intentionally treated as separate surfaces.

## Findings and corrections

- **RPC-DOC-01 (P2):** the protocol guide lacked one complete operation inventory. Added 33 sorted commands with stable markers; automated exact comparison against dispatch now fails on missing/extra/duplicate operations.
- **RPC-CLIENT-01 (P2):** client source references were not mechanically checked against Core dispatch. Added a scanner for Android, Flutter, Qt and Node Agent, including dynamically selected model lifecycle commands and per-client floors to catch scanner failure.
- **RPC-TEST-01 (P2):** protocol inventory was not in CTest. Added `core_rpc_contract_inventory` with no extra dependencies.

## Verification

- Runtime dispatch vs documentation: 33/33 exact.
- Client operation references: all discovered static/dynamic operations are supported by the dispatcher (Android 33, Flutter 15, Qt 19, Node Agent 9 references/unique operations).
- Standalone test: 4/4 PASS.
- Clean Release build and registered CTest results are recorded in `PROJECT_AUDIT_STAGE_14_CORE_RPC_PARITY.md` and the cumulative final report after execution.

## Limits

This is source-level inventory/contract checking. It does not replace per-client runtime tests. No independent subagent launcher is available; three local review passes are not claimed as external review.
