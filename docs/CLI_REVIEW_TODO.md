# CLI Review — Implementation TODO

This TODO is based on `docs/CLI_AUDIT.md`. Each stage is a separately reviewable deliverable; the archive produced after each stage should contain the current source, audit, TODO and tests, without build directories.

## Stage A — Audit and plan
- [x] Map CLI/TUI architecture and active test coverage.
- [x] Record findings with priority, source locations, impact and recommended verification in `docs/CLI_AUDIT.md`.
- [x] Decompose changes into ordered, independently testable stages here.
- [x] Record baseline limitations honestly: local isolated build exceeded the 240-second command budget during memory-library compilation; integration has a known failing/timeout path in the supplied logs.

## Stage B — Event history integrity and shutdown reliability (P1)
- [x] Widen event cursor/event ID to 64-bit.
- [x] Drain event pages after terminal status until the server returns an empty/short page; handle the race between event polling and terminal status.
- [x] Ensure a stop request cannot leave the worker asleep forever on a full queue or blocked poll.
- [x] Give the `plan_wait` watcher a bounded per-thread RPC timeout aligned with its long-poll interval.
- [x] Add tests for >100 queued events, completion between event and status requests, and plan watcher shutdown when the server accepts but does not answer.
- [x] Verify with clean compile and relevant CTest contracts; archive Stage B.


Validation result: `asgend-cli` compiled and linked successfully; the 8 targeted TUI/CLI tests passed. A complete project build exceeded the 240-second command budget while compiling unrelated networking/memory objects; full integration has not been revalidated in this stage.

## Stage C — Remove long synchronous work from interactive paths (P1)
- [x] Measure and inventory RPC calls performed in the main TUI loop (documented in CLI-003).
- [x] Coalesce file/workspace refreshes across one event batch; the current event loop uses a single `refresh_files` flag per batch.
- [x] Bound TCP connect waits and foreground TUI RPC socket waits (3 seconds), and bound `plan_wait` watcher RPC to 2 seconds. Offline/stale status messaging remains open.
- [ ] Move slow preview/list operations to a worker-result queue; the current stage uses bounded waits but synchronous RPC can still briefly block the main loop.
- [ ] Add a fake-core responsiveness test for stalled connect/read, plus a runtime assertion that workers never write to the terminal.
- [x] Archive Stage C.


Validation result: the `asgend-cli` Release target built cleanly and 7/7 targeted CLI/TUI contracts passed. The complete project build remains unverified in this slow isolated environment.

## Stage D — Preview and stream lifecycle correctness (P2)
- [x] Refresh files larger than 1 MiB using the existing chunk-read API, retaining viewport offset.
- [x] Make completed stream state reusable when a new lifecycle with the same compatibility identity begins.
- [x] Add regressions for repeated stream IDs/call keys and modified large-file previews.
- [x] Archive Stage D.


Validation result: the `asgend-cli` Release target built cleanly and 8/8 targeted CLI/TUI tests passed. Large-preview behavior is covered by a source-contract regression; the test suite does not launch a real core with a >1 MiB modified workspace file yet.

## Stage E — Bounded UI history and long response visibility (P2)
- [x] Replace unconditional oldest-tile eviction with an explicit priority policy that protects active streams.
- [x] Surface a visible “older history omitted” notice when chat content must be evicted.
- [x] Replace silent 131,072-byte append truncation with a visible truncation policy; preserve useful tail or spool full output.
- [x] Add tests for 512+ tiles, active-stream retention and responses above the body limit.
- [x] Archive Stage E.


Validation result: the TUI library and CLI rebuilt successfully; 10/10 targeted CLI/TUI tests passed. Library tests exercise truncation markers, recent-tail retention, completed-tile eviction priority, and protection of active streams.

## Stage F — CLI security and maintainability (P2/P3)
- [x] Prefer `ASGEND_TOKEN` or a non-echoed interactive token prompt; warn that `--token` may be visible in process listings.
- [x] Add a runtime fake-core test verifying the token is sent to the RPC but never printed to stdout/stderr in CLI diagnostics.
- [x] Document future module boundaries and the work that still requires an asynchronous RPC result queue.
- [x] Archive Stage F.

Validation result: the CLI and TUI test targets built successfully; all 11 targeted CLI/TUI tests passed, including the runtime token diagnostic test.

## Final verification and review
- [x] Build the `asgend-cli` and TUI test targets in the isolated tree.
- [x] Run all available targeted CLI/TUI CTest tests (11/11 pass).
- [x] Review concurrency/lifecycle, visible-content correctness, and credential handling; discovered issues were either fixed and tested or explicitly left open below.
- [ ] Build every project target from a fully clean directory: **unverified**, the isolated command exceeded its 240-second budget in unrelated memory-library compilation.
- [ ] Re-run the full integration suite after applying the changes in the project's native environment. The latest provided log fails in `run_cli_two_client_plan_sync` while waiting for `TUI_TWO_CLIENT_SYNC_TODO`; the isolated targeted tests do not prove the full suite passes.
- [x] Build the final source archive without build directories.
