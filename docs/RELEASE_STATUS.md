# ASGEND 1.0.0 release readiness

- Product release: `1.0.0`
- Current candidate: `1.0.0`
- Channel: `stable`
- Release approved: `true`
- Gate manifest ready: `true`
- Decision: **READY**

## Component versions

| Component | Version | Source |
|---|---|---|
| `core` | `1.0.0` | CMakeLists.txt: project(agent_orchestrator_c VERSION) |
| `control_plane` | `1.0.0` | server/asgend_cp/__init__.py and server/deploy/compose.yaml |
| `api` | `1.0.0` | contracts/v1/openapi.yaml |
| `qt_client` | `1.0.0` | clients/asgend_qt/CMakeLists.txt and packaging/linux/org.asgend.QtClient.metainfo.xml |
| `flutter_client` | `1.0.0+2` | clients/asgend_flutter/pubspec.yaml |

## Acceptance gates

| Gate | Required | Status | Evidence / blocker |
|---|---:|---|---|
| Component version metadata is consistent (`version_consistency`) | yes | **passed** | 2026-10-22: all components at 1.0.0 (Flutter build 1.0.0+2); `python3 scripts/check_release_readiness.py` reports no version errors and `tests/test_release_gate.py` passes. |
| Release build and fast CTest suite (`core_release_build`) | yes | **passed** | 2026-10-22: clean `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` with no compiler warnings; `ctest --test-dir build -E '^integration$'` 18/18 passed (run as an unprivileged user). |
| OpenAPI and contract regression tests (`contract_tests`) | yes | **passed** | 2026-10-22: `python3 tests/contracts/test_contracts.py` — 31 passed, 1 optional validator test skipped. |
| Control Plane server test suite (`control_plane_tests`) | yes | **passed** | 2026-10-22: `python3 -m unittest discover -s server/tests -p 'test_*.py'` — 34 run, 31 passed, 3 skipped by design in plain discovery (compiled-core E2E without a binary path and the two live PostgreSQL tests, which pass in their own gates below). |
| Node-agent protocol and resilience tests (`node_agent_tests`) | yes | **passed** | 2026-10-22: `python3 -m unittest discover -s node_agent/tests -p 'test_*.py'` — 37/37 passed. |
| Compiled C core ↔ NodeAgent ↔ Control Plane E2E (`compiled_core_e2e`) | yes | **passed** | 2026-10-22: CTest `node_agent_c_core_e2e` and `python3 server/tests/test_node_agent_c_core_e2e.py build/agent-orchestrator` passed against the clean Release build. |
| Live PostgreSQL migrations, restart and command-redelivery acceptance (`postgres_acceptance`) | yes | **passed** | 2026-10-22: PostgreSQL 16.15 (postgres:16-alpine), psycopg 3; `ASGEND_TEST_POSTGRES_URL=… python3 -m unittest discover -s server/tests -p test_postgres_acceptance.py -v` — 2/2 passed (v1 → latest upgrade, repeat migration no-op, restart, lease expiry/redelivery, idempotent submit, ack/event persistence). |
| Live pg_dump/pg_restore with artifact hash verification (`postgres_backup_restore`) | yes | **passed** | 2026-10-22: `test_postgres_backup_restore.py` 1/1 passed against PostgreSQL 16.15 with pg_dump/pg_restore 16.15. A PG18 client against the PG16 server fails on `transaction_timeout`; the test now reports such a client/server mismatch explicitly, and production scripts run the tools inside the server container. |
| Docker Compose deployment and end-to-end backup/restore/readiness drill (`compose_runtime`) | yes | **passed** | 2026-10-22: `sh server/scripts/compose_drill.sh` passed: image build, migrate ordering, healthy non-root/read-only containers, seed through NGINX TLS, backup.sh, post-backup mutation, restore.sh --verify-only and destructive restore, API restart, artifact bytes/SHA-256 and node token preserved, mutation rolled back, 200-request probe with 0 errors, no tokens or download tickets in logs. Six deployment defects found by this drill were fixed (see docs/SECURITY_OPERATIONS.md). |
| Long-running core integration test (`long_integration`) | yes | **passed** | 2026-10-22: `ctest --test-dir build -R '^integration$' --timeout 900` passed in 119 s as an unprivileged user. As root it fails by design: the MCP sandbox drops children to UID 65534, which cannot read a private home directory. CI job `core-integration` added. |
| Qt 6 client build, tests and package install smoke test (`qt_client`) | yes | **passed** | 2026-10-22: builds without warnings and Qt Test 8/8 passes on Qt 6.4.2 (ubuntu:24.04), Qt 6.10.2 (fedora:42) and Qt 6.12 (Arch); TGZ+DEB and RPM generated; DEB installed, launched (offscreen, still running after the timeout) and removed in a clean ubuntu:24.04 container, RPM likewise in fedora:42. Two compile errors fixed. CI now builds the DEB and runs the install smoke test. |
| Flutter analyze, widget/unit tests and unsigned release APK build (`flutter_android`) | yes | **passed** | 2026-10-22, Flutter 3.29.3: `flutter analyze --no-pub` no issues, `flutter test --no-pub` 20/20, `./scripts/bootstrap_android_wrapper.sh && flutter build apk --release --no-pub` produced unsigned app-release.apk (versionName 1.0.0, versionCode 2, minSdk 24, INTERNET only). A re-signed copy launched on an Android 14 (API 34) emulator without crashes. Code was brought back to the pinned SDK's APIs. |
| Independent human-judged retrieval and query-planner quality evaluation (`memory_quality`) | no | **not_run** | Maintainer decision 2026-10-22: `query --plan` and `--rerank` ship as EXPERIMENTAL in 1.0.0 (CLI help, README, docs/MEMORY_INDEX.md); default agent retrieval is local and covered by synthetic quality fixtures. A human-judged evaluation is scheduled after 1.0.0. |
| Cross-architecture build or documented supported-target matrix (`cross_architecture`) | no | **not_run** | 1.0.0 supported target: Linux x86_64 (core, Control Plane image, Qt packages) and Android minSdk 24 (APK). aarch64 builds are post-1.0.0. |
| Production security/logging review and representative load/quota test (`security_operations_review`) | yes | **passed** | 2026-10-22 review of the reference Compose stack (docs/SECURITY_OPERATIONS.md): six deployment defects fixed, including download tickets leaking into NGINX access logs; load ramp through NGINX TLS (1–4 tokens, 4–64 workers): per-token 429 after the burst, no 5xx/transport errors, 1 600/1 600 OK across four tokens at ~145 req/s on 2 vCPU, CP ≈27 MiB, 15/16 DB connections; drill verifies no secrets in logs; decision: one Control Plane replica per database in 1.0.0. Per-deployment checks are listed for operators; release approval is recorded in release/versions.json. |

## Release blockers

No required blockers recorded. The maintainer approved this stable release.

This report is derived from `release/versions.json` and `release/1.0.0-gates.json`.
Do not change a gate to `passed` without a reproducible run or an observed hosted CI result. Source files existing is not acceptance evidence.
