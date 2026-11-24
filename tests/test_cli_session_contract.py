#!/usr/bin/env python3
"""Regression checks for the CLI session picker and task-scoped workspace."""
from __future__ import annotations

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_cli_session_contract.py PROJECT_ROOT", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])
    cli = (root / "clients/asgend_cli.c").read_text(encoding="utf-8")
    tui = (root / "src/asgend_tui.c").read_text(encoding="utf-8")
    header = (root / "include/asgend_tui.h").read_text(encoding="utf-8")

    assert 'strncmp(value, "/session", 8)' in cli, "composer must recognize /session"
    assert "asgend_tui_open_session_picker(&cli_tui, query)" in cli
    assert "cli_tui_handle_session_key" in cli
    assert "asgend_tui_selected_session" in cli, "selection must return a real task ID"
    assert 'str_get(row, "created_at", "")' in cli, "task creation date must reach the TUI"
    assert 'request_op("workspace_list")' in cli
    assert 'json_object_object_add(request, "task_id", json_object_new_string(scope_id))' in cli
    assert 'asgend_tui_set_project_path(&cli_tui, "Выберите задачу")' in cli
    assert "cli_tui_local_directory" not in cli, "workspace tree must not fall back to CLI cwd"
    assert "welcome_screen" in header and "session_picker_active" in header
    assert "tui_render_welcome_panel" in tui and "tui_render_session_picker" in tui
    assert "created_at" in header and "session_filtered_indices" in header
    assert "cli_tui.focused_pane = 1;" in cli, "default TUI focus must be chat"
    print("CLI /session, welcome task list, task workspace, and default focus contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
