# ASGEND Remote API v1 — contract change log

This file records binding API decisions. Documentation-only clarification that does not change the wire contract may be recorded here without a compatibility note; any wire-shape or state-machine change must identify its compatibility impact and update OpenAPI, JSON Schema, examples and tests together.

## 2026-10-22 — 1.0.0

- `info.version` moves from `1.0.0-rc.1` to `1.0.0`. No endpoint path, request/response field, enum value or state transition changed since the release candidate; API version header stays `1`.

## 2026-10-21 — Complete A0 contract package

- Added `openapi.yaml` (OpenAPI 3.1.0) for the existing user and Execution Node endpoints. The endpoint inventory is cross-checked against `CONTRACT_SKETCH.md` by `tests/contracts/test_contracts.py`.
- Added `docs/API_CONTRACT.md` to consolidate normative error behavior, RBAC, idempotency, delivery, event cursor/gap handling, task/command transitions and artifact integrity.
- Added ADR-0001 through ADR-0003 for the Control/Execution Plane boundary, storage split and credential lifecycle.
- No endpoint path, request field, response field, enum value or state transition was intentionally changed by this documentation work. Any future implementation mismatch must be recorded as a separate change with regression coverage.
