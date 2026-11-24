# Core Review TODO

## Scope and rule

Work from highest-risk correctness/availability fixes to defensive hardening. Each stage has an isolated archive and its own targeted tests. Avoid broad rewrites while fixing lifecycle invariants. Keep `docs/CORE_AUDIT.md` updated when findings are closed or their status changes.

## Stage A — Audit and baseline

- [x] Map core components: HTTP/RPC entry points, worker queue, SQLite task state, LLM/SSE, MCP/LSP supervisors, memory store, and Python Control Plane.
- [x] Record confirmed and potential findings with code locations, impact and remediation.
- [x] Define regression gates for each finding.
- [x] Initial baseline run: clean Release build, CTest excluding long integration (34/34 before adding the MCP launcher test), and Python Control Plane suite (34 tests; 31 passed, 3 environment-dependent skips).
- [ ] Complete the full `integration` test in the target environment; the local execution limit was 240 seconds and a pass was not confirmed.

Acceptance: audit report distinguishes observed evidence from risk hypotheses and records the baseline limitations.

## Stage B — Atomic task submission and claim handling (High)

- [x] Wrap task idempotency lookup, queue-capacity check, workspace association, task insert and session upsert in a checked SQLite `BEGIN IMMEDIATE` transaction.
- [x] Roll back on every failure; remove only an empty workspace created by the failed enqueue.
- [x] Check the empty-queue `COMMIT` in `db_claim_next()` and report/rollback on failure.
- [x] Add a runtime regression test injecting a failed session upsert after task insert, asserting rollback, workspace cleanup, connection reuse, and idempotency-key reuse. Stage J adds deterministic commit-denial coverage via SQLite authorizer and checks connection reuse after rollback.

Acceptance: no partially committed task is reported as a failed submission; a claim error is not silently reported as an empty queue. Status: implementation and rollback regression test complete.

## Stage C — Bounded graceful shutdown (High)

- [x] Wake already accepted TCP/HTTP/Unix clients when shutdown begins by shutting down their tracked descriptors under `connections_mutex`.
- [x] Keep descriptor close/release ownership in `connection_main()` to avoid double-close races.
- [x] Add an integration test holding an idle socket (60-second request timeout) while sending SIGTERM; assert process exit within 2.5 seconds.

Acceptance: idle/slow clients cannot delay normal process shutdown until the full request timeout. Status: implemented and verified by the idle-client runtime test.

## Stage D — Strict configuration validation (Medium)

- [x] Distinguish absent optional keys (fallback allowed) from present invalid or overlong values (fail configuration).
- [x] Add tests for wrong JSON type and overlong auth tokens, model endpoint URLs and paths.
- [x] Ensure failures identify the key without printing secret values.

Acceptance: malformed security-sensitive configuration never silently becomes a weaker default. Status: implemented and tested with `--check-config`.

## Stage E — Provider error privacy (Medium)

- [x] Stop including arbitrary provider error-body text in normal task error strings for streaming LLM and System One requests.
- [x] Retain HTTP status and a sanitized diagnostic (`provider error body omitted`); do not persist raw response bodies.
- [x] Add a mock-provider runtime test with a secret marker and assert it is absent from task status, events, and stderr logs.

Acceptance: ordinary logs/task errors do not mirror arbitrary upstream response body content. Status: implemented and covered by `core_provider_error_privacy`.

## Stage F — HTTP API status consistency and DB close observability (Low/Medium)

- [x] Map common application result classes to stable HTTP codes while preserving the existing JSON error envelope: validation=400, missing resources=404, conflict=409, full queue=429, unexpected internal errors=500.
- [x] Check `sqlite3_close()` and log unexpected busy resources; use `sqlite3_close_v2()` for deferred cleanup instead of dropping an open handle.
- [x] Add runtime API tests for HTTP 400/404 and preserved JSON error fields. A live-statement fixture in Stage J forces `SQLITE_BUSY`, verifies the diagnostic and deferred-close behavior.

Acceptance: transport status reflects common application result classes and DB lifecycle close failures are diagnosed. Status: implementation and HTTP tests pass; Stage J subsequently added a deterministic `SQLITE_BUSY` close fixture and verified it.

## Stage G — Module boundaries and final verification (Maintainability)

- [x] Propose an ordered incremental module extraction plan for HTTP parser/router, task transactions, process supervision, LLM transport and memory extraction in `docs/CORE_MODULE_EXTRACTION_PLAN.md`.
- [x] Record the rule that no large structural move should merge before the relevant contract tests exist.
- [x] Historical Stage 2 verification: all 34 CTest tests excluding `integration` passed after the HTTP correction. The cumulative Stage 3 run below superseded that count with 35/35 after adding `core_mcp_launcher_smoke`; Stage 4 adds the 36th non-integration test.
- [x] Run Python Control Plane tests with the compiled C core configured: 34 tests, 31 passed, 3 skipped for environment reasons.
- [ ] Confirm full core `integration` completes; not confirmed within the local 240-second command limit.
- [x] Update `CORE_AUDIT.md` and this TODO with implemented fixes, exact verification outcomes and remaining limitations.
- [ ] Perform the actual module extractions only as follow-up changes, one boundary at a time, after lifecycle/ownership tests are complete.

Acceptance: final archive excludes build directories, carries audit/TODO/module plan, and reports reproducible test commands/results and remaining limitations. Historical Stage 4 status: at that checkpoint full integration remained NOT CONFIRMED. The later final cumulative checkpoint passed integration 1/1 (152.01 seconds, exit code 0); structural extraction remains intentionally open.

## Current cumulative verification (Stage 4)

- [x] Fresh CMake Release configure/build passed after the MCP helper integration.
- [x] CTest excluding `integration`: 36/36 passed in the final cumulative run.
- [x] Python Control Plane suite: 34 tests, 31 passed, 3 environment-dependent skips.
- [x] At the final cumulative checkpoint, full `integration` passed 1/1 with exit code 0 in 152.01 seconds after CORE-11–CORE-14.

## Stage H — HTTP known-route validation semantics (Low/Medium) — DONE

- [x] Return HTTP 400 for malformed JSON / unsupported actions on recognized `POST /api/tasks` and `POST /api/plan` endpoints instead of a generic 404.
- [x] Keep genuinely unknown routes at HTTP 404 with a matching error message.
- [x] Harden `http_status_for_rpc_result()` so a missing or non-boolean `ok` field is treated as an internal error (500), not success (200).
- [x] Add runtime regression coverage for malformed submit JSON, invalid plan action, unknown route and preserved JSON error envelope.
- [x] Record the current clean build and baseline non-integration test outcomes.

Acceptance: known endpoint request errors are distinguishable from unknown routes; the status mapping and JSON envelope remain consistent. Status: implementation and runtime regression verified in the cumulative stage archive.

## Stage I — MCP post-fork safety (High; launcher implemented, parity coverage partial)

- [x] Implement the dedicated single-threaded `--mcp-launcher` helper and fixed-size, versioned, bounds-checked control payload over descriptor 3.
- [x] Move environment, resource-limit, UID/GID, Landlock and seccomp setup out of the post-fork child; preserve process group and stdin/stdout channels.
- [x] Verify that parent-only secret variables do not reach the MCP process and configured MCP allow-list values do.
- [x] Verify sandbox UID/GID/group drop (when the test runner is root), resource limits, default seccomp socket denial, no non-standard descriptors after final exec in non-sandbox mode, and supervisor restart after initial child exit.
- [ ] Exercise positive/negative Landlock path rules, including explicit read-only paths and denied writes/execution.
- [ ] Inject failures in every helper/sandbox setup step and verify cleanup, bounded diagnostics, and no workspace/process leaks.
- [ ] Run repeated restart/cancel/shutdown under concurrent service load in a successful full integration run.

Acceptance: the multithreaded service's post-fork branch contains no environment/sandbox setup and uses only process/descriptor operations and `execve`; focused regression passes. Full policy equivalence remains open until remaining matrix and failure-injection tests pass. Historical note: integration was NOT CONFIRMED at the Stage 3/4 checkpoint; the final cumulative integration run later passed after CORE-11–CORE-14.


## Stage J — SQLite failure injection and multi-process idempotency — DONE for targeted branches

- [x] Add `tests/test_core_db_faults.c` and `core_db_fault_injection` CTest target without production fault-injection switches.
- [x] Deny the actual empty-queue `COMMIT` via SQLite authorizer; assert a claim error, empty result, rollback/transaction cleanup, and connection reuse after the authorizer is removed.
- [x] Leave a prepared statement open to force `sqlite3_close()` to return `SQLITE_BUSY`; assert the diagnostic, deferred `sqlite3_close_v2()` state, and successful statement finalization.
- [x] Run four independent producer processes against one database: 80 distinct tasks plus a shared idempotency key; assert 81 total rows and exactly one row for the shared key.
- [x] Run targeted CTest (`core_db_fault_injection`) and cumulative non-integration CTest after the change.
- [ ] Expand to sustained high-load and queue-capacity-boundary stress; current process-race coverage is bounded and deterministic, not a stress certification.

Acceptance: the previously untriggered `COMMIT` and `SQLITE_BUSY` paths are now directly exercised and connection/runtime state is checked. Status: targeted acceptance passed; broader DB load/capacity coverage remains open.


## Stage K — Integration harness correctness (CORE-11 / CORE-12)

- [x] **CORE-11 — Correct the captured CLI mode assertion.** A subprocess using `capture_output=True` is intentionally non-TTY and must use the line-oriented fallback; assert the menu and reject alternate-screen entry.
- [x] **CORE-12 — Correct generic HTTP MCP mock arguments.** When `mcp__httpserver__echo` is selected, include `region: "us-west1"` so the client transmits `Mcp-Param-Region`; assert that the fixture error list remains empty after the session follow-up.

## Stage L — Deterministic workspace browser ordering (CORE-13)

- [x] Sort workspace entries directory-first, then by case-insensitive path and bytewise tie-breaker; do not inherit the platform's `readdir()` order.
- [x] Extend the TUI library contract test with deliberately unsorted entries and assert stable selection/navigation.
- [x] Rebuild and run targeted TUI/terminal-layout tests.

## Stage M — Correct the PTY `--no-tui` contract (CORE-14)

- [x] Keep the PTY to validate real spinner redraw sequences while respecting that `--no-tui` does not render the full-screen answer box.
- [x] Assert streamed mock content, completion marker, status/result text and redraw bytes; assert the decorative frame and alternate-screen entry are absent in line mode.

## Final acceptance gate

- [x] Re-run the full non-integration CTest group from the latest tree: 36/36 passed in 68.70 seconds.
- [x] Complete the full `integration` suite from the latest tree with exit code `0` (1/1; 152.01 seconds).
- [x] Update the audit/verification records with exact final results: non-integration 36/36 PASS (68.70 sec); integration 1/1 PASS (152.01 sec, exit code 0).
