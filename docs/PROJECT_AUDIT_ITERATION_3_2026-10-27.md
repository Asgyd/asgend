# Project audit — iteration 3 (2026-10-27)

## Scope

Follow-up to Stage 14 local Core TCP/Unix JSON-line RPC inventory. This iteration covers required request fields, invalid parameter types, and representative runtime response shapes without conflating Core RPC with Control Plane HTTPS/OpenAPI.

## Findings and corrections

- **RPC-REQ-01:** the dispatcher inventory did not by itself prove required inputs were rejected. Added runtime negative checks for documented required fields and typed optional parameters.
- **RPC-RESP-01:** the inventory did not guard response field presence and JSON types. Added smoke assertions for representative success envelopes across read-only APIs, plans, tasks, workspace files and artifacts.
- **RPC-DOC-02:** documented request requirements and representative response fields for each of the 33 Core RPC operations in `docs/PROTOCOL.md`.

## Verification

- Fresh Release build: PASS.
- `core_rpc_runtime_contract`: PASS (1/1 in 1.05 seconds).
- Full CTest: **42/42 PASS**, 222.46 seconds, exit code `0` on the exact Stage 15 source tree; the integration target passed in 131.63 seconds and SSE watchdog in 39.64 seconds.

## Limits

No hosted CI result is claimed. No device-level runtime action is inferred from source checks. Successful state transitions for cancellation, answer submission, retry, model start/stop and LSP remain dedicated follow-up coverage.
