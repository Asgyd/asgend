# ASGEND implementation status — iteration 11

Date: 2026-10-21

## Delivered

- Added `server/tests/test_node_agent_e2e.py`: a hermetic acceptance test using the real HTTP Control Plane, the real Python `NodeAgent`, real HTTP command lease/ack and event/artifact endpoints, and a protocol-faithful `MockCore` in place of the compiled C service.
- The test verifies heartbeat; task submission; command lease and acknowledgement; core execution; event/state projection; chunked artifact upload; server-side SHA-256 verification; one-time download-ticket creation; byte-for-byte artifact download; checksum response header; exclusion of a deliberately sensitive marker from user-visible events; and non-publication of a symlink fixture.
- Added `.github/workflows/control-plane-node-e2e.yml` with two jobs: Python contracts/server/node-agent acceptance, and Linux Release build plus CTest (excluding the known long `integration` test).
- Added a runbook section to `docs/REMOTE_CONTROL_PLANE.md` and updated `TODO.md` to distinguish implemented API handlers from the remaining live PostgreSQL/production acceptance.

## Verification results

- Control Plane/server suite: **11/11 passed**.
- Node-agent suite: **37/37 passed**.
- API contracts: **30 passed, 1 skipped** (31 tests total; skip is the optional validator check).
- Fresh CMake Release configure and build: **passed**.
- All **16 non-integration CTest tests passed**, split into two bounded groups due to a timeout during a single combined invocation: tests 2–9 passed 8/8 and tests 10–17 passed 8/8.
- Workflow YAML parsed successfully; `server/tests/test_node_agent_e2e.py` passed Python bytecode compilation.

## Scope and limitations

The new acceptance test runs the actual Control Plane server and the actual node-agent, but uses `MockCore` for the local core protocol so the test is deterministic and independent from model runtimes. The long `integration` CTest was not counted as passed. Runtime validation with the compiled C core, PostgreSQL, Docker Compose and a full backup/restore drill remains open. Flutter `analyze`/`test`/APK and Qt build/runtime also remain unverified because their SDKs are not installed in this environment.

## Next recommended work

1. Add an e2e profile that runs the node-agent against the compiled C core, reusing `tests/integration.py` fixtures where possible.
2. Add PostgreSQL service-based CI for migrations, redelivery after server restart, and backup/restore.
3. Run Flutter/Qt SDK workflows and fix build/runtime findings before 1.0.0 acceptance.
