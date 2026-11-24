# ASGEND implementation status — iteration 12

Date: 2026-10-21

## Delivered

- Added `server/tests/test_node_agent_c_core_e2e.py`, a new end-to-end acceptance test which starts the compiled C `agent-orchestrator` process with isolated configuration/database/workspace, runs the actual Python `NodeAgent` and a live HTTP Control Plane, and exercises the real TCP line-delimited JSON core API.
- Test coverage includes node heartbeat; Control Plane task submission; command lease/ack; core task creation and server-reported status; event/state projection; workspace creation by the C core; direct file listing and bounded `workspace_read_bytes`; exclusion of a workspace symlink from published artifacts; chunked artifact upload; server-side SHA-256; single-use download-ticket flow; byte-for-byte download and checksum response header.
- Uses `completed_without_model` as the expected terminal status when no LLM provider is configured. The test intentionally validates integration/transport/artifact behavior rather than model output.
- Registered the test in CMake/CTest with a 30-second timeout. CI's normal Release CTest command therefore runs it against the just-built binary. Updated `.github/workflows/control-plane-node-e2e.yml`, `README.md`, `docs/REMOTE_CONTROL_PLANE.md`, `TODO.md`, and the top-level implementation status.

## Verification results

- Fresh CMake Release configure/build: **passed**, no compiler warnings observed.
- CTest excluding the pre-existing long `integration` test: **17/17 passed** in one invocation, including `node_agent_c_core_e2e`.
- Contract tests: **30 passed, 1 skipped** (optional external OpenAPI validator).
- Server unittest discovery: **11 passed, 1 skipped**. The skipped case is the C-core e2e because discovery intentionally does not get a binary path; CTest runs that same test explicitly with the compiled target and it passes.
- Node-agent suite: **37/37 passed**.
- Workflow YAML and Python bytecode compilation: passed.

## Scope and limitations

The C-core e2e uses the real compiled C service and real Python agent, but the Control Plane storage backend is SQLite, not PostgreSQL. No LLM/provider is configured; the core task ends with `completed_without_model`, and the test does not assert model execution quality. PostgreSQL migration/upgrade concurrency, restart/redelivery, Docker Compose runtime and backup/restore drill remain open. The long `integration` CTest is deliberately excluded from this acceptance command and is not claimed as passed. Flutter/Dart/APK and Qt build/runtime acceptance still require their missing SDKs.

## Recommended next work

1. Add PostgreSQL service-based CI for migration, restart/redelivery and backup/restore acceptance.
2. Run the long `integration` suite in an appropriately bounded CI job and investigate its runtime separately.
3. Execute Flutter/Qt SDK CI and device/GUI acceptance, then complete 1.0.0 Definition of Done review.
