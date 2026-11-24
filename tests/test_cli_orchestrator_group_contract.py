#!/usr/bin/env python3
"""Ensure orchestration lifecycle records are accumulated into one collapsible tile."""
from __future__ import annotations
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_cli_orchestrator_group_contract.py PROJECT_ROOT", file=sys.stderr)
        return 2
    cli = (pathlib.Path(sys.argv[1]) / "clients/asgend_cli.c").read_text(encoding="utf-8")
    start = cli.index("static bool cli_tui_is_orchestrator_group_event")
    end = cli.index("static bool cli_tui_render_event", start)
    group = cli[start:end]
    for event in ("orchestration_started", "agent_selected", "analysis_started",
                  "decomposition_completed", "resources_selected", "model_selected", "task_resumed"):
        assert f'"{event}"' in group, f"missing orchestrator child event {event}"
    render_start = cli.index("static bool cli_tui_render_event")
    render_end = cli.index('    const char *kind = (strstr(type, "progress")', render_start)
    render = cli[render_start:render_end]
    assert 'cli_tui_upsert(key, ASGEND_TUI_TILE_ACTION,' in render
    assert 'append, active, active);' in render, "child records must append while group remains active"
    assert 'strcmp(type, "orchestrator_waiting_input") != 0' in render, "spinner must stop while waiting for a user answer"
    assert 'active ? "Оркестратор ↓" : "Оркестратор →"' in render
    assert 'cli_tui_close_key("orchestrator:summary")' in render
    assert 'strcmp(type, "task_completed") == 0' in render
    assert 'strcmp(type, "task_failed") == 0' in render
    print("orchestrator lifecycle events accumulate into a single collapsible tile")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
