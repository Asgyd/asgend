# ASGEND Core Audit

**Date:** 2026-10-23  
**Scope:** C orchestration core (`src/server.c`, `src/db.c`, `src/llm.c`, `src/mcp.c`, `src/models.c`, `src/config.c`, `src/orchestrator.c`, `src/memory/*`) and Python Control Plane (`server/asgend_cp/*`).  
**Baseline:** source tree from `asgend-cli-audit-final.tar.gz`, including the previous CLI/TUI changes. This is a source review and targeted test plan, not a formal proof or a full penetration test.

## Executive summary

The core already has several important protections: parameterized SQLite operations in the task store, bounded request/response sizes, strict JSON parsing for RPC bodies, task-workspace traversal checks using `openat`/`O_NOFOLLOW`, explicit TLS verification for MCP HTTP, no HTTP redirects for model/MCP clients, a worker pool, durable task state, and transactional operations in several DB paths.

The most important issues found in this review concern **atomic task enqueue**, **failure handling for a queue poll transaction**, and **shutdown behavior with idle accepted clients**. Those are correctness and availability problems in the kernel, rather than cosmetic or speculative findings. Configuration parsing and provider error handling also deserve improvement. No assumption is made that a source pattern alone proves an exploitable vulnerability; each finding below states its evidence and limitations.

## Findings

### CORE-01 — `db_enqueue` is not an atomic transaction (High)

**Location:** `src/db.c`, `db_enqueue()`.

The function checks idempotency and queue capacity, creates the task workspace, inserts the task row, and optionally updates the session in separate SQLite statements without a surrounding `BEGIN IMMEDIATE` transaction. If the task row insert succeeds but the subsequent `sessions` statement fails, the function returns failure while the task row remains queued. A caller can therefore retry a submission that actually exists. Concurrent core processes can also race between the idempotency/queue-capacity reads and the insert; the unique constraint prevents duplicate idempotency rows, but the result may be a generic enqueue error and capacity checks are not serialized across processes.

**Impact:** confusing API outcomes, orphaned task workspaces, violated queue-capacity invariants under multi-process use, and partial writes.

**Remediation:** implemented in Stage B: idempotency lookup, queue-capacity check, task insert, and session update are protected by `BEGIN IMMEDIATE`/checked `COMMIT`; errors roll back and remove only the empty workspace created by the failed attempt. A runtime trigger-injection regression test checks rollback and idempotency-key reuse. Multi-process concurrent enqueue stress remains an additional verification target. **Status:** implemented and target-test verified.

### CORE-02 — Empty-queue `COMMIT` result is ignored (Medium)

**Location:** `src/db.c`, `db_claim_next()` branch where `sqlite3_step()` returns `SQLITE_DONE`.

The no-task branch executes `COMMIT` but ignores its result and reports “no work” regardless. A failed commit can leave SQLite in a transaction or hide a database error from the worker loop, causing subsequent claims to fail repeatedly with a less useful error.

**Impact:** degraded queue processing and masked storage failures.

**Remediation:** implemented in Stage B: the empty-queue branch now checks `COMMIT`, attempts rollback on failure, and returns a DB error. At the time this historical finding was written, deterministic failure injection was not available. **Current status (Stage J):** `core_db_fault_injection` uses SQLite authorizer denial to trigger the real failed-`COMMIT` branch and verifies connection reuse; see the final re-audit addendum.

### CORE-03 — Shutdown can wait on idle accepted connections (High)

**Location:** `src/server.c`, `server_run()` shutdown path and `connection_main()`; active descriptors are tracked in `Runtime.connection_fds`.

On shutdown, listener sockets are closed and workers are signaled, but already accepted client sockets are not shut down. After joining workers, `server_run()` polls `active_connections` until each detached connection handler returns. An idle client that has connected but not completed its request can remain in `read_request_line()`/`read_http_request()` until the configured request timeout (validated up to 600 seconds). The loop has no independent shutdown deadline.

**Impact:** a stop/restart/deployment can be delayed by idle or slow clients; under the configured maximum it can wait for minutes.

**Remediation:** implemented in Stage C: once shutdown starts, call `shutdown(SHUT_RDWR)` on tracked accepted sockets while holding `connections_mutex`, while connection threads retain ownership of final descriptor close/release. The runtime test holds an idle connection with a 60-second request timeout and asserts prompt SIGTERM exit. **Status:** implemented and runtime-test verified.

### CORE-04 — Configuration strings can silently fall back when the supplied value is malformed or too long (Medium)

**Location:** `src/config.c`, `get_string_or()` and its call sites.

`get_string_or()` chooses the default when a value exists but is not a JSON string or does not fit the destination. It does not distinguish “not provided” from “provided but invalid”. For secret and endpoint/path settings, silent fallback is hard to diagnose; for example, an oversized auth token on loopback can become an empty token instead of causing configuration failure.

**Impact:** misconfiguration may silently change security or runtime behavior.

**Remediation:** implemented in the follow-up stage. Defaults are retained only for absent keys; explicit values with wrong JSON types or overlong content cause configuration loading to fail with the field label (never the supplied value). Runtime tests cover `server.auth_token`, `ui.auth_token`, `local_llm.base_url`, and `paths.agents_dir`.

### CORE-05 — Error body fragments from model providers can be persisted or logged (Medium / privacy)

**Location:** `src/llm.c`, `systemone_post()` and the streaming LLM HTTP non-2xx error path.

Before remediation, provider response bodies were copied (up to 300 bytes) into error strings. Those strings can be persisted in task status/events and printed to logs. Provider bodies may echo submitted content or contain identifiers; truncation bounds size but does not bound sensitivity.

**Impact:** user prompt fragments or provider metadata could be exposed in logs/task error views.

**Remediation:** implemented in Stage E. Both paths now retain the HTTP status and use the bounded diagnostic `provider error body omitted`; arbitrary response-body text is not copied into task errors. A mock-provider runtime test injects a unique secret marker and asserts it is absent from events, terminal task status and stderr. Raw-body diagnostics remain intentionally unavailable by default.

**Status:** addressed for the reviewed provider HTTP error paths; `core_provider_error_privacy` passes.

### CORE-06 — HTTP API maps some application errors to HTTP 200 (Low/Medium interoperability)

**Location:** `src/server.c`, final JSON response for routed `/api/*` commands.

Before remediation, successful route dispatch always emitted status 200 even if the core result contained `ok:false`. Clients had to inspect the JSON body and HTTP monitoring/retry behavior could mistake failures for success.

**Remediation:** implemented in Stage F. Common result categories map to stable HTTP status codes (400 validation, 401 unauthorized, 404 missing object, 409 conflict, 429 queue full, 500 unexpected internal error), while preserving the existing JSON `ok`/`error` envelope.

**Status:** covered by `core_http_status_mapping` for 400 and 404. The complete mapping table is implemented; additional per-category tests can be expanded.

### CORE-07 — `db_close()` ignores `sqlite3_close()` failure (Low/observability)

**Location:** `src/db.c`, `db_close()`.

Before remediation the return code of `sqlite3_close()` was discarded and `rt->db` was nulled first. If SQLite returned `SQLITE_BUSY`, the live handle was lost and the shutdown failure was invisible.

**Remediation:** implemented in Stage F. The close result is checked while holding `db_mutex`; a non-OK result is logged, and `sqlite3_close_v2()` is used to request deferred destruction before the pointer is cleared.

**Status:** fixed and build-verified. A deterministic fault-injection test for an outstanding prepared statement remains a test gap.

### CORE-08 — Large subsystem files make lifecycle review difficult (Maintainability)

**Location:** `src/server.c`, `src/mcp.c`, `src/models.c`, `src/llm.c`, and `src/memory/{index,embedding,extract,extract_pdf,pipeline}.c`.

Several files combine transport parsing, request dispatch, process lifecycle and state mutation. This increases regression risk for changes to shutdown, limits, cancellation, and error propagation. The code already has many defensive checks, so the recommendation is incremental extraction around stable interfaces rather than a broad rewrite.

**Remediation:** separate the HTTP parser/router, task queue transactions, subprocess supervision, and memory extraction backends with targeted tests at module boundaries.

## Areas reviewed with no immediate defect confirmed

- Request parsing enforces body/header caps, rejects transfer-encoding and ambiguous/pipelined framing, validates UTF-8 for line JSON requests, and uses strict JSON parsing for HTTP bodies.
- Workspace operations require relative safe paths and use descriptor-relative access with `O_NOFOLLOW`.
- MCP HTTP sets `CURLOPT_SSL_VERIFYPEER=1`, `CURLOPT_SSL_VERIFYHOST=2`, disables redirects, and constrains protocols to HTTP(S); insecure HTTP is config-gated elsewhere.
- LLM clients disable redirects, set connect/request timeouts, and use bounded response buffers. libcurl's default TLS verification remains enabled in the inspected calls.
- The Python Control Plane uses parameterized SQL, role checks for workspace operations, pairing-code hashing, token-hash storage, transactional mutations and bounded rate-limit state in the reviewed modules.
- Memory indexing opens files descriptor-relatively, compares inode/size/mtime around open, hashes via OpenSSL, bounds input sizes, and uses SQLite transactions for several multi-row mutations.

These notes are limited to inspected code paths; they do not establish the absence of vulnerabilities elsewhere.

## Verification status

The final archive was configured and built in Release mode from a clean tree; all binary targets linked successfully. All 34 CTest cases other than the long `integration` test passed in a parallel run. The Python Control Plane suite ran 34 tests with 3 environment-dependent skips (live PostgreSQL acceptance/backup-restore not configured; secret-file permission test skipped under root). The full integration test did not finish within the available 240-second command limit; a direct diagnostic run progressed through the HTTP/SSE fault cases and into streaming cancellation, but the entire test is not claimed as passed. See `CORE_AUDIT_VERIFICATION.md` for commands and exact status. This is not a release sign-off until the full integration test completes in the target environment.


## Final verification and remaining work

This paragraph records the earlier Stage F verification snapshot: it had 34 non-integration CTest passes and did not yet have SQLite `SQLITE_BUSY` injection. Stage 4 supersedes those counts and gaps: 36 non-integration tests now pass, and the targeted DB harness verifies the close-busy path. The Python Control Plane suite remains 31 passed / 3 environment-dependent skips. Full integration remains NOT CONFIRMED because no successful exit code 0 was obtained within the execution limit.

The ordered module extraction proposal is in `CORE_MODULE_EXTRACTION_PLAN.md`. The structural code moves are not included in this audit patch series; they should follow one boundary at a time after dedicated contract tests have landed.

The maintainability finding CORE-08 is not marked fixed: an ordered extraction plan exists in `CORE_MODULE_EXTRACTION_PLAN.md`, but moving several large C translation units without first adding parser, process-lifecycle and transport contract tests would be a risky broad rewrite.


## Re-audit addendum — 2026-10-23

The standalone, source-snapshot-specific report is [`../CORE_AUDIT_FINAL_2026-10-23.md`](../CORE_AUDIT_FINAL_2026-10-23.md); the subsystem decomposition and acceptance gates are in [`CORE_REVIEW_DECOMPOSITION.md`](CORE_REVIEW_DECOMPOSITION.md). Stage 4 completes a clean Release build and brings the current non-integration CTest suite to **36/36 PASS**. The Python Control Plane suite reports 31 passed / 3 environment-dependent skips. Full `integration` remains **NOT CONFIRMED** because the invocation did not return exit code 0 before the external command limit.

The re-audit corrected known POST routes incorrectly returning 404 for malformed bodies/actions (CORE-09), and replaced unsafe MCP post-fork setup with a single-threaded launcher helper (CORE-10). The new `core_db_fault_injection` target directly covers denied empty-queue `COMMIT`, `SQLITE_BUSY` close/deferred cleanup, and four-process concurrent enqueue with shared idempotency. The complete Landlock policy equivalence matrix, helper setup-failure injection, larger database load/capacity tests, and full `integration` run remain open; see `CORE_REVIEW_TODO.md`, `CORE_AUDIT_VERIFICATION.md`, and `CORE_MCP_SPAWN_REMEDIATION.md`.
