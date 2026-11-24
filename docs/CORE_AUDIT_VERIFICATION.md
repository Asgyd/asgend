# Core Audit Verification Record

**Re-audit date:** 2026-10-23  
**Input:** source archive supplied for the ASGEND Core re-audit; includes HTTP contract and MCP launcher changes from this run.

## Clean Release build

```sh
cmake -S . -B build-reaudit -DCMAKE_BUILD_TYPE=Release
cmake --build build-reaudit --parallel 4
```

**Result: PASS.** All configured targets built and linked: `asgend_tui`, `test-asgend-tui`, `asgend-cli`, `asgend_memory`, `asgend-memory`, and `agent-orchestrator`.

## CTest excluding full integration

```sh
ctest --test-dir build-reaudit -E '^integration$' --output-on-failure -j 4
```

**Result: PASS, 36/36 tests in 68.70 seconds on the final tree.** This run includes the deterministic TUI list-ordering change; full integration was verified separately below.

## Python Control Plane

```sh
ASGEND_CORE_BINARY="$PWD/build-reaudit/agent-orchestrator" \
  python3 -m unittest discover -s server/tests -p 'test_*.py' -v
```

**Result: 34 tests; 31 passed and 3 skipped.** Skips: live PostgreSQL acceptance and PostgreSQL backup/restore (no `ASGEND_TEST_POSTGRES_URL` configured), and secret-file permission test (the container runs as root).

## Targeted HTTP status regression

```sh
ctest --test-dir build-reaudit -R '^core_http_status_mapping$' --output-on-failure
```

**Result: PASS, 1/1 test** after expanding coverage for malformed JSON on `POST /api/tasks`, unsupported action on `POST /api/plan`, an unknown route, HTTP 400/404, and the JSON error envelope.

## MCP launcher and policy smoke regression

```sh
ctest --test-dir build-reaudit -R '^core_mcp_launcher_smoke$' --output-on-failure
```

**Result: PASS, 1/1 test.** The runtime probe verifies parent-secret isolation, configured environment values, sandbox UID/GID and supplementary groups when run as root, CPU/address-space/file-size/open-file/core limits, default seccomp socket denial, closure of non-standard descriptors before the MCP server exec in the non-sandbox case, and supervisor restart after a first-process exit. This is focused coverage, not the complete Landlock/network policy equivalence matrix.

## SQLite fault injection and multi-process idempotency regression

```sh
ctest --test-dir build-reaudit -R '^core_db_fault_injection$' --output-on-failure
```

**Result: PASS, 1/1 test.** `tests/test_core_db_faults.c` uses SQLite's authorizer to deny the actual empty-queue `COMMIT`, verifies the error is surfaced and the connection/transaction state can be reused, leaves a live statement to force `sqlite3_close()` into `SQLITE_BUSY` and checks the captured diagnostic/deferred-close path, then runs four independent writer processes. The multi-process portion requires 80 unique inserts plus one shared idempotency row (81 task rows total). It does not yet replace sustained high-load or queue-capacity-boundary testing.

## User-reported integration failure (2026-10-23)

The submitted `install.sh` log shows a successful Release build and 36/37 CTest cases passing. `integration` failed after 78.86 seconds at `tests/integration.py:2350`, where pipe-captured CLI output was incorrectly required to contain a full-screen TUI frame. The CLI branches on `isatty(STDIN_FILENO)` and `isatty(STDOUT_FILENO)`, so the captured path correctly uses the line-oriented fallback. CORE-11 adjusts the assertion to match that contract. The log also surfaced `Mcp-Param-Region mismatch` from the generic mock call path. CORE-12 now adds the required `region` argument for the HTTP echo mock and makes the post-follow-up peer error check explicit.

CORE-11 corrects the captured non-TTY assertion; CORE-12 supplies the region argument to the HTTP MCP mock; CORE-13 adds deterministic directory-first workspace listing; CORE-14 removes the contradictory response-box requirement from the PTY `--no-tui` test. The final full-integration run passed: CTest `1/1`, 152.01 seconds, process exit code `0`. The final aggregate non-integration run also passed `36/36` in 68.70 seconds. Thus all 37 registered CTest tests passed across the two completed invocations.

## Full integration

```sh
ctest --test-dir build-reaudit -R '^integration$' --output-on-failure
```

**Result: PASS.** The latest full integration run on the tree with CORE-11/12/13/14 completed as `1/1` with exit code `0` in 152.01 seconds. The separate final non-integration CTest run completed `36/36` in 68.70 seconds. The user-supplied original failure is reproduced in the audit history and its test-contract cause is corrected. Do not treat this one pass as security certification; remaining sandbox matrix and external PostgreSQL acceptance gaps are unchanged.

## Open test gaps

- Sustained high-load and queue-capacity-boundary stress remain open; Stage 4 now has bounded four-process enqueue/idempotency coverage.
- The targeted empty-queue `COMMIT` denial and live-statement `SQLITE_BUSY` close branches are covered by `core_db_fault_injection`.
- PostgreSQL acceptance and backup/restore need a configured test database.
- The full `integration` suite still requires a successful, uninterrupted exit. The core MCP launcher smoke test is targeted coverage and does not replace it.
