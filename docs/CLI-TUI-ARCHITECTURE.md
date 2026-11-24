# ASGEND CLI/TUI architecture

This document describes the terminal client introduced in the CLI/TUI refresh.
The UI remains a native C implementation and does not add a runtime framework dependency.

## Layout

- **Project** (left): directory rooted at the process working directory. Directories are listed first; paths shown in the preview remain project-relative when navigating nested folders.
- **Chat** (center): user messages, assistant output, streaming activity, tool calls and orchestrator events use separate tile palettes. Streaming activity includes a small animated glyph; completed activities use a collapsed state marker.
- **Goals · MCP** (right): goals, MCP server status, then To-do. Opening a local or task-workspace file temporarily replaces this panel with a preview.
- **Composer**: fixed-width centered rectangle below the panels. A question from the agent overlays the lower portion of chat and displays selectable answers.

The layout recalculates from the current terminal width and height. Below the minimum usable dimensions, the UI displays a resize prompt instead of attempting to draw the three-column layout outside the terminal.

## Rendering and concurrency

`src/asgend_tui.c` owns terminal modes, resize handling, mouse input, layout, and rendering. It assembles the current frame and compares it against the previous frame. Only changed terminal rows are written; the entire alternate screen is cleared only on initial activation/resume or a geometry change. The spinner tick follows the same path, so an animation changes only the row containing the active tile.

`clients/asgend_cli.c` owns application operations and task watching. A watcher thread polls events/status and pushes JSON events into a mutex-protected bounded queue. The main thread drains a limited batch per iteration, updates view state, accepts keyboard/mouse input, and is the only thread that renders. The event batch limit prevents a busy token stream from starving navigation. If the bounded event queue fills, the oldest queued event is released before the new event is added.

The TUI library is deliberately unaware of JSON, RPC, model providers, or the filesystem. It receives typed UI state and data from the CLI, which keeps terminal rendering testable independently.

## Input reference

- `↑` / `↓`: navigate the focused panel or select a choice in an active question.
- `Tab`: move focus between project, chat, and right panel.
- `Enter`: open a selected file, enter a selected directory, open a file event from chat, or expand/collapse the selected chat tile.
- `/` or `n`: compose a new task; `o`: open a task by ID.
- `t` / `g`: add To-do / Goal; `d`: change the selected plan item's state.
- `b` / `Backspace`: move to the parent directory while the project panel is focused.
- `e`: open the selected preview in the configured external editor (task workspace only).
- `Esc`: close a preview or dismiss the question overlay; `q`: leave the TUI.
- `1` / `2` / `3`: expand all tiles, collapse eligible activity tiles, or use dynamic tile display.
- Mouse click/drag: select visible rows. Wheel: scroll the focused view; horizontal wheel in preview enables no-wrap panning.

## Verification

The build registers `tui_library_smoke`, `tui_terminal_layout`, and `cli_terminal_layout`. The integration suite includes PTY coverage for multiple terminal sizes, partial-row redraw, directory navigation, mouse drag selection, file preview scrolling, resize restoration, and multi-client plan notifications.

## Implementation checklist

- [x] Responsive three-panel geometry and a minimum terminal-size guard.
- [x] Rectangular panels with per-panel and per-message palettes.
- [x] Dirty-row rendering, resize handling, and a main-thread animation tick.
- [x] Bounded asynchronous task-event queue and event batches to preserve input responsiveness.
- [x] Project file navigation, preview, horizontal no-wrap panning, and task-workspace edit integration.
- [x] Change preview with added/removed line palettes and context around the changed segment.
- [x] Distinct user/assistant/tool/orchestrator tiles and question overlay with arrow-key choice selection.
- [x] PTY regression coverage for layout, mouse selection, directory navigation, preview scrolling, resize, and multi-client plan refresh.


## Streamed model output

The core consumes OpenAI-compatible SSE `delta` fragments and persists each visible assistant text fragment as `assistant_delta`. Provider reasoning fields (`reasoning_content`, `reasoning`, `thinking`, or `analysis`) are published as `llm_thinking_delta` text fragments. Tool-call argument fragments use `tool_call_stream_delta.argument_delta`; the CLI/TUI groups them by model, stream ID, and call key. Line-mode CLI polls task events every 120 ms; the TUI watcher polls at approximately 110 ms and applies backpressure instead of dropping queued events. These are provider-emitted text fragments, not raw tokenizer token IDs; authoritative completion-token usage is shown only when the provider returns it.

**Sensitivity:** Thinking and partial tool-call argument text is included in persisted task-event payloads so polling clients can render it live. These payloads can contain private reasoning or sensitive tool parameters. Protect the task-event API with the normal auth token and protect the database/backups as sensitive application data.

### CLI stream lifecycles

Line-mode CLI keeps Thinking and tool-call state independently per `(model_name, stream_id)` and `call_key`, rather than using one global process indicator. Interleaved fragments remain labelled by their originating model. Each state is collapsed on its own `llm_status` completion/error event; task termination is a final cleanup fallback. The TUI uses a separate incremental tile for each model/stream/call.

