# CLI/TUI refactor — staged TODO

## Stage 1 — Streaming and model isolation
- [x] Stop showing byte counters as the body of active Thinking / orchestrator tiles.
- [x] Render active progress with a live spinner and a descriptive state label.
- [x] Keep assistant, Thinking, tool-call, orchestrator, and status tiles keyed by model fingerprint + stream ID.
- [x] Close only the stream associated with the completing model/stream; stop its spinner and collapse it.
- [x] Keep all streamed processes correlated by model/stream/call; Stage 5 now makes provider Thinking and tool-call deltas visible to the local CLI/TUI.
- [x] Run isolated Release build and targeted tests; full integration did not finish within the isolated runner time limit and remains unconfirmed.

## Stage 2 — Visual language and terminal geometry
- [x] Apply the official Catppuccin Mocha palette to semantic UI roles using truecolor ANSI sequences.
- [x] Remove all box-drawing borders from CLI/TUI source output; use only accent-filled surfaces and row highlights.
- [x] Make columns fill the height down to the composer and collapse right then left as width shrinks.
- [x] Add file-extension icons and return focus to the composer after a mouse click in the chat.
- [x] Add PTY checks for width/height constraints, pane visibility thresholds, palette roles, and borderless rendering; target suite passed 9/9.


## Stage 3 — Session selection and welcome screen
- [x] Make the selected task workspace the root of the left file tree; never enumerate the CLI process current directory.
- [x] Implement `/session` centered picker with search, arrow navigation, title, status and creation date.
- [x] Add centered welcome/task picker with selectable active-task tiles and bottom composer.
- [x] Default focus to chat/composer and cover welcome/selection with PTY/API tests.
- [x] Add CLI contract tests for session selection and task-scoped workspace RPC.

Stage 3 validation: clean Release build and targeted TUI/CLI test suite passed in the isolated workspace.

## Stage 4 — Integration and quality review
- [x] Perform a clean Release build in an isolated build directory.
- [x] Re-run layout, session, stream-correlation, and orchestration-group contract tests after review fixes.
- [x] Run all non-integration CTest cases: 24/24 passed.
- [ ] Confirm the full `integration` test: it does not finish in the isolated runner, stalling in `OpenAI-compatible SSE HTTP/parse/timeouts/cancellation` before reaching CLI/TUI integration scenarios. This remains unconfirmed and is recorded as a test-harness/environment blocker rather than a pass.
- [x] Fix orchestration event grouping so `orchestration_started`, profile/model selection, analysis, resources and resume events accumulate inside a single subdued tile; pause spinner at `orchestrator_waiting_input`, resume on `task_resumed`, and collapse on terminal task events.
- [x] Make initial focus and input focus consistent with the center chat/composer; restore chat focus when closing `/session`.
- [x] Package a clean final archive without build artifacts or temporary files.

Final validation summary before streaming Stage 5: the previous refactor's non-integration checks passed. During Stage 5, a clean Release build and 25/25 non-integration tests passed. The full integration test stalls in the OpenAI-compatible SSE fault cases before reaching CLI/TUI-specific integration steps, so no full-integration pass is claimed.

## Stage 5 — Verify and tighten token-by-token CLI streaming
- [x] Trace the complete path from OpenAI-compatible SSE `choices[].delta.content` through `append_stream_content()` and persisted `assistant_delta` events to CLI/TUI output.
- [x] Confirm that visible assistant text is already appended incrementally in TUI tiles and emitted incrementally in line-mode CLI; do not mislabel byte counters as tokens.
- [x] Reduce line-mode polling latency to 120 ms so a slow stream is displayed while generation is in progress, rather than arriving in large batches.
- [x] Use the actual `model_name` metadata in the line-mode stream header.
- [x] Add regression checks for streamed text fragment ordering, live event delivery, and model-labelled output.
- [x] Prevent TUI event-queue overflow from dropping streamed fragments; watcher applies backpressure until the render thread drains queue space.
- [x] Stream provider Thinking text and partial tool-call argument fragments to the local CLI/TUI with model/stream/call identity; this makes task-event payloads sensitive and requires normal authenticated access controls.
- [x] Complete final verification and resolve issues found during review.
- [x] Run clean isolated Release build and the complete available non-integration CTest suite.
- [x] Package a final source archive without build artifacts or temporary files.
- [ ] Confirm the full `integration` test. It stalls in the existing OpenAI-compatible SSE fault cases (`HTTP/parse/timeouts/cancellation`) before it reaches CLI/TUI integration checks; no pass is claimed.

Stage 5 implementation validation: clean Release build succeeded; 25/25 CTest cases outside the long-running `integration` scenario passed, including the live mock-SSE test that verifies actual Thinking, answer and tool-argument text deltas. The full integration scenario remains unconfirmed due to the SSE fault-case hang before the CLI/TUI flows.

Streaming terminology: the CLI renders provider text deltas as they arrive. A delta can contain part of a token or several tokens; the application does not receive raw tokenizer token IDs. A numeric completion-token count is shown only when the provider includes `usage.completion_tokens`.

## Stage 6 — Fix plan-title clipping found by PTY integration
- [x] Reproduce the `integration` failure using the last streaming archive with `asgend-cli-stream-final-followup.diff` already applied.
- [x] Inspect the failing two-client flow and confirm plan-change notification reaches the second TUI; the rendered title was clipped to `TUI_TWO_CLIENT_SYNC_TOD`, so the PTY test could not observe the complete title.
- [x] Increase the right panel's minimum width just enough to display the status marker plus a 24-character title without truncation, while preserving responsive hide-right-then-left behavior.
- [x] Add a renderer regression covering complete To-do/Goal titles in a 120-column terminal; retain the integration assertion that the second client sees the change without manual refresh.
- [x] Run a clean Release build and all non-integration tests in isolation: 25/25 passed without compiler warnings.
- [ ] Confirm the full `integration` test. In this isolated environment it stalls in the earlier OpenAI-compatible SSE HTTP/parse/timeouts/cancellation block. A disposable diagnostic copy skipped only that block and confirmed the two-client To-do/Goal notification scenario passes; the broader harness later stalled in a different CLI question/answer flow, so a full end-to-end pass remains unconfirmed.

### Failure analysis
The user-provided traceback is in `run_cli_two_client_plan_sync`, waiting for `○ TUI_TWO_CLIENT_SYNC_TODO`. The captured screen shows the item arriving as `○ TUI_TWO_CLIENT_SYNC_TOD`, one character short. The plan notification path is functioning; the failure is caused by right-panel content width (panel padding + status marker + title) rather than missed event delivery. The renderer's minimum right-panel width is now 29 columns, with regression assertions on the full To-do and Goal labels.
