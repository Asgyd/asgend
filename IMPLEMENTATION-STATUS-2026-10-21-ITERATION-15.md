# ASGEND implementation status — iteration 15

Date: 2026-10-21

## Focus

WP8 Definition-of-Done review and release/version governance. This iteration does not claim stable `1.0.0`: required live PostgreSQL, Docker Compose recovery, Qt/Flutter SDK, long integration, retrieval-quality, and operational security/load gates remain open.

## Changes

- Added `release/versions.json` as an explicit map of product candidate and independently versioned components.
  - Product target: `1.0.0`; active release candidate: `1.0.0-rc.1`.
  - Core and Qt component versions remain `0.9.9`; Flutter package version remains `0.7.0+1`.
  - Control Plane Python version and Compose image tags were changed from `1.0.0` to `1.0.0-rc.1`, matching the OpenAPI metadata. This avoids presenting the unaccepted server as a stable release.
- Added `release/1.0.0-gates.json`, a machine-readable register of required and optional acceptance gates with statuses and evidence/blocker descriptions.
- Added `scripts/check_release_readiness.py` to audit source-version consistency, validate the gate register and emit a readiness summary. `--strict` exits non-zero while any required gate or maintainer approval is outstanding.
- Added `tests/test_release_gate.py` and registered `release_gate_smoke` in CTest.
- Added `.github/workflows/release-gate.yml`; pushes of the stable `v1.0.0` tag are blocked unless the strict gate passes. The regular Control Plane workflow also checks version metadata and the gate script regression tests.
- Added `docs/RELEASE_CHECKLIST_1.0.md` and generated `docs/RELEASE_STATUS.md`; updated README, ROADMAP, IMPLEMENTATION_PLAN, DEPLOYMENT and TODO to state the actual release condition.

## Checks executed locally

- Clean Release configure/build: passed.
- CTest excluding the long integration test: **18/18 passed**, including compiled C-core ↔ node-agent ↔ Control Plane E2E and `release_gate_smoke`.
- Contract tests: **31 passed, 1 skipped** (optional external validator).
- Control Plane tests: **20 passed, 3 skipped** (compiled-core test is run through CTest; PostgreSQL live migration/restart and dump/restore drills need the external database).
- Node-agent tests: **37/37 passed**.
- Release-gate tests: **5/5 passed**, including an assertion that `--strict` rejects an unapproved release.
- Python compile checks, `sh -n` for backup/restore scripts, XML parsing, JSON parsing and workflow YAML parsing: passed.
- `scripts/check_release_readiness.py --strict` returned exit code 2 as expected while release gates are open; audit found no version inconsistencies.

## Open blockers

1. Observe green hosted PostgreSQL migration/restart/redelivery and `pg_dump`/`pg_restore` CI jobs.
2. Run real Docker Compose readiness and full backup → verify → restore → readiness → artifact hash round-trip.
3. Run the long core `integration` test to completion.
4. Obtain a green Qt 6 build/test/package install check and Flutter analyze/widget-test/unsigned APK build; execute Android pairing/session acceptance.
5. Evaluate PDF/OOXML and query planning with representative documents and an independent human-judged retrieval corpus/provider.
6. Complete production security/logging review and representative load/quota/rate-limit testing.

No hosted CI run was observed from this session. The overall release remains **BLOCKED**, and stable `1.0.0` is not assigned.
