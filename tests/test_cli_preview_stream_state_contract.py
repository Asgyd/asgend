#!/usr/bin/env python3
"""Regressions for large workspace preview refresh and reused stream IDs."""
from __future__ import annotations
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_cli_preview_stream_state_contract.py PROJECT_ROOT", file=sys.stderr)
        return 2
    cli = (pathlib.Path(sys.argv[1]) / "clients/asgend_cli.c").read_text(encoding="utf-8")

    preview_start = cli.index("static void cli_tui_refresh_open_preview(const Client *client)")
    preview_end = cli.index("static void cli_tui_maybe_stream_preview", preview_start)
    preview = cli[preview_start:preview_end]
    assert "CLI_WORKSPACE_FILE_MAX" in preview
    assert "cli_tui_show_workspace_chunk(client, path, offset, false)" in preview
    assert "preview_chunk_offset" in preview, "refresh must retain the visible chunk offset"

    thinking_start = cli.index("static CliThinkingOutputState *cli_thinking_state")
    thinking_end = cli.index("static void cli_thinking_break_current_line", thinking_start)
    thinking = cli[thinking_start:thinking_end]
    assert "if (create && state->completed)" in thinking
    assert 'snprintf(state->stream_id' in thinking

    tool_start = cli.index("static CliToolOutputState *cli_tool_state")
    tool_end = cli.index("static void cli_tool_finish_state", tool_start)
    tool = cli[tool_start:tool_end]
    assert "if (create && state->completed)" in tool
    assert 'snprintf(state->call_key' in tool

    print("CLI preview and stream lifecycle contracts verified")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
