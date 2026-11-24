# ASGEND CLI/TUI Audit

- **Audit date:** 2026-10-23
- **Scope:** `clients/asgend_cli.c`, `src/asgend_tui.c`, `include/asgend_tui.h`, CLI/TUI contract and PTY tests, and the way the client consumes core RPC/event APIs.
- **Baseline:** `asgend-cli-stream-final-integration-fix.tar.gz` with the previously requested streaming changes and plan-title fix present.
- **Method:** static review of control flow, resource ownership, terminal lifecycle, RPC timeouts, streaming state/correlation, event polling, workspace preview refresh, plus the repository's existing test definitions. No separate subagent runner is available in this execution environment; findings were checked in three review passes (correctness/concurrency, UX/runtime behavior, and test/maintenance coverage).

## Executive summary

The CLI already has a substantial full-screen TUI, a line-oriented fallback, JSON-line RPC, a separate task-event watcher thread, streaming output for assistant/Thinking/tool-call deltas, task/session selection, workspace file preview, and contract/PTY tests. The largest risks are not missing features; they are reliability boundaries around RPC performed on the UI thread, final-event draining, thread shutdown, and bounded UI history.

The implementation is concentrated in a single ~3,100-line C translation unit (`clients/asgend_cli.c`) and a ~2,100-line renderer/input library (`src/asgend_tui.c`). This increases regression risk because networking, event interpretation, task workflows, terminal state, file preview, editing and input-mode control all share global/static state.

## Findings

### CLI-001 — Terminal task completion may stop event polling before the whole event history has been drained

- **Priority:** P1 — correctness / lost output
- **Location:** `clients/asgend_cli.c`, `cli_tui_task_watch_main()` (around lines 364–420).
- **Evidence:** Each iteration asks `events` for at most 100 records (`limit = 100`), then requests task `status`. If status is terminal, the watcher breaks immediately. The loop does not keep fetching pages until it sees an empty/short page. A task that emitted more than 100 events, or whose last events are committed between the events request and the status request, can finish while the client still has unread events.
- **Impact:** The TUI may omit the final assistant delta, tool completion, file-change event, question, or terminal lifecycle record. Draining the in-memory queue after the watcher exits cannot recover events that were never fetched from the core.
- **Recommended change:** Track the last event ID as `int64_t`; after observing a terminal task state, enter a drain phase that repeatedly fetches `events(after_id, limit)` until no new events are returned. Mark the watcher finished only after that drain completes. Add a fake-core test for >100 events and the status/event race.

### CLI-002 — Plan watcher shutdown can block much longer than the server long-poll interval

- **Priority:** P1 — responsiveness / shutdown
- **Location:** `clients/asgend_cli.c`, `cli_tui_plan_watch_main()` (around lines 251–294), `cli_tui_stop_plan_watch()` (around lines 296–304), `connect_core()` / `rpc()`.
- **Evidence:** The watcher requests `plan_wait` with `timeout_ms = 750`, but the thread does not set its thread-local `cli_rpc_timeout_seconds`; the default socket receive timeout is 60 seconds. If the server accepts a connection but fails to answer, `cli_tui_stop_plan_watch()` waits in `pthread_join()` for the RPC timeout.
- **Impact:** Exiting the TUI, changing app state or shutting down can appear frozen for up to 60 seconds.
- **Recommended change:** Use a small per-thread RPC timeout consistent with the long poll (for example 2–3 seconds), and make stop behavior bounded. Add a PTY test with a mock core that accepts the plan-wait connection but never responds.

### CLI-003 — Synchronous RPC calls run from the TUI/input thread

- **Priority:** P1 — responsiveness
- **Location:** `clients/asgend_cli.c`, `cli_tui_refresh_data()` (around lines 1555–1615), `cli_tui_refresh_files()`, `cli_tui_refresh_mcp()`, `cli_tui_show_workspace_file()`, `cli_tui_refresh_open_preview()`, task/plan input handlers.
- **Evidence:** Data refresh makes `list_tasks`, `plan_list`, `workspace_list`, and `mcp_status` RPCs sequentially. File opening and preview refresh also call `rpc()` directly from key handling/event-batch processing. General RPC timeout is 60 seconds. Only task events and plan-change notifications have background threads.
- **Impact:** On a slow/stalled core, typing and navigation freeze until the blocking RPC returns. Event updates can make this worse because file/tool events trigger multiple synchronous calls from the main loop.
- **Recommended change:** Move nontrivial RPC work to a worker/command queue and publish results to the main loop. As a tactical improvement, apply short endpoint-specific timeouts and coalesce refresh requests; never perform repeated refreshes once per event in a long batch.

### CLI-004 — Large workspace previews do not refresh after a file changes

- **Priority:** P2 — stale UI state
- **Location:** `clients/asgend_cli.c`, `cli_tui_show_workspace_file()` and `cli_tui_refresh_open_preview()` (around lines 1151–1197).
- **Evidence:** Initial opening falls back to `workspace_read_chunk` when the file is larger than `CLI_WORKSPACE_FILE_MAX` (1 MiB). On subsequent refresh, `cli_tui_refresh_open_preview()` only updates from `workspace_read` when the whole file fits under 1 MiB; for larger files it performs no chunk read or visible error.
- **Impact:** A large file can remain stale in the preview after a tool edit or workspace event, even though refresh was requested.
- **Recommended change:** Preserve the current chunk offset and reload that chunk from `workspace_read_chunk` during refresh. If diffing large files is unsupported, show the refreshed chunk and a clear “diff unavailable for chunked preview” state.

### CLI-005 — Completed stream states can reject a later stream with a reused identity

- **Priority:** P2 — stream correctness
- **Location:** `clients/asgend_cli.c`, `cli_thinking_state()` and `cli_tool_state()` (around lines 646–727).
- **Evidence:** Both lookup helpers return an exact identity match before considering that a completed slot can be reused. The caller then checks `state->completed` and ignores new deltas. If an old/compatibility event producer reuses the same `stream_id` / `(call_key, stream_id, model_name)`, the subsequent stream is silently discarded.
- **Impact:** A later Thinking or tool-call stream with a reused/fallback identity may never render.
- **Recommended change:** Make a new lifecycle explicit: preserve the state only while active, or reset a completed exact-match slot when a new start/delta lifecycle begins. Add repeated-identity tests.

### CLI-006 — TUI history silently evicts the oldest tile at the fixed capacity

- **Priority:** P2 — history integrity
- **Location:** `src/asgend_tui.c`, `asgend_tui_upsert_tile()` (around lines 1660–1685); `TUI_MAX_TILES = 512`.
- **Evidence:** On reaching 512 tiles, the implementation unconditionally frees/removes `tiles[0]` before inserting a new tile. It does not prefer completed non-chat items or avoid evicting a tile whose stream is still active.
- **Impact:** Old user/assistant messages or active stream cards may disappear without notice. A later delta can recreate an active card without its earlier content.
- **Recommended change:** Use an eviction policy that prioritizes completed transient status tiles, never silently evicts active streams, and surfaces a history-truncated marker when persistent chat entries must be evicted. Add tests that fill the tile array with mixed active/completed/chat items.

### CLI-007 — Streaming tile content is silently truncated at a fixed body limit

- **Priority:** P2 — long-output correctness
- **Location:** `src/asgend_tui.c`, `TUI_BODY_MAX` and `asgend_tui_upsert_tile()` (around lines 24 and 1660–1715).
- **Evidence:** Appended content is capped at `ASGEND_TUI_TEXT_MAX * 8` bytes (currently 131,072). Once the body reaches the cap, later bytes are dropped, but the tile may remain marked streaming and no visible truncation notice is added.
- **Impact:** Long generated answers, Thinking or tool output can look complete while their tail is missing in the UI. This is especially misleading for code or structured data.
- **Recommended change:** Maintain a bounded rolling window or spool full content outside the render buffer; show a visible truncation indicator and preserve the final tail. Add a test streaming beyond the cap.

### CLI-008 — Input and TUI control paths are coupled through global/static state

- **Priority:** P2 — maintainability / regression risk
- **Location:** `clients/asgend_cli.c` global state near the top of file, `print_event()`, `cli_tui_render_event()`, `cli_tui_loop()`, `cli_tui_follow_task_async()`; `src/asgend_tui.c` renderer/input globals.
- **Evidence:** Stream states, preview state, task scope, requested session, watcher status and input action are static globals; renderer frame buffers and resize/mouse parser state are also process-global in the library. Main event interpretation, rendering and RPC side effects are interleaved in large functions.
- **Impact:** The code is difficult to unit-test in isolation, makes multiple TUI instances unsafe, and increases the chance of lifecycle/focus regressions when features are added.
- **Recommended change:** Incrementally separate protocol/RPC, task watch, event-to-view-model translation, workspace-preview controller and terminal renderer. Keep terminal writes on the main thread and pass owned event/result messages across thread boundaries.

### CLI-009 — CLI credentials supplied as a command-line argument can be exposed by process listings

- **Priority:** P2 — secret handling
- **Location:** `clients/asgend_cli.c`, `main()` argument parsing (around lines 3080–3110), `usage()`.
- **Evidence:** `--token TOKEN` is supported alongside `ASGEND_TOKEN`; command-line arguments can be visible to local process-inspection tools on common operating systems.
- **Impact:** A token may be exposed to other local users/processes where process command lines are readable, shell history, or diagnostics.
- **Recommended change:** Prefer environment or an interactive prompt; retain `--token` only for compatibility with an explicit warning, and avoid echoing tokens in logs/errors. Consider a future file descriptor/stdin token option.

## Remediation status after Stages B–F

- **CLI-001:** Implemented post-terminal drain: the watcher uses a 64-bit event cursor, fetches in pages of 100, requires at least one successful event-page request after seeing terminal status, and only exposes `finished` after a short page is drained. This covers both the >100-event case and the event/status race at the end of a task.
- **CLI-002:** Implemented a 2-second thread-local RPC timeout for the plan watcher, with quiet background diagnostics and restoration of the defaults when the thread exits.
- **Verification:** new `cli_watch_reliability` contract and 8 targeted CLI/TUI tests pass. Full-project compile/integration is still unconfirmed in this run due to command timeout during unrelated targets.

- **CLI-003:** Partially mitigated in Stage C: TCP `connect()` now uses nonblocking connect + `poll()` with the configured timeout, the fullscreen TUI uses a 3-second foreground RPC socket timeout, and refreshes from a single event batch are already coalesced. This reduces but does not eliminate input-thread blocking; moving preview/list RPC to a worker-result queue remains open.

- **CLI-004:** Implemented in Stage D: a large file preview now re-fetches its current chunk when whole-file `workspace_read` cannot be used; the current chunk offset is retained. Visual diffing is still limited to whole files at or below 1 MiB.
- **CLI-005:** Implemented in Stage D: a completed Thinking/tool state that is matched by a new delta lifecycle is reset instead of rejecting later deltas with a reused compatibility identity.

- **CLI-006:** Implemented in Stage E: eviction prefers completed non-chat tiles, then completed chat; active streams are not evicted. If every tile is active, insertion is refused and a visible history-loss marker/count is recorded.
- **CLI-007:** Implemented in Stage E: messages exceeding the UI body budget receive a visible truncation prefix and preserve the recent tail. The server event log remains the full source of truth.
- **CLI-008:** Still open. The main CLI and renderer remain large, coupled translation/rendering units with shared static state. The audit identifies safe extraction seams, but broad modularization was not attempted as a risky one-shot rewrite.
- **CLI-009:** Partially addressed in Stage F: when no token is configured and stdin is a TTY, CLI prompts with terminal echo disabled and restores terminal settings before returning. Usage prefers `ASGEND_TOKEN` and warns that `--token` may be visible in process listings. A runtime mock-core regression confirms the configured token is sent in the RPC request but not printed to stdout/stderr. The `--token` compatibility argument remains supported and is still visible to process-inspection tools.

## Remaining high-priority work

- **Asynchronous foreground RPC (CLI-003):** the 3-second foreground TUI timeout reduces stalls but does not make networking nonblocking from the user's perspective. The next architectural step is a worker/command queue with owned request/result messages, cancellation, stale-result rejection, and all terminal writes kept on the main thread.
- **Offline/stale connection state:** bounded timeout errors can currently leave the user with a silent or generic failed refresh; expose a visible disconnected/stale indicator and retry policy.
- **End-to-end large preview test:** current regression checks verify the chunk-read refresh path at the source-contract level. Add an integration test with a real core and a modified workspace file larger than 1 MiB.
- **Full integration verification:** supplied logs still show failure in a PTY TUI scenario, and the full isolated integration run did not complete within the execution budget. Targeted tests must not be treated as proof of the complete integration suite.

## Strengths observed

- RPC uses bounded response size (16 MiB), socket read/write timeouts, JSON parsing, and suppresses `SIGPIPE` in `main()`.
- Event-watch queue is bounded and applies backpressure rather than intentionally dropping stream deltas.
- The main TUI loop owns rendering; watcher threads publish data through a mutex/condition variable or atomics rather than directly writing terminal output.
- Workspace file reads and editor staging have explicit size limits; editing uses a temporary directory and launches an editor without passing a command through a shell.
- The test suite contains targeted stream/session/layout contracts in addition to broad integration coverage.

## Validation after the audit stages

The CMake project now registers 30 tests, including the added watcher, RPC-timeout, preview/stream-lifecycle, and secret-handling contracts. In the final isolated worktree, the `asgend-cli` and `test-asgend-tui` targets built successfully and all 11 targeted CLI/TUI tests passed. The runtime secret test uses a mock core to assert that the supplied token arrives in the RPC request and is absent from captured stdout/stderr.

A full clean build of every target was **not verified**: the isolated build exceeded the command execution budget while compiling unrelated memory-library objects. The full integration suite is also **not verified**. The latest user-provided test log fails in `run_cli_two_client_plan_sync` while waiting for `TUI_TWO_CLIENT_SYNC_TODO`; a separate complete integration run could not be finished within the tool limit. These results are not described as passing.

## Suggested priorities

1. **P1:** move foreground RPC from the input loop to a worker/result queue, with cancellation, stale-result rejection and visible offline state.
2. **P1:** rerun and fix full PTY integration, starting with the two-client To-do/Goal synchronization scenario in the supplied log.
3. **P2:** replace source-contract-only testing of modified >1 MiB workspace previews with a real core/workspace integration test.
4. **P2/P3:** incrementally extract RPC, event/view-model translation, workspace preview and rendering from the CLI and TUI monoliths; do not attempt a broad rewrite without interface contracts.
5. **P3:** consider a file-descriptor token input mode for automation so secrets need not be placed in process arguments.
