# ASGEND project review decomposition

## 1. Execution core and API

**Files:** `src/server.c`, `src/orchestrator.c`, `src/agent.c`, `src/net.c`, `src/config.c`, `src/config_editor.c`, `include/`.
**Review:** input framing, deadline handling, authentication, status/error mapping, cancellation, queue backpressure, shutdown, configuration validation, and API compatibility.
**Acceptance:** targeted runtime tests, complete CTest groups, and an integration run that exits `0`.

## 2. Persistence and recovery

**Files:** `src/db.c`, SQLite fault-injection tests, migrations and task/plan store.
**Review:** transaction boundaries, commit/bind failures, busy handling, schema migration, retry/dead-letter/redrive, idempotency, graceful shutdown, and multi-process contention.
**Acceptance:** deterministic fault injection plus atomicity, migration, recovery, and contention tests.

## 3. LLM and streaming

**Files:** `src/llm.c`, model registry, SSE integration fixtures.
**Review:** first-token, idle and total deadlines, cancellation, malformed events, missing `[DONE]`, non-streaming compatibility, provider error privacy, and retries.
**Acceptance:** each fault fixture reaches a terminal task state within its deadline, no leftover server/workers, a standalone `core_llm_sse_watchdog` CTest, and integration exit code `0`. The stream uses `curl_multi_poll()` to enforce deadlines while the socket is silent.

## 4. MCP and LSP

**Files:** `src/mcp.c`, `src/lsp.c`, launch helper, MCP/LSP fixtures.
**Review:** post-fork safety, sandbox equivalence, descriptor closure, restart lifecycle locking, readiness semantics, protocol versioning, tool schema/header mapping, cancellation and bounded retry.
**Acceptance:** launcher smoke, restart-boundary tool call, sandbox matrix, LSP diagnostics, and integration suite.

## 5. Memory/retrieval

**Files:** `src/memory/`, `clients/asgend-memory.c`, memory tests.
**Review:** extractor bounds and malformed documents, indexing consistency, chunking, embeddings, graph edges, ANN correctness, reranking, query plans, garbage collection, PDF/OOXML and retrieval quality.
**Acceptance:** component test groups, fixture-quality checks, corruption/limit cases and source-document parity checks.

## 6. Client surfaces

**Files:** `clients/asgend_cli.c`, `src/asgend_tui.c`, `clients/asgend_android/`, Flutter/Qt clients.
**Review:** parity with core commands, task lifecycle, live events, terminal/IME/accessibility behavior, responsive layout, offline/reconnect behavior, credentials, and artifact export.
**Acceptance:** C CLI/TUI contract tests; Flutter/Qt/Android builds and device acceptance where toolchains are installed. Missing SDK means NOT RUN, not PASS.

## 7. Control Plane and distributed nodes

**Files:** `server/asgend_cp/`, `node_agent/`, OpenAPI and protocol contracts.
**Review:** auth/RBAC, pairing and revocation, role gating, task/event cursors, node reconnect, artifact tickets, checksum/size enforcement, DB migrations, and idempotent operations.
**Acceptance:** Python tests, Node tests, OpenAPI consistency, Postgres-backed acceptance when Postgres is available.

## 8. Web UI, release and operations

**Files:** `ui/`, `packaging/`, `release/`, `scripts/`, `.github/workflows/`, Docker files and docs.
**Review:** API compatibility, asset/version gates, checksums, reproducible archives, package install/upgrade, secret redaction, supported platform matrix, and CI triggers for cross-layer contract changes.
**Acceptance:** JS syntax/build tests, release-gate tests, clean archive extraction, checksums, package smoke in matching container/OS.

## Evidence rules

- Every run is labelled PASS only when the command exits `0` and its output agrees with the expected test count.
- A command timeout is TIMEOUT / NOT CONFIRMED.
- Missing external dependencies or platform toolchains are NOT RUN with the dependency named.
- Historical statuses from prior project documents are not used as evidence for the current source snapshot.


## CORE-16 — SSE deadline and cancellation correctness

**Primary file:** `src/llm.c`; test: `tests/test_core_llm_sse_watchdog.py`; CTest: `core_llm_sse_watchdog`.

**Failure mode:** cURL easy-perform can remain waiting on a silent chunked response while progress callbacks do not run often enough to enforce the monotonic idle deadline.

**Implementation:** stream requests are driven using `curl_multi_poll()` with a bounded 100 ms poll interval. The watchdog checks first-token, inter-token idle, total deadline and cancellation conditions outside the socket wait.

**Acceptance:** SSE fault matrix exits `0`, no orphan orchestrator remains, and full integration exits `0`. Current observed results: standalone CTest 1/1 PASS in 39.31 s; integration 1/1 PASS in 126.75 s. One complete combined CTest run is still pending.


## CI-ANDROID-01 — backend contract changes must trigger native Android checks

**Primary file:** `.github/workflows/native-android.yml`. Its push/PR filters currently watch the Android module, Android docs and workflow only. Changes in core RPC/API source or OpenAPI can alter the wire contract without scheduling Android validation.

**Required fix:** include API/schema/header/core contract paths in both filters and add a guard test that fails if those paths disappear.

## 9. Runtime/OpenAPI/client contract parity — Stage 13

**Files:** `server/asgend_cp/app.py`, `server/asgend_cp/http.py`, `contracts/v1/openapi.yaml`, `tests/contracts/test_contracts.py`, `tests/contracts/requirements.txt`, and the Control Plane/Android/Flutter/Qt workflows.

**Review:** exact route registration inventory vs OpenAPI method/path pairs; runtime auth class vs per-operation `security`; endpoint path references in the native Android, Flutter, Qt and Node Agent clients; robustness of the source scanners; and whether client-only changes execute shared contract tests in CI.

**Acceptance:** runtime route set matches the OpenAPI operation set in both directions, no duplicate method/path route, auth/security mappings match for every route, every discovered client path is covered by OpenAPI, the negative path matcher rejects invalid static segments, and all client workflows run the shared contract suite.

**Stage 13 result:** PASS for source checks. The current server exposes 37 documented operations and all 37 match exactly. Forty-nine client path references were discovered and matched. Hosted Actions remains unverified until a push/pull request runs on GitHub.

## Stage 14 — Core RPC protocol

**Surface:** `src/server.c::server_handle_request`, `docs/PROTOCOL.md`, native Android/Flutter/Qt/Node Agent local-core clients, `CMakeLists.txt`.

**Review questions:** Does the documentation name every dispatch operation exactly once? Does source scanning detect unsupported operation literals in each client? Are dynamically selected model actions covered? Is the checker registered in CTest? Are local TCP/Unix RPC, browser HTTP and Control Plane HTTPS kept distinct?

**Acceptance:** exact set of 33 runtime operations equals the sorted documentation inventory; client scanners report no unsupported operations; source scanner has per-client minimum floors; negative malformed-inventory tests fail as expected; CTest target passes.

**Out of scope:** this inventory does not prove each operation's request fields, authorization, or runtime response semantics. Those require focused contract/runtime tests.


**Stage 14 verification result:** 41/41 CTest PASS (227.53 s, exit 0); 33/33 Core RPC dispatch operations match the protocol inventory; client source scans contain no unsupported operations; client workflow guard passes 76 required path assertions. Hosted Actions and platform acceptance remain separate.
