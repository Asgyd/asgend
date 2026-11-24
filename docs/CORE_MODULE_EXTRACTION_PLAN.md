# Core Module Extraction Plan

This is a low-risk follow-up roadmap from the core audit. It intentionally does not move code as part of the correctness/security patch series: transport, DB transactions and worker shutdown are coupled, so broad extraction without boundary tests would increase regression risk.

## Proposed sequence

### M1 — HTTP parser and route dispatch (`src/server.c`)

**Destination:** `src/http_server.c` plus a private `src/http_server_internal.h` only if a private interface is needed.

**Boundary:** request framing/parser, route-to-core-command mapping, response status mapping, static UI files. Keep socket acceptance, connection accounting, `Runtime`, and shutdown ownership in `server.c` initially.

**Before move:** preserve the current max-header/body limits, reject transfer-encoding and ambiguous framing, authorization behavior, route methods, JSON envelope, status mapping and idle-client shutdown test. Add request parser unit tests independent of a running daemon.

**Acceptance:** `core_http_status_mapping`, security parser tests, idle-client shutdown test and full integration all pass with no route behavior change except explicit status semantics.

### M2 — Task queue transaction layer (`src/db.c`)

**Destination:** `src/task_queue.c` and a narrow internal header for enqueue/claim/finish operations.

**Boundary:** transaction bodies for enqueue, claim, retry/dead-letter and task state transitions. Keep SQLite connection and schema/migrations ownership in `db.c` until transaction functions are stable.

**Before move:** define transaction invariants and error classes, add deterministic fault injection for enqueue and empty-queue claim COMMIT, and add multi-process capacity/idempotency tests.

**Acceptance:** atomic enqueue test, process-level duplicate idempotency test, queue saturation, shutdown recovery, all DB schema migrations and core integration pass.

### M3 — Child-process supervision (`src/mcp.c`, LSP lifecycle in `src/server.c`)

**Destination:** `src/process_supervisor.c` and private helper interfaces.

**Boundary:** spawn environment construction, descriptor closure, stdout/stderr draining, timeout/cancel, wait/reap, restart/backoff and stop signaling. Protocol-specific MCP/LSP code retains protocol state.

**Before move:** explicit lifecycle state machine and ownership table for pid/fds; tests for child exits before handshake, stdout flood, stderr flood, cancel while blocked, shutdown with live child and repeated restart.

**Acceptance:** no zombies or leaked descriptors; shutdown bounded; existing MCP/LSP protocol and sandbox tests pass.

### M4 — LLM HTTP/SSE transport (`src/llm.c`)

**Destination:** `src/llm_transport.c` and a transport-neutral event interface.

**Boundary:** curl setup, HTTP status classification, bounded body buffers, chunk/SSE parsing, timeouts and cancellation. Prompt construction, agent selection, persistence and model policy remain outside the transport module.

**Before move:** protocol corpus for CRLF/chunk boundaries, malformed UTF-8/JSON, multiple choices, missing `[DONE]`, oversized bodies, header delay, first-token/idle/total timeout, cancellation, tool-argument deltas and error-body redaction.

**Acceptance:** streaming order and event sequence unchanged; provider error privacy test passes; transport tests run without the orchestrator DB where feasible.

### M5 — Memory extraction backends (`src/memory/extract*.c`, `src/memory/pipeline.c`)

**Destination:** dedicated PDF, OOXML, plaintext and common-document adapter modules with a shared bounded stream interface.

**Boundary:** parsing backends only. Path ownership, file identity checks, quotas, indexing and transactions remain in the memory pipeline/index layers.

**Before move:** golden fixtures plus corrupt, encrypted, oversized, malformed and decompression-bomb cases. All extraction must preserve byte/time caps and never bypass descriptor-relative path checks.

**Acceptance:** PDF/OOXML smoke tests, memory pipeline, embedding/chunk indexing and retrieval quality tests pass; extraction errors remain explicit and bounded.

## Cross-cutting guardrails

- Extract one module per PR/patch; avoid simultaneous API changes.
- Preserve public RPC fields unless a versioned contract change is explicitly approved.
- Add contract tests before moving code; tests must run from a clean archive extraction.
- No broad mechanical move before lifecycle ownership and thread-safety are documented.
- Consider `-Werror` only after pre-existing warnings are resolved and the supported compiler matrix is known.
