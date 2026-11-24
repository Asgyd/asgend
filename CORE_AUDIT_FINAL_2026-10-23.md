# ASGEND Core — Final Re-audit Report

**Audit date:** 2026-10-23  
**Snapshot:** previous cumulative project snapshot plus the incremental regression fixes described in this report  
**Scope:** C orchestration core (`src/server.c`, `src/db.c`, `src/llm.c`, `src/mcp.c`, `src/lsp.c`, `src/models.c`, `src/config.c`, `src/orchestrator.c`, `src/memory/*`), Python Control Plane (`server/asgend_cp/*`), runtime tests, build/test integration, and release/packaging documentation.

## Executive summary

The supplied tree contains prior fixes for task enqueue atomicity, shutdown with idle clients, strict configuration strings, provider error-body privacy, HTTP status mapping, and observable SQLite close handling. A fresh Release build succeeds. The 36 non-`integration` CTest cases pass; the Python Control Plane suite has 34 tests with 3 environment-dependent skips.

The user's latest local run exposed an integration-test contract failure after 78.86 seconds: pipe-captured CLI output was incorrectly required to include a full-screen TUI frame (CORE-11). Review of subsequent test runs exposed two related issues in test/UI contracts: filesystem entry order was inherited from `readdir()` and made coordinate-based navigation nondeterministic (CORE-13), and a PTY test invoked the explicitly line-oriented `--no-tui` mode while requiring a decorative response frame (CORE-14). The UI list now sorts directories first with deterministic name ordering, and the PTY test checks streamed content and redraw behavior without requiring a box. The user's log also showed an HTTP MCP mock invocation missing its required `region` argument; CORE-12 fixes that fixture and asserts no protocol errors after follow-up. The focused changes build successfully. The final full integration run passed 1/1 in 152.01 seconds with exit code `0`. The final aggregate run of the 36 non-integration CTest cases passed in 68.70 seconds. Together with the separate full-integration run, every one of the 37 registered CTest tests passed in its latest completed run. The MCP post-fork hazard has been remediated by moving environment/sandbox setup into a dedicated single-threaded launcher image; focused tests cover environment isolation, sandbox identity/resource limits, denied networking, descriptor closure and supervisor restart. The full sandbox equivalence matrix and some additional SQLite stress/failure-injection cases remain open. The codebase also has large translation units that make lifecycle invariants harder to review.

This is a source audit and reproducible test run, not a formal security certification or penetration test.

## Review decomposition

The subsystem-by-subsystem review map, severity/disposition matrix, dependency order and acceptance gates are in [`docs/CORE_REVIEW_DECOMPOSITION.md`](docs/CORE_REVIEW_DECOMPOSITION.md). The staged implementation checklist is in [`docs/CORE_REVIEW_TODO.md`](docs/CORE_REVIEW_TODO.md).

## Findings and disposition

### CORE-01 — Task submission transaction invariants — High; implementation present

`src/db.c::db_enqueue()` previously performed idempotency lookup, queue capacity check, workspace setup, task insert and optional session upsert without one write transaction. The current tree wraps the database mutations in a checked `BEGIN IMMEDIATE` transaction and rolls back on failure. `tests/test_core_db_atomicity.py` exercises a failed session upsert and asserts rollback, workspace cleanup, connection reuse and idempotency-key reuse.

**Verification added in Stage 4:** a four-process test concurrently enqueues 80 distinct tasks and races on one shared idempotency key; the final database contains 81 rows and exactly one row for the shared key. Higher-load capacity-boundary stress remains open.

### CORE-02 — Empty-queue claim `COMMIT` failure handling — Medium; code fix present, test gap open

`src/db.c::db_claim_next()` now checks the empty-queue `COMMIT`, attempts rollback and reports a database error instead of treating a failed commit as “no work”. The Stage 4 `core_db_fault_injection` test installs a SQLite authorizer that denies the actual `COMMIT`, asserts the claim returns a database error instead of “empty queue”, removes the authorizer, then verifies the connection and transaction state are reusable.

### CORE-03 — Graceful shutdown with idle accepted clients — High; addressed and runtime-tested

`src/server.c` tracks accepted descriptors and shuts them down when shutdown begins, leaving close/release ownership with each connection thread. `core_shutdown_idle_client` passes and covers a client that does not complete a request before SIGTERM.

### CORE-04 — Strict configuration string validation — Medium; addressed and tested

Explicit malformed or overlong values are rejected rather than silently replaced by defaults; diagnostics identify the key without printing supplied secret content. `core_config_strings` passes.

### CORE-05 — Provider error-body privacy — Medium; addressed and runtime-tested

Reviewed LLM/System One non-2xx paths no longer copy arbitrary provider response text into ordinary task error/event/log strings. `core_provider_error_privacy` passes.

### CORE-06 — HTTP status semantics for RPC errors — Low/Medium; partially tested

`src/server.c::http_status_for_rpc_result()` maps common application errors to stable status classes while preserving the JSON error envelope. The mapper now fails closed with HTTP 500 when an internal result lacks a boolean `ok` field (implemented with CORE-09). Runtime coverage directly verifies HTTP 400/404 and malformed internal result handling is still not exhaustive across the full mapping table.

### CORE-07 — SQLite close failure observability — Low/Medium; code fix present, test gap open

`db_close()` checks `sqlite3_close()` and logs an unexpected busy result before attempting `sqlite3_close_v2()`. The Stage 4 test keeps a prepared statement open to force `sqlite3_close()` to report `SQLITE_BUSY`, captures and checks the diagnostic, verifies `rt->db` is cleared after deferred `sqlite3_close_v2()`, and finalizes the statement successfully.

### CORE-08 — Large translation units and lifecycle coupling — Maintainability; open

`src/server.c`, `src/mcp.c`, `src/models.c`, `src/llm.c` and multiple `src/memory/*.c` files combine protocol parsing, lifecycle management and state mutation. The existing `docs/CORE_MODULE_EXTRACTION_PLAN.md` appropriately recommends incremental extraction only after ownership and boundary tests exist. No structural extraction is included in this audit baseline.

### CORE-09 — Invalid body on known POST routes returned 404 — Low/Medium; fixed in this re-audit

In the original snapshot, malformed JSON or an unsupported action on known endpoints such as `POST /api/tasks` or `POST /api/plan` could leave `command == NULL`; the shared fallback then returned `404 Not Found`, conflating an invalid request with an unknown route.

**Fix:** `src/server.c::http_handle_connection()` now tracks route-failure semantics. Invalid bodies/actions on recognized POST endpoints return `400 Bad Request` with the existing JSON error envelope, while unknown paths retain `404 Not Found` and an accurate `unknown route` message. `http_status_for_rpc_result()` also now requires `ok` to exist and be a JSON boolean; missing/malformed markers return `500` rather than being reported as `200`.

**Verification:** `tests/test_core_http_status_mapping.py` now exercises malformed JSON on `POST /api/tasks`, an unsupported action on `POST /api/plan`, an unknown route, validation HTTP 400, missing task HTTP 404, and the `ok:false` error envelope. The targeted test and all 36 non-integration CTest cases pass.

### CORE-10 — MCP post-fork environment/sandbox setup — High; launch path remediated, parity coverage partial

The original `src/mcp.c::start_server()` child branch ran `clearenv()`/`setenv()`, sandbox setup, formatted diagnostics and `execvp()` after `fork()` from a multithreaded service. The supervisor can start or restart MCP children from worker threads, making this a credible deadlock/undefined-state risk.

**Fix implemented:** the fork child now uses only `setpgid`, `dup2`, `fcntl`, `write`, `close`/`close_range`, `execve` and `_exit`-style failure handling. It execs the same core binary in a dedicated `--mcp-launcher` mode and passes a bounded, fixed-layout launch payload through descriptor 3. The helper is a fresh single-threaded image, validates the payload, reconstructs only the MCP-specific environment/config, reuses the existing UID/GID drop, resource limits, Landlock and seccomp setup, then `execvp()`s the configured server. Parent service environment and unrelated API credentials are not inherited by the helper. The process group and MCP stdin/stdout channels remain intact.

**Verification:** `tests/test_core_mcp_launcher.py` checks that a parent-secret sentinel is not inherited; configured MCP environment variables survive; root-run sandbox children drop to UID/GID 65534 with empty supplementary groups; CPU/address-space/file-size/open-file/core limits are applied; default seccomp blocks socket creation; the final non-sandbox MCP sees no descriptors beyond standard streams; and the supervisor restarts an MCP after its first process exits. The full integration suite subsequently passed after the CORE-11–CORE-14 follow-up changes; full sandbox policy equivalence remains open.

**Residual risk / next step:** the focused smoke matrix does not yet exercise every Landlock allow/deny path, every helper setup failure, descriptor closure under all launch configurations, or repeated restart during concurrent load. Keep the full sandbox parity matrix as follow-up; do not interpret this implementation as a formal security certification.

### CORE-11 — Integration assertion contradicts the CLI's non-TTY mode — Test correctness; fixed in this iteration

The failing assertion in `tests/integration.py` required `"╭─ ASGEND"` and `"│"` in output captured through `subprocess.run` pipes. In this invocation, `stdin` and `stdout` are not TTYs, so `clients/asgend_cli.c::main()` intentionally selects the line-oriented menu; the full-screen TUI is exercised in the adjacent PTY regressions. The test therefore enforced mutually incompatible conditions rather than exposing a production rendering failure.

**Fix:** assert the line-oriented greeting and menu in the pipe-captured path and explicitly reject the alternate-screen entry sequence there. Keep the PTY tests responsible for asserting actual full-screen/redraw behavior.

**Acceptance:** full `integration` must be rerun after this test correction; other checks from the user's CTest summary passed, but integration coverage after the first failure remained unexecuted.

### CORE-12 — Generic mock HTTP MCP call omitted required `region` — Test-fixture defect; fixed in Stage 6

The user's diagnostic output showed a generic mock-model tool call to `mcp__httpserver__echo` with only `text`, while the test MCP server requires the `region` argument to be transported in `Mcp-Param-Region`. The resulting `Mcp-Param-Region mismatch` was emitted by the integration fixture, not the production MCP transport. The generic mock call now includes `region: "us-west1"` only when the selected tool is the HTTP echo tool. The session-follow-up path also asserts that the mock HTTP peer's error list remains empty. Full integration must still be rerun after this correction.

### CORE-13 — Workspace browser ordering depended on filesystem enumeration — UI determinism; fixed in this iteration

The file browser received entries in raw filesystem enumeration order. Since `readdir()` ordering is not guaranteed, the UI and coordinate-based integration scenario could disagree about which item occupied the first row; a click intended for a directory could instead select a file and make Enter appear ineffective.

**Fix:** `asgend_tui_set_files()` sorts entries by kind (directories before files), then by case-insensitive path with bytewise tie-breaking for a deterministic total order. The TUI library test now supplies deliberately unsorted entries and asserts the stable directory-first order and path navigation.

**Verification:** Release rebuild succeeded; focused TUI library, visual-contract and CLI terminal-layout tests passed in the targeted run. Aggregate acceptance must use a completed CTest run from the latest tree, not be inferred from targeted tests.

### CORE-14 — PTY test required a response box from `--no-tui` mode — Test correctness; fixed in this iteration

`run_cli_pty_regression()` allocates a PTY to verify carriage-return spinner redraws, but launches `asgend-cli --no-tui`. The test previously required the decorative `╭─ ASGEND · ответ модели` frame even though this option intentionally selects line-oriented output. This repeated the same mode-contract mistake as CORE-11, through a PTY rather than `capture_output=True`.

**Fix:** the PTY test now checks the streamed mock response, completion marker, spinner redraw bytes, and status/result text while explicitly asserting that the answer box and alternate-screen entry are absent in `--no-tui` mode. Full-screen frame expectations remain the responsibility of genuine TUI-mode tests.

**Acceptance:** PASSED in the final full integration run (1/1, exit code 0, 152.01 seconds).

## Verification record

The commands below were rerun against the supplied source tree after the HTTP API correction and MCP launcher implementation. Exact outcomes are recorded in `docs/CORE_AUDIT_VERIFICATION.md`.

| Check | Result |
|---|---|
| Clean CMake Release configure/build | PASS; all targets built and linked |
| CTest excluding `integration` on the final tree | PASS, 36/36 in 68.70 seconds |
| Python Control Plane tests with `ASGEND_CORE_BINARY` set | 34 total; 31 passed, 3 skipped for environment reasons (PostgreSQL URL absent; root-mode secret permission check skipped) |
| Full `integration` CTest | PASS, 1/1; exit code 0; 152.01 seconds after CORE-11/12/13/14 |
| Live PostgreSQL acceptance/backup-restore | SKIPPED; `ASGEND_TEST_POSTGRES_URL` not configured |
| Deterministic SQLite empty-queue `COMMIT` denial and `SQLITE_BUSY` close test | PASS; multi-process enqueue/idempotency test also passes |

## Ordered remediation TODO

1. **HTTP API contract correction — DONE:** recognized POST routes return 400 for malformed bodies/actions; unknown routes remain 404; malformed internal `ok` markers fail closed with 500. Acceptance met: targeted runtime regression and all 36 non-integration CTest cases pass.
2. **MCP post-fork safety — IMPLEMENTED; broader parity coverage OPEN:** setup now runs in a fresh single-threaded launcher, and targeted tests cover environment isolation, core sandbox limits/identity, default network denial, descriptor closure and restart. Follow up with full Landlock allow/deny testing, fault injection for each setup failure and repeated restart under concurrent load.
3. **SQLite fault injection — DONE for the targeted branches:** `core_db_fault_injection` forces the real empty-queue `COMMIT` failure via SQLite authorizer, verifies DB transaction/connection reuse, forces `SQLITE_BUSY` by leaving a statement outstanding and checks the diagnostic/deferred close path. It also races four independent producer processes for distinct and shared idempotency keys (81 expected rows total). Larger capacity-boundary and sustained-load stress remains open.
4. **Integration completion:** rerun full `integration` in an environment that permits the configured runtime; record the final test and exit code. Never mark it passed based only on progress output.
5. **Optional environment gates:** run PostgreSQL acceptance and backup/restore with `ASGEND_TEST_POSTGRES_URL`; run secret-file permission tests as a non-root user.
6. **Module extraction:** perform only one boundary at a time from `docs/CORE_MODULE_EXTRACTION_PLAN.md`, after its contract tests are in place.

## Stage 4 — SQLite fault injection and cross-process idempotency stress

`tests/test_core_db_faults.c` and CTest target `core_db_fault_injection` are new. No production-only fault hook was added: SQLite’s authorizer denies `COMMIT` for the test connection, then is removed to assert the same connection can process a subsequent empty-queue claim and a transaction. A second fixture leaves a prepared statement outstanding so `sqlite3_close()` reaches the actual `SQLITE_BUSY` branch; the test captures the warning, checks the runtime handle is cleared by deferred close, and successfully finalizes the statement. Finally, four child processes open the same SQLite database independently, insert 20 unique tasks each, and concurrently submit one common idempotency key. The check requires 81 total task rows and exactly one row for that shared key.

**Stage 4 test:** `ctest --test-dir build-reaudit -R '^core_db_fault_injection$' --output-on-failure` — PASS, 1/1. Full non-integration CTest — PASS, 36/36.

## Audit limitations

- The local environment does not expose an independent subagent launcher. No external-agent assessment is claimed.
- No live PostgreSQL instance was configured; the two PostgreSQL-dependent tests were skipped.
- Earlier full-integration attempts were interrupted or failed at test-contract assertions; the final corrected run passed 1/1 in 152.01 seconds with exit code 0. Full sandbox parity and external PostgreSQL acceptance remain unverified.
- “No defect confirmed” in an inspected area must not be interpreted as proof of absence of vulnerabilities.


## Update: client workflow trigger coverage (2026-10-25)

A cross-client CI path-filter gap was found and fixed in native Android, Flutter Android, and Qt workflows. Both `push` and `pull_request` now trigger for core/API sources, public headers, the Control Plane, OpenAPI/protocol contracts, contract tests, shared API documentation, and root CMake changes. The regression guard is `tests/test_client_workflow_contract_triggers.py` / CTest `client_workflow_contract_triggers`. See `docs/PROJECT_AUDIT_STAGE_12_CLIENT_CI_TRIGGERS.md`; hosted workflow execution requires a pushed commit/PR.


## Whole-project verification update (2026-10-25, Stage 12)

A fresh Release build completed successfully with GCC 14.2. All registered tests passed in one CTest invocation: **40/40 PASS, 222.28 seconds, exit code 0**, including `integration` (132.73 s), `core_llm_sse_watchdog` (39.73 s), and the new `client_workflow_contract_triggers`. Native Android, Flutter Android, and Qt workflows now schedule validation on shared API/core contract changes. See `docs/PROJECT_AUDIT_STAGE_12_CLIENT_CI_TRIGGERS.md` for exact scope and evidence. The local result does not claim that hosted GitHub Actions ran, because no commit/PR was pushed from this environment.
