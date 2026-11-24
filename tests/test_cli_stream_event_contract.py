#!/usr/bin/env python3
"""Isolated end-to-end contract for streamed answer and Thinking deltas."""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import shutil
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path


def load_integration_helpers(binary: Path, project: Path):
    helper_path = project / "tests" / "integration.py"
    saved_argv = sys.argv[:]
    sys.argv = [str(helper_path), str(binary), str(project)]
    try:
        spec = importlib.util.spec_from_file_location("asgend_integration_stream_helpers", helper_path)
        if spec is None or spec.loader is None:
            raise RuntimeError("could not load integration helpers")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module
    finally:
        sys.argv = saved_argv


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_cli_stream_event_contract.py CORE_BINARY PROJECT_ROOT", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    project = Path(sys.argv[2]).resolve()
    helpers = load_integration_helpers(binary, project)

    with tempfile.TemporaryDirectory(prefix="asgend-stream-contract-") as tmp:
        root = Path(tmp)
        model_port = helpers.free_port()
        mock_server = ThreadingHTTPServer(("127.0.0.1", model_port), helpers.MockLLMHandler)
        mock_server.daemon_threads = True
        server_thread = threading.Thread(target=mock_server.serve_forever, daemon=True)
        server_thread.start()
        core = None
        try:
            agents_dir = root / "agents"
            shutil.copytree(project / "config" / "agents", agents_dir)
            generalist_path = agents_dir / "generalist.json"
            generalist = json.loads(generalist_path.read_text(encoding="utf-8"))
            generalist["tools"].extend([
                "mcp__testserver__resources_list", "mcp__testserver__resource_read",
                "mcp__testserver__prompts_list", "mcp__testserver__prompt_get",
            ])
            generalist_path.write_text(json.dumps(generalist, ensure_ascii=False), encoding="utf-8")
            helpers.MockLLMHandler.reasoning_first_token_remaining = 1
            core = helpers.start_server(
                helpers.free_port(), root / "stream.sqlite3", llm_enabled=True,
                llm_base_url=f"http://127.0.0.1:{model_port}/v1", llm_timeout_seconds=3,
                agents_dir=agents_dir,
                mcp_servers=[{
                    "name": "testserver", "command": sys.executable,
                    "args": [str(project / "tests" / "mock_mcp_server.py")],
                    "read_only_paths": [str(project / "knowledge")], "timeout_seconds": 5,
                }],
            )
            core_port = int(json.loads((root / "config.json").read_text(encoding="utf-8"))["server"]["port"])
            submitted = helpers.rpc(core_port, {
                "op": "submit",
                "task": "Verify visible streaming and Thinking deltas __SSE_REASONING_FIRST_TOKEN__",
                "agent": "generalist",
            })
            assert submitted.get("ok") and submitted.get("task_id"), submitted
            final = helpers.wait_for(
                lambda: (lambda status: status if status.get("status") in
                         ("completed", "completed_without_model", "failed", "cancelled") else None)(
                             helpers.rpc(core_port, {"op": "status", "task_id": submitted["task_id"]})),
                timeout=25, label="streamed task completion",
            )
            events = helpers.rpc(core_port, {"op": "events", "task_id": submitted["task_id"], "limit": 1000})["events"]
            thinking = [event for event in events if event.get("type") == "llm_thinking_delta"]
            visible = [event for event in events if event.get("type") == "assistant_delta"]
            assert final.get("status") == "completed", final
            assert len(thinking) >= 2, thinking
            assert len(visible) >= 1, visible
            thinking_text = "".join(event.get("payload", {}).get("delta", "") for event in thinking)
            assert "Thinking token one." in thinking_text and "Thinking token two." in thinking_text, thinking
            for event in thinking + visible:
                payload = event.get("payload", {})
                assert isinstance(payload.get("delta"), str) and payload["delta"], event
                assert isinstance(payload.get("sequence"), int) and payload["sequence"] > 0, event
                assert payload.get("model_name") and payload.get("stream_id"), event
            visible_sequences = {}
            for event in visible:
                payload = event["payload"]
                key = (payload["model_name"], payload["stream_id"])
                visible_sequences.setdefault(key, []).append(payload["sequence"])
            assert all(numbers == sorted(numbers) for numbers in visible_sequences.values()), visible_sequences

            tool_task = helpers.rpc(core_port, {
                "op": "submit", "agent": "generalist",
                "task": "MCP_PROTOCOL_TOOL_TEST: stream the MCP resource listing call",
            })
            assert tool_task.get("ok") and tool_task.get("task_id"), tool_task
            tool_final = helpers.wait_for(
                lambda: (lambda status: status if status.get("status") in
                         ("completed", "completed_without_model", "failed", "cancelled") else None)(
                             helpers.rpc(core_port, {"op": "status", "task_id": tool_task["task_id"]})),
                timeout=25, label="streamed tool-call completion",
            )
            tool_events = helpers.rpc(core_port, {"op": "events", "task_id": tool_task["task_id"], "limit": 1000})["events"]
            tool_deltas = [event for event in tool_events if event.get("type") == "tool_call_stream_delta"]
            assert tool_final.get("status") == "completed", tool_final
            assert tool_deltas, tool_events
            assert any(isinstance(event.get("payload", {}).get("argument_delta"), str) and
                       event["payload"]["argument_delta"] for event in tool_deltas), tool_deltas
            assert all(event.get("payload", {}).get("model_name") and
                       event.get("payload", {}).get("call_key") for event in tool_deltas), tool_deltas
            print("stream event contract passed: Thinking, answer, and tool-call deltas stream with correlation metadata")
            return 0
        finally:
            if core is not None:
                helpers.stop_server(core)
            mock_server.shutdown()
            mock_server.server_close()
            server_thread.join(timeout=2)


if __name__ == "__main__":
    import os
    raise SystemExit(main())
