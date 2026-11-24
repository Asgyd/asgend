# ASGEND implementation status — iteration 14

Date: 2026-10-21

## Focus

WP2/WP8 backup and restore safety. This iteration hardens the backup format and adds a live PostgreSQL `pg_dump`/`pg_restore` drill to CI. It does **not** claim that live PostgreSQL or Docker was run in this environment.

## Changes

- `server/asgend_cp/backup.py`
  - `SHA256SUMS` must cover the database file matching manifest dialect (`db.sqlite3` or `db.pgdump`); duplicate entries, symlinked files, conflicting dump types, malformed manifests and inconsistent artifact metadata are rejected.
  - `verify_backup()` checks outer checksums, manifest schema, allowed archive entries, SHA-256 of every content-addressed blob and presence/size of each artifact referenced by the manifest.
  - SQLite backup preflight runs `PRAGMA integrity_check` and compares schema version and available-artifact inventory with the manifest.
  - Restore validates preconditions before mutation; rejects archive symlinks and symlinked blob trees; stages archive extraction privately; hashes staged files and atomically promotes blob files. SQLite database content is copied to a same-directory temporary file and atomically replaced.
  - CLI `restore --verify-only` now performs full backup verification, not only checksum-file comparison.
- `server/scripts/backup.sh`
  - Refuses to overwrite an existing backup name; creates private output directories and removes a newly created incomplete backup after a failed run.
  - Stages dump/archive data in a unique hidden directory on the artifact volume, outside `blobs/`, instead of the Compose `/tmp` tmpfs limited to 16 MiB. Temporary stage cleanup is attempted on success and failure.
- `server/scripts/restore.sh`
  - Preserves source backup file permissions instead of changing them to `0644`.
  - Uses a private hidden staging directory on the artifact volume and runs `pg_restore --list` against the staged custom dump before destructive restore.
- `server/tests/test_storage_backup.py`
  - Adds checks for missing database-dump checksums, tampered blob content even when outer checksums are refreshed, invalid SQLite dumps, preflight non-mutation, dialect mismatch and archive/destination symlink rejection.
- `server/tests/test_postgres_backup_restore.py`
  - Adds a live drill that creates disposable source/target databases, creates a custom dump with `pg_dump`, assembles the application backup plus blob archive, verifies the dump, restores with `pg_restore`, and checks restored schema, task/artifact rows, blob bytes and SHA-256.
- `.github/workflows/control-plane-node-e2e.yml`
  - Installs PostgreSQL client tools in the PostgreSQL acceptance job and runs the live dump/restore drill after migration/restart/redelivery acceptance.
- Updated `TODO.md`, `README.md`, and `server/DEPLOYMENT.md`.

## Checks run locally

- Backup/migration tests: **14/14 passed**.
- Full server unittest suite: **20 passed, 3 skipped**. Skips: compiled C-core test in standalone discovery (it passes under CTest with the built executable), PostgreSQL migration/restart acceptance, and PostgreSQL dump/restore drill.
- API contract tests: **30 passed, 1 skipped**.
- Node-agent suite: **37/37 passed**.
- CMake Release build: passed.
- CTest excluding the long `integration` test: **17/17 passed**, including `node_agent_c_core_e2e`.
- `sh -n` for both backup/restore scripts, Python compile checks, and workflow YAML parse: passed.

## Not verified here

This environment has no `pg_dump`, `pg_restore`, Docker, or configured `ASGEND_TEST_POSTGRES_URL`. The new live PostgreSQL drill and Compose `backup.sh`/`restore.sh` sequence therefore have not been executed locally. The hosted GitHub Actions result has not been observed from this session. The long C `integration` test, Qt build, Flutter analysis/tests/APK, and final ASGEND 1.0 acceptance remain open.

## Next

1. Observe the PostgreSQL CI job and address any real `pg_dump`/`pg_restore` compatibility errors.
2. Run the full Compose backup → verification → restore → readiness → artifact-hash drill in a disposable deployment.
3. Continue platform SDK acceptance (Qt 6, Flutter/Android), the long integration test and the 1.0 Definition-of-Done review.
