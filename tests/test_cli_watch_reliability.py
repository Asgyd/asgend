#!/usr/bin/env python3
"""Contracts for lossless terminal-event draining and bounded watcher shutdown."""
from __future__ import annotations

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_cli_watch_reliability.py PROJECT_ROOT", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])
    cli = (root / "clients/asgend_cli.c").read_text(encoding="utf-8")

    plan_start = cli.index("static void *cli_tui_plan_watch_main(void *opaque)")
    plan_end = cli.index("static void cli_tui_start_plan_watch", plan_start)
    plan_watch = cli[plan_start:plan_end]
    assert "cli_rpc_timeout_seconds = 2;" in plan_watch
    assert "cli_rpc_quiet = true;" in plan_watch
    assert "cli_rpc_timeout_seconds = 60;" in plan_watch

    task_start = cli.index("static void *cli_tui_task_watch_main(void *opaque)")
    task_end = cli.index("static json_object *cli_tui_watch_pop_event", task_start)
    task_watch = cli[task_start:task_end]
    assert "int64_t last_event_id = 0;" in task_watch
    assert 'json_object_new_int64(last_event_id)' in task_watch
    assert 'json_object_new_int(CLI_TUI_EVENT_PAGE_LIMIT)' in task_watch
    assert "terminal_status_seen" in task_watch
    assert "terminal_before_page" in task_watch, "must fetch at least one page after terminal status"
    assert "fetched_count < CLI_TUI_EVENT_PAGE_LIMIT" in task_watch
    assert "events_fetch_ok" in task_watch, "failed event RPC must not be treated as an empty page"
    assert "cli_tui_watch_mark_finished(watch);" in task_watch

    publish_start = cli.index("static void cli_tui_watch_publish_status")
    publish_end = cli.index("static bool cli_tui_status_is_terminal", publish_start)
    publish = cli[publish_start:publish_end]
    assert "watch->finished = true" not in publish, "status terminal must not skip event drain"

    print("CLI watcher contracts verified: int64 event cursor, post-terminal drain, bounded plan-watcher RPC")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
