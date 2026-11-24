# ASGEND — implementation status, iteration 3 (21 October 2026)

## Scope

Work continued from `TODO.md`, targeting WP6/M5 lifecycle and observability. This iteration does not claim ASGEND 1.0 release readiness.

## Implemented

- Added `asgend-memory stats ROOT`, returning machine-readable JSON with schema version, root/database paths, database/WAL/SHM bytes, active/deleted file counts, current extraction counts, current/stale chunks and vectors, current/stale graph edges, pipeline job counts grouped by stage/state, and last-scan metrics.
- Added `asgend-memory gc ROOT [--vacuum]`. It runs cleanup inside `BEGIN IMMEDIATE`; removes stale chunks, chunk states, document/chunk vectors, pipeline jobs, graph edges, and inactive search documents/FTS postings. `--vacuum` additionally optimizes FTS5, checkpoints/truncates WAL, and runs `VACUUM`.
- Cleanup deliberately preserves `files` rows/tombstones, `scan_runs`, `file_snapshots`, `file_events`, and historical `descriptions`.
- Added `tests/test_memory_gc.py`, testing JSON stats, stale chunk/vector/job cleanup, tombstone/history retention, `--vacuum`, and SQLite `integrity_check`.
- Registered `memory_gc_smoke` in CTest and documented the new commands in `README.md` and `docs/MEMORY_INDEX.md`.
- Updated `TODO.md` and `docs/ROADMAP.md`; reconciled WP3 checkboxes with the implemented node-agent modules and 37 passing tests. Full e2e against a live Control Plane remains open.

## Verification in this environment

- `cmake -S . -B build-iteration-3 -DCMAKE_BUILD_TYPE=Release`: passed.
- `cmake --build build-iteration-3 -j2`: all targets built successfully; no compiler warnings were emitted in the build log.
- `ctest --test-dir build-iteration-3 --output-on-failure -E '^integration$'`: **12/12 passed**.
- `python3 tests/contracts/test_contracts.py`: **31 passed, 1 skipped** (optional OpenAPI validator unavailable).
- `python3 -m unittest discover -s server/tests -p 'test_*.py' -v`: **10/10 passed**.
- `python3 -m unittest discover -s node_agent/tests -p 'test_*.py' -v`: **37/37 passed**.
- The new stats/gc regression test passes with a real temporary SQLite index and `PRAGMA integrity_check = ok` after garbage collection and VACUUM.

## Not claimed as complete

- The long `integration` CTest was excluded from the short regression run; it has not been confirmed in this iteration.
- Docker image build/Compose startup, live PostgreSQL upgrade and backup/restore drill, and TLS handshake need a Docker/PostgreSQL-capable target environment.
- End-to-end acceptance against a live WP2 Control Plane remains open.
- Qt/Android build and packaging remain unverified here.
- PDF/Office extractors and decision-model `query --plan` remain open WP6 items; ANN tuning against a representative real-world corpus and long-running pipeline monitoring also remain.
- Cross-architecture C cross-compilation was not performed. The verified check in this iteration is the native Linux CMake Release build.
