# ASGEND 1.0.0 Definition of Done

**Current decision: STABLE — approved by the maintainer on 2026-10-22.** All required gates passed; every component is versioned `1.0.0` (Flutter build `1.0.0+2`); `release/versions.json` has `release_approved: true`, `release_channel: "stable"`. Tagging `v1.0.0` triggers the `release-gate` workflow, which re-runs `scripts/check_release_readiness.py --strict`.

The machine-readable source of truth is [`../release/1.0.0-gates.json`](../release/1.0.0-gates.json). The generated status report is [`RELEASE_STATUS.md`](RELEASE_STATUS.md). Full run log and environment: [`../IMPLEMENTATION-STATUS-2026-10-22-ITERATION-17.md`](../IMPLEMENTATION-STATUS-2026-10-22-ITERATION-17.md).

```sh
python3 scripts/check_release_readiness.py --write-report
python3 scripts/check_release_readiness.py --strict   # exits 0 for an approved release with all required gates passed
```

## Required gates

### Core and protocol

- [x] Clean Release CMake build (no compiler warnings) and CTest **19/19**, including the long `integration` scenario and `node_agent_c_core_e2e`. Run CTest as an unprivileged user: as root, the MCP sandbox drops children to UID 65534 by design.
- [x] Contract, Control Plane, node-agent and release-gate suites.
- [x] PDF/OOXML extraction: unsupported or partially parsed documents are reported as `unsupported` / `parsed_partial` rather than silently indexed; broader real-corpus review is listed after 1.0.0.
- [x] Decision-model retrieval features (`query --plan`, `--rerank`) are documented as **experimental** in 1.0.0; default retrieval is local and covered by synthetic quality fixtures.

### Control Plane and operations

- [x] Live PostgreSQL 16 migration/restart/redelivery acceptance.
- [x] Live `pg_dump`/`pg_restore` drill with schema/data and artifact byte/SHA-256 checks (client tools must match the server major version; the test says so explicitly).
- [x] Docker Compose in a disposable environment: readiness, TLS proxy, migration ordering, non-root/read-only containers, clean restart.
- [x] Backup → `--verify-only` → destructive restore → readiness → artifact download/hash round-trip (`server/scripts/compose_drill.sh`).
- [x] Security/log-redaction review and load/quota/rate-limit test on the reference stack ([`SECURITY_OPERATIONS.md`](SECURITY_OPERATIONS.md)); 1.0.0 supports a single Control Plane replica.

### Client platforms

- [x] Qt 6 build, unit tests, TGZ/DEB/RPM packages; install/launch/remove smoke on clean Ubuntu 24.04 and Fedora 42 containers.
- [x] Flutter 3.29.3 analyze (0 issues), unit/widget tests, unsigned release APK; launch smoke on an Android 14 emulator (APK re-signed with a throwaway test key).
- [x] Packaging scope: TGZ, DEB and RPM are supported for 1.0.0; AppImage, Flatpak and PKGBUILD are post-1.0.0.

### Release governance

- [x] Component versions are explicit and audited by `scripts/check_release_readiness.py`.
- [x] Every required gate in `release/1.0.0-gates.json` carries a reproducible command and date.
- [x] Release notes, version sources and package metadata updated.
- [x] Maintainer approval (2026-10-22). Remaining action outside this checkout: tag `v1.0.0` in the repository.

## Release decision rules

1. A source file, test file or configured workflow is not acceptance evidence by itself.
2. Required gates must be `passed` and include a reproducible run or a hosted CI URL/log before a stable tag is created.
3. Missing toolchains/services are `blocked`, not `passed` or silently skipped.
4. Optional items may remain open only if scope and support policy are explicitly documented in release notes.
