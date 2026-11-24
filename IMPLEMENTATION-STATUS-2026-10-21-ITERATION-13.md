# ASGEND implementation status — iteration 13

Date: 2026-10-21

## Summary

Added a PostgreSQL-backed Control Plane acceptance test and a dedicated GitHub Actions job using PostgreSQL 16. The test exercises schema upgrade, process restart, durable command delivery state, lease-expiry redelivery, submission idempotency, acknowledgement persistence, and event ordering. The test creates an isolated temporary schema and drops it during cleanup; it must only target a disposable test database.

## Changes

- `server/tests/test_postgres_acceptance.py`
  - Builds a schema-specific PostgreSQL DSN while preserving connection options.
  - Starts from migration v1 and verifies startup applies remaining migrations to `LATEST_VERSION`.
  - Verifies re-running migrations is a no-op.
  - Submits a task, leases its command, stops/restarts the real HTTP Control Plane, advances the test clock past lease expiry, and checks that the same command is redelivered with a new lease and incremented delivery attempt count.
  - Acknowledges the command, repeats the idempotent submit, restarts again, and verifies task state, core task ID, acknowledgement and ordered event IDs persisted.
- `.github/workflows/control-plane-node-e2e.yml`
  - Adds an independent PostgreSQL 16 service job and installs `psycopg[binary]`.
  - Runs the live PostgreSQL acceptance test separately from SQLite unit tests.
- `server/DEPLOYMENT.md`, `README.md`, `TODO.md`
  - Document the job, local invocation, and disposable-database warning.

## Verification performed in this environment

- CMake Release configure/build: passed.
- CTest excluding the long-running `integration`: **17/17 passed**, including `node_agent_c_core_e2e` with the compiled `agent-orchestrator`.
- API contracts: **31 passed, 1 skipped**.
- Server tests: **12 passed, 2 skipped**. The skipped tests are the PostgreSQL live acceptance (no `ASGEND_TEST_POSTGRES_URL`/PostgreSQL server available here) and the Python-discovery version of the C-core e2e test (its binary was exercised by CTest).
- Node-agent tests: **37/37 passed**.
- Workflow YAML parsing and `py_compile` for the new test: passed.
- The local DSN helper unit test passed.

## Not verified here

The PostgreSQL live test itself was not executed locally: this environment has no PostgreSQL server/client, Docker runtime, or configured `ASGEND_TEST_POSTGRES_URL`. The GitHub Actions job is added but has not been remotely executed from this chat, so it is not claimed as passing. Docker Compose runtime, actual PostgreSQL `pg_dump`/`pg_restore` recovery drill, the long-running core `integration` test, Qt build, Flutter analysis/tests/APK and the final 1.0.0 Definition of Done remain open.

## Next steps

1. Run the new PostgreSQL CI job and fix any backend-specific SQL or DSN issues it reveals.
2. Add a disposable PostgreSQL dump/restore drill that validates both database state and artifact hashes across restore.
3. Run the long-running core integration test and complete the Qt/Flutter SDK-enabled acceptance tasks.
