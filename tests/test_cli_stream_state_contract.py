#!/usr/bin/env python3
"""Regression checks for streamed CLI activity and per-model event correlation."""
from __future__ import annotations

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_cli_stream_state_contract.py PROJECT_ROOT", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])
    cli = (root / "clients/asgend_cli.c").read_text(encoding="utf-8")
    llm = (root / "src/llm.c").read_text(encoding="utf-8")
    agent = (root / "src/agent.c").read_text(encoding="utf-8")

    render_start = cli.index("static bool cli_tui_render_event(json_object *event)")
    render_end = cli.index('    if (strcmp(type, "task_created") == 0', render_start)
    streaming = cli[render_start:render_end]
    assert '"Thinking · %zu байт"' not in streaming
    assert '"%zu байт' not in streaming
    assert 'cli_tui_stream_key(key, sizeof(key), "assistant", model, stream_id)' in streaming
    assert 'cli_tui_stream_key(key, sizeof(key), "thinking", model, stream_id)' in streaming
    assert 'cli_tui_stream_key(key, sizeof(key), "orchestrator-stream", model, stream_id)' in streaming
    assert 'cli_tui_stream_key(key, sizeof(key), "tool-form", model, call_key)' in streaming
    assert 'cli_tui_close_model_stream_tools(model, stream_id)' in streaming

    assert '"_asgend_stream_id"' in llm and '"_asgend_model_name"' in llm
    assert 'json_object_object_del(message, "_asgend_stream_id")' in agent
    assert 'json_object_object_del(message, "_asgend_model_name")' in agent
    assert '"call_key", json_object_new_string(tool_call_key)' in agent
    assert '"model_name", json_object_new_string(response_model_name)' in agent

    # The transport streams textual deltas into the event log, and the CLI must
    # display the actual model name and poll frequently enough for live output.
    assert 'json_object_new_string_len(delta, (int)length)' in llm
    assert 'internal_stream ? "orchestrator_model_delta" : "assistant_delta"' in llm
    assert '#define CLI_STREAM_POLL_INTERVAL_NS 120000000L' in cli
    assert 'tv_nsec = CLI_STREAM_POLL_INTERVAL_NS' in cli
    line_mode = cli[cli.index('static bool print_event(json_object *event)'):]
    line_mode = line_mode[:line_mode.index('static ' , 20)] if 'static ' in line_mode[20:] else line_mode
    assert 'str_get(payload, "model_name", str_get(payload, "model", NULL))' in line_mode
    assert 'cli_write_stream_delta(delta)' in line_mode and 'fflush(stdout)' in line_mode
    assert 'bool continuing_stream = strcmp(type, "assistant_delta") == 0;' in line_mode
    assert 'pthread_cond_wait(&watch->event_space, &watch->mutex)' in cli
    assert 'pthread_cond_signal(&watch->event_space)' in cli
    assert 'oldest event is released' not in (root / "docs/CLI-TUI-ARCHITECTURE.md").read_text(encoding="utf-8")
    assert 'json_object_new_string_len(delta, (int)length)' in llm
    assert '"argument_delta", json_object_new_string(argument_delta ? argument_delta : "")' in llm
    assert 'const char *delta = str_get(payload, "delta", "")' in streaming
    assert 'str_get(payload, "argument_delta", "")' in streaming
    assert 'cli_write_stream_delta(delta)' in line_mode
    assert 'cli_write_stream_delta(argument_delta)' in line_mode
    # Line-mode process state must be keyed independently so an interleaved
    # model cannot collapse another stream's Thinking or tool-call indicator.
    assert 'CliThinkingOutputState cli_thinking_outputs[CLI_STREAM_STATE_CAPACITY]' in cli
    assert 'strcmp(state->stream_id, sid) == 0 && strcmp(state->model_name, model) == 0' in cli
    assert 'cli_thinking_finish(stream_id, model_name, false)' in line_mode
    assert 'cli_tool_finish_stream(stream_id, model_name, false)' in line_mode
    assert r'Tool call %s · %s\n' in cli
    assert 'header_printed' in cli
    print("CLI streams visible response, Thinking, and tool-call deltas with independent model/stream state")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
