# ASGEND Core — Decomposed Review

**Review date:** 2026-10-23  
**Authoritative finding report:** [`../CORE_AUDIT_FINAL_2026-10-23.md`](../CORE_AUDIT_FINAL_2026-10-23.md)  
**Execution plan:** [`CORE_REVIEW_TODO.md`](CORE_REVIEW_TODO.md)  
**Verification log:** [`CORE_AUDIT_VERIFICATION.md`](CORE_AUDIT_VERIFICATION.md)

This document decomposes the core re-audit by subsystem, execution boundary, failure mode, and acceptance gate. It is a technical review map, not a claim that every listed risk has been exhaustively proven or closed.

## 1. Status vocabulary

- **Fixed and exercised:** code change is present and a focused runtime/regression test passed.
- **Implemented; parity incomplete:** the core remediation is in place, but its full policy/failure matrix or end-to-end gate is still open.
- **Code path present; injection gap:** defensive handling exists, but deterministic fault injection has not triggered the specific branch.
- **Open maintainability work:** a structure/ownership risk is documented; no large refactor was included in this audit.
- **Not confirmed:** the command did not finish with an observed success exit code. Progress output is not treated as a pass.

## 2. Review by subsystem

### A. HTTP request parsing, routing, and response semantics

**Primary code:** `src/server.c`  
**Findings:** CORE-06, CORE-09  
**Current result:** the identified known-route misclassification is fixed and tested; complete status-map coverage remains partial.

The router now distinguishes malformed input on a known POST route from an unknown path. Invalid JSON or unsupported actions on recognized `/api/tasks` and `/api/plan` operations produce `400 Bad Request`; a genuinely unknown path remains `404 Not Found`. RPC status conversion fails closed to `500` when the result is not an object or its `ok` marker is absent/non-boolean. Error responses preserve the JSON envelope.

**Acceptance gate:** retain tests for malformed and valid JSON, unsupported action, missing resource, unknown path, and `ok:false` envelope. Add direct coverage for each supported HTTP status class (validation, not found, conflict, queue full, and internal error) so future mapper changes cannot silently regress.

### B. SQLite task-state and transaction boundaries

**Primary code:** `src/db.c` and `tests/test_core_db_atomicity.py`  
**Findings:** CORE-01, CORE-02, CORE-07  
**Current result:** enqueue rollback, empty-queue `COMMIT` failure handling, and close-error observability are covered by targeted tests.

The enqueue path protects idempotency lookup, capacity check, workspace association, task insert, and optional session update with a checked write transaction. The empty-queue claim path checks `COMMIT` instead of treating commit failure as an empty queue. `db_close()` logs an unexpected busy close and falls back to deferred close.

**Verification added in Stage 4:** SQLite authorizer denial forces the real empty-queue `COMMIT` failure; the test checks error reporting and connection reuse. An outstanding statement forces `sqlite3_close()` to return `SQLITE_BUSY`; the diagnostic and `sqlite3_close_v2()` deferred-close path are checked. Four separate producer processes inserted 80 distinct tasks and raced on one shared key; final state was 81 tasks with a single shared-key row. Larger sustained-load/capacity-boundary stress remains open.

**Acceptance gate:** add deterministic injection/fixtures for each exact failure branch, assert returned error and rollback/cleanup state, then run concurrent producers across separate processes against a shared database.

### C. Connection lifecycle and graceful shutdown

**Primary code:** `src/server.c`, `tests/test_core_shutdown.py`  
**Finding:** CORE-03  
**Current result:** fixed and runtime exercised.

Accepted TCP/HTTP/Unix descriptors are tracked; shutdown wakes idle clients while preserving each connection thread's close ownership. This avoids waiting for request timeouts and avoids double-close ownership races.

**Acceptance gate:** preserve the idle-client SIGTERM regression and keep the suite passing under concurrent requests and repeated start/stop cycles.

### D. Configuration trust and secret diagnostics

**Primary code:** `src/config.c`, configuration tests  
**Finding:** CORE-04  
**Current result:** strict type/length validation and non-secret diagnostics are present and tested.

Present-but-invalid security-sensitive fields should fail configuration rather than silently fall back. Errors identify the field/key without printing submitted secret content.

**Acceptance gate:** maintain type, overlength, invalid URL/path, and token tests, and run secret-permission cases as a non-root user because a root test runner bypasses normal permission-denial behavior.

### E. Upstream provider failure and data privacy

**Primary code:** `src/llm.c`, provider test fixture  
**Finding:** CORE-05  
**Current result:** fixed and runtime exercised.

Non-2xx provider response bodies are not echoed into ordinary task error/event/log fields. Diagnostics retain status and a sanitized explanation without persisting an arbitrary upstream body.

**Acceptance gate:** preserve the marker-based test proving a secret sentinel from a mock provider is absent from task state, events, and stderr.

### F. MCP child-process launch and sandbox boundary

**Primary code:** `src/mcp.c`, `src/main.c`, `include/runtime.h`; `tests/test_core_mcp_launcher.py`  
**Finding:** CORE-10  
**Current result:** the unsafe post-fork setup is remediated; broader sandbox parity is incomplete.

The multi-threaded service's post-fork child now performs only process/descriptor operations and `execve()` into the same executable's dedicated `--mcp-launcher` mode. A bounded, versioned payload is passed through control descriptor 3. The newly exec'd single-threaded helper validates that payload, constructs the allow-listed MCP environment, reuses the existing resource-limit/identity/Landlock/seccomp setup, closes inherited descriptors, then `execvp()`s the configured MCP process. Unrelated service environment and credentials are not placed in the helper environment.

**Focused verification:** no parent-secret sentinel leakage; explicit MCP environment value retained; root-run sandbox child drops to UID/GID 65534 and clears supplementary groups; configured CPU/address-space/file-size/open-file/core limits are observed; seccomp rejects socket creation by default; non-sandbox target sees only standard descriptors; supervisor restarts after an initial MCP process exit.

**Residual risk:** this smoke matrix does not yet cover every Landlock allow/deny branch, every setup failure point, restart under concurrent load, or all shutdown/cancellation combinations. The full `integration` suite also remains not confirmed.

**Acceptance gate:** add positive and negative path-policy tests for read/write/execute, required-Landlock fail-closed tests, helper setup fault injection, process/workspace leak assertions, then complete the full integration suite with exit code `0`.

### G. Cross-module structure and test/release gates

**Primary code:** `src/server.c`, `src/mcp.c`, `src/models.c`, `src/llm.c`, and `src/memory/*`  
**Finding:** CORE-08  
**Current result:** decomposition plan exists; extraction remains open by design.

Several translation units mix protocol parsing, state mutation, and process/resource lifecycle. Broad refactoring during a security/lifecycle fix would increase regression risk. The module plan therefore orders extraction after ownership contracts and boundary tests exist.

**Acceptance gate:** perform one extraction at a time, without behavior change; require the subsystem contract suite and the full non-integration regression suite before merging each move.

### H. Integration harness and deterministic mock contracts

**Primary file:** `tests/integration.py`  
**Findings:** CORE-11, CORE-12, CORE-13 and CORE-14  
**Current status:** CORE-11/12/13/14 have implementation/test changes. The latest clean build succeeds; targeted TUI checks passed. Full integration acceptance depends on the newest run returning exit code 0.

The integration harness must distinguish terminal modes. A subprocess captured with pipes is not a TTY and exercises the line-oriented fallback; full-screen framing, cursor redraw, and PTY spinner behavior belong to tests that allocate a pseudo-terminal. Mixing these contracts caused the reported assertion failure without evidence of a production TUI regression.

The mock MCP peer is also a protocol contract, not a permissive echo server: when the selected HTTP tool declares a header-bound `region` parameter, the mock-model response must provide it so the client can construct `Mcp-Param-Region`. A generic tool-call fallback that omits this argument creates a false failure and can conceal whether HTTP header mapping itself works. In the workspace browser, file ordering must not rely on unspecified `readdir()` output; the UI now lists directories first and sorts names deterministically. Terminal-mode tests must be explicit: `capture_output=True` is non-TTY, while a PTY with `--no-tui` is still line-oriented and should assert spinner/control sequences and textual response, not a decorative full-screen answer frame.

**Acceptance gates:** captured-output line-mode assertions pass; PTY `--no-tui` checks verify redraw behavior and streamed answer without requiring a frame; actual full-screen TUI tests continue to verify full-screen rendering; generic HTTP echo supplies `region`; mock peer records no errors after follow-up; workspace list order is deterministic; full `integration` exits with status `0`.

## 3. Severity and disposition matrix

| Finding | Domain | Priority | Disposition | Remaining acceptance work |
|---|---|---:|---|---|
| CORE-01 | SQLite enqueue atomicity | High | Transaction rollback and four-process enqueue/idempotency test pass | Sustained load and queue-capacity boundary stress |
| CORE-02 | SQLite empty-queue commit | Medium | Authorizer-denied `COMMIT` regression passes; connection reuse checked | Additional transaction error cases under stress |
| CORE-03 | Connection shutdown | High | Fixed; runtime tested | Preserve under stress/repeated lifecycle cycles |
| CORE-04 | Config validation | Medium | Fixed; tests pass | Non-root secret-permission validation |
| CORE-05 | Provider error privacy | Medium | Fixed; runtime tested | Preserve marker-based regression |
| CORE-06 | HTTP status mapping | Low/Medium | Fail-closed marker handling; mapping coverage partial | Table-driven status coverage |
| CORE-07 | SQLite close handling | Low/Medium | Outstanding-statement fixture forces `SQLITE_BUSY`; diagnostics and deferred close pass | Broader DB lifecycle failure coverage |
| CORE-08 | Module cohesion | Maintainability | Plan only | Incremental extraction after contracts |
| CORE-09 | Known-route validation status | Low/Medium | Fixed; runtime regression passes | Broader status table coverage |
| CORE-10 | MCP post-fork safety | High | Launcher fix implemented; focused smoke passes | Full Landlock/failure-injection/concurrency matrix |

## 4. Dependency-ordered implementation sequence

1. **Protect observable contracts first:** retain the HTTP status and shutdown tests; ensure future changes preserve the existing JSON/error and descriptor-ownership semantics.
2. **Finish MCP acceptance coverage:** extend the now-safe launch architecture with Landlock path allow/deny tests, setup fault injection, and concurrent supervisor lifecycle checks. Do not reduce sandbox policy to simplify spawn.
3. **Extend SQLite stress coverage:** Stage 4 now covers commit denial, busy close, connection recovery and multi-process idempotency. Follow with sustained concurrent load and queue-capacity boundary testing.
4. **Close environment-dependent release gates:** run PostgreSQL acceptance/backup-restore with `ASGEND_TEST_POSTGRES_URL`, run permission tests as non-root, and execute full `integration` to exit code `0`.
5. **Reduce architecture debt deliberately:** extract one module boundary per change with contract tests and the non-integration suite green before and after each extraction.

## 5. Current reproducible verification snapshot

- Release CMake configure/build: **PASS**.
- CTest excluding full `integration`: **36/36 PASS**.
- Python Control Plane suite: **34 total, 31 PASS, 3 SKIP** (live PostgreSQL URL absent; root permission semantics skip).
- Full C integration test: **NOT CONFIRMED**; the previous invocation did not return exit code `0` within the command execution limit.
- External independent subagent assessment: not performed; this environment exposes no subagent launcher. The audit does not claim an independent external-agent score.
