#!/usr/bin/env python3
"""Deterministic runtime coverage for Core RPC task/model/LSP state transitions.

Usage: test_core_rpc_state_transitions.py <agent-orchestrator binary> <source root>

This suite uses only temporary paths and local mock endpoints/processes. It checks
successful state transitions rather than only required request fields.
"""
from __future__ import annotations

import importlib.util
import json
import sqlite3
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
from pathlib import Path
from typing import Any

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
TOKEN = "core-rpc-state-stage16-token"

_INTEGRATION_PATH = PROJECT / "tests" / "integration.py"
_saved_argv = sys.argv[:]
try:
    sys.argv = [str(_INTEGRATION_PATH), str(BINARY), str(PROJECT)]
    _spec = importlib.util.spec_from_file_location("asgend_core_rpc_state_transitions_integration", _INTEGRATION_PATH)
    if _spec is None or _spec.loader is None:
        raise RuntimeError("cannot load integration helpers")
    integration = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(integration)
finally:
    sys.argv = _saved_argv


def require(condition: bool, message: str, value: Any = None) -> None:
    if not condition:
        raise AssertionError(f"{message}: {value!r}")


def status(port: int, task_id: str) -> dict[str, Any]:
    result = integration.rpc(port, {"op": "status", "task_id": task_id}, token=TOKEN, timeout=4)
    require(result.get("ok") is True, f"status lookup for {task_id} should succeed", result)
    return result


def wait_terminal(port: int, task_id: str, timeout: float = 20, expected: str | None = None) -> dict[str, Any]:
    terminal_states = {"completed", "completed_without_model", "failed", "cancelled"}
    result = integration.wait_for(
        lambda: (lambda current: current if current.get("status") in terminal_states else None)(status(port, task_id)),
        timeout=timeout,
        label=f"task {task_id} reaches terminal state",
    )
    if expected is not None:
        require(result.get("status") == expected, f"task {task_id} terminal status should be {expected}", result)
    return result


def run(root: Path) -> None:
    # Existing deterministic peers provide a real streamed OpenAI-compatible
    # endpoint, a model subprocess with /health, and an LSP JSON-RPC subprocess.
    llm_port = integration.free_port()
    llm_server = ThreadingHTTPServer(("127.0.0.1", llm_port), integration.MockLLMHandler)
    llm_server.daemon_threads = True
    llm_thread = threading.Thread(target=llm_server.serve_forever, daemon=True)
    llm_thread.start()

    workspace = root / "lsp-project"
    workspace.mkdir()
    lsp_file = workspace / "rpc-stage16.lspx"
    lsp_file.write_text("stage 16 LSP fixture\n", encoding="utf-8")

    model_file = root / "tiny-rpc-model.gguf"
    integration.write_test_gguf(model_file)
    model_port = integration.free_port()
    model_log = root / "model-requests.jsonl"
    model = {
        "id": "rpc-stage16-model",
        "name": "Stage 16 RPC lifecycle model",
        "enabled": True,
        "auto_start": False,
        "start_at_boot": False,
        "executable": sys.executable,
        "model_path": str(model_file),
        "working_directory": str(root),
        "args": [str(PROJECT / "tests" / "managed_model_server.py"), "--port", str(model_port),
                 "--tag", "rpc-stage16", "--requests", str(model_log)],
        "port": model_port,
        "base_url": f"http://127.0.0.1:{model_port}/v1",
        "health_url": f"http://127.0.0.1:{model_port}/health",
        "model_name": "stage16-test-model",
        "api_key": "",
        "startup_timeout_seconds": 8,
        "shutdown_timeout_seconds": 2,
        "request_timeout_seconds": 5,
        "temperature": 0.0,
        "ram_estimate_mb": 0,
        "vram_estimate_mb": 0,
        "tier": "medium",
    }
    registry = {
        "defaults": {"model_id": "", "analysis_model_id": "", "decision_model_id": ""},
        "models": [model],
        "decision_models": [],
        "nodes": [],
    }
    lsp_config = [{
        "language": "plaintext",
        "command": sys.executable,
        "args": [str(PROJECT / "tests" / "mock_lsp_server.py")],
        "extensions": [".lspx"],
        "root_dir": str(workspace),
        "timeout_seconds": 3,
    }]

    db_path = root / "core-rpc-stage16.sqlite3"
    proc = None
    try:
        proc = integration.start_server(
            integration.free_port(), db_path, token=TOKEN,
            llm_enabled=True, llm_base_url=f"http://127.0.0.1:{llm_port}/v1",
            model_registry=registry, lsp_servers=lsp_config, sandbox_enabled=False,
        )
        port = proc.test_port

        # 1) Cancel a task while its model response is deliberately still streaming.
        cancelling = integration.rpc(port, {
            "op": "submit",
            "task": "MCP_PROTOCOL_TOOL_TEST: deterministic running cancellation __SSE_CANCEL_STREAM__",
        }, token=TOKEN, timeout=4)
        require(cancelling.get("ok") is True and isinstance(cancelling.get("task_id"), str),
                "cancel fixture task should be accepted", cancelling)
        cancel_id = cancelling["task_id"]

        def stream_started() -> list[dict[str, Any]] | None:
            reply = integration.rpc(port, {"op": "events", "task_id": cancel_id, "limit": 500}, token=TOKEN, timeout=4)
            require(reply.get("ok") is True, "events lookup for cancellation fixture should succeed", reply)
            rows = reply.get("events", [])
            return rows if any(row.get("type") == "llm_status" and
                               row.get("payload", {}).get("status") == "streaming" for row in rows) else None

        started_rows = integration.wait_for(stream_started, timeout=8, label="model stream begins before RPC cancel")
        cancel_reply = integration.rpc(port, {"op": "cancel", "task_id": cancel_id}, token=TOKEN, timeout=4)
        require(cancel_reply.get("ok") is True and cancel_reply.get("status") == "cancellation_requested",
                "cancel should acknowledge an active task", cancel_reply)
        cancelled = wait_terminal(port, cancel_id, timeout=5, expected="cancelled")
        cancel_events = integration.rpc(port, {"op": "events", "task_id": cancel_id, "limit": 500},
                                        token=TOKEN, timeout=4)
        require(cancel_events.get("ok") is True, "cancel events lookup should succeed", cancel_events)
        rows = cancel_events.get("events", [])
        require(any(row.get("type") == "task_cancel_requested" for row in rows),
                "cancel should persist task_cancel_requested event", rows)
        require(any(row.get("type") == "llm_status" and row.get("payload", {}).get("status") == "cancelled"
                    for row in rows), "stream should publish cancelled status", rows)
        require(cancelled.get("attempts", 0) >= 1, "running cancellation should exercise a claimed attempt", cancelled)

        # 2) Answer a real waiting_input task, verify durable user_answer event and completion.
        asked = integration.rpc(port, {
            "op": "submit", "task": "NEEDS_CLARIFICATION: Stage 16 successful answer transition",
        }, token=TOKEN, timeout=4)
        require(asked.get("ok") is True and isinstance(asked.get("task_id"), str),
                "clarification fixture should be accepted", asked)
        answer_id = asked["task_id"]
        waiting = integration.wait_for(
            lambda: (lambda current: current if current.get("status") == "waiting_input" else None)(status(port, answer_id)),
            timeout=12, label="task enters waiting_input before answer",
        )
        require(isinstance(waiting.get("result", {}).get("question"), dict),
                "waiting_input result should include a question object", waiting)
        answer = integration.rpc(port, {
            "op": "answer", "task_id": answer_id,
            "option_id": "local", "answer": "Keep this workflow local to the device",
        }, token=TOKEN, timeout=4)
        require(answer.get("ok") is True and answer.get("status") == "queued",
                "answer should persist and requeue waiting task", answer)
        answer_final = wait_terminal(port, answer_id, timeout=18, expected="completed")
        require(answer_final.get("attempts", 0) >= 1, "answered task should execute after resuming", answer_final)
        answer_events = integration.rpc(port, {"op": "events", "task_id": answer_id, "limit": 700},
                                        token=TOKEN, timeout=4)
        require(answer_events.get("ok") is True, "answered task event lookup should succeed", answer_events)
        answer_rows = answer_events.get("events", [])
        user_answer = next((row for row in answer_rows if row.get("type") == "user_answer"), None)
        require(user_answer is not None, "answer should persist a user_answer event", answer_rows)
        require(user_answer.get("payload", {}).get("answer") == "Keep this workflow local to the device" and
                user_answer.get("payload", {}).get("option_id") == "local",
                "user_answer event should retain answer and option id", user_answer)

        # 3) Seed one known dead letter in the isolated DB, then exercise the live
        # RPC redrive path and confirm its event and attempt budget are updated.
        retry_id = "stage16-dead-letter-0001"
        retry_request = {"op": "submit", "task": "Stage 16 deterministic dead-letter retry", "priority": 5}
        with sqlite3.connect(db_path) as con:
            con.execute(
                """INSERT INTO tasks(id, task_json, status, attempts, max_attempts, priority, error)
                   VALUES(?, ?, 'failed', 2, 2, 5, ?)""",
                (retry_id, json.dumps(retry_request, separators=(",", ":")), "deterministic stage 16 fixture failure"),
            )
        seeded = status(port, retry_id)
        require(seeded.get("status") == "failed" and seeded.get("attempts") == 2,
                "dead-letter fixture must be terminal before retry", seeded)
        retried = integration.rpc(port, {"op": "retry_task", "task_id": retry_id}, token=TOKEN, timeout=4)
        require(retried.get("ok") is True and retried.get("task_id") == retry_id and
                retried.get("status") == "queued" and retried.get("additional_attempts") == 1,
                "retry_task should queue one additional attempt", retried)
        retry_final = wait_terminal(port, retry_id, timeout=20, expected="completed")
        require(retry_final.get("attempts") == 3 and retry_final.get("max_attempts") == 3,
                "successful retry should consume exactly one additional attempt", retry_final)
        retry_events = integration.rpc(port, {"op": "events", "task_id": retry_id, "limit": 200},
                                       token=TOKEN, timeout=4)
        require(retry_events.get("ok") is True and any(event.get("type") == "task_redriven"
                for event in retry_events.get("events", [])), "retry should persist task_redriven event", retry_events)
        require(integration.rpc(port, {"op": "retry_task", "task_id": retry_id}, token=TOKEN, timeout=4).get("ok") is False,
                "completed task must not be retried as a dead letter")

        # 4) Exercise successful local managed-model start/stop and enable/disable.
        model_start = integration.rpc(port, {"op": "model_start", "model_id": "rpc-stage16-model", "kind": "llm"},
                                      token=TOKEN, timeout=12)
        require(model_start.get("ok") is True, "model_start should launch the test model", model_start)

        def model_row() -> dict[str, Any]:
            health = integration.rpc(port, {"op": "health"}, token=TOKEN, timeout=4)
            require(health.get("ok") is True, "health should remain available during model lifecycle", health)
            rows = health.get("model_status", {}).get("models", [])
            found = next((row for row in rows if row.get("id") == "rpc-stage16-model"), None)
            require(found is not None, "health should include the configured test model", rows)
            return found

        require(model_row().get("running") is True, "model_start should produce running=true", model_row())
        model_stop = integration.rpc(port, {"op": "model_stop", "model_id": "rpc-stage16-model", "kind": "llm"},
                                     token=TOKEN, timeout=8)
        require(model_stop.get("ok") is True, "model_stop should stop the test model", model_stop)
        require(model_row().get("running") is False, "model_stop should produce running=false", model_row())
        disable = integration.rpc(port, {"op": "model_disable", "model_id": "rpc-stage16-model", "kind": "llm"},
                                  token=TOKEN, timeout=5)
        require(disable.get("ok") is True and model_row().get("enabled") is False,
                "model_disable should persist enabled=false", disable)
        enable = integration.rpc(port, {"op": "model_enable", "model_id": "rpc-stage16-model", "kind": "llm"},
                                 token=TOKEN, timeout=5)
        require(enable.get("ok") is True and model_row().get("enabled") is True,
                "model_enable should persist enabled=true", enable)

        # 5) Run real LSP JSON-RPC against the disposable workspace file.
        diagnostics = integration.rpc(port, {"op": "lsp_diagnostics", "file_path": str(lsp_file)},
                                      token=TOKEN, timeout=8)
        require(diagnostics.get("ok") is True, "LSP diagnostics should return a successful RPC envelope", diagnostics)
        text = diagnostics.get("diagnostics", "")
        require("mock error from LSP" in text and ":1:1: ERROR:" in text,
                "diagnostics response should contain the mock server's error with location", diagnostics)

        print("core RPC state transition tests passed: cancel, answer, retry, model lifecycle, LSP diagnostics")
    finally:
        if proc is not None:
            integration.stop_server(proc)
        llm_server.shutdown()
        llm_server.server_close()
        llm_thread.join(timeout=2)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="asgend-core-rpc-state-stage16-") as directory:
        run(Path(directory))


if __name__ == "__main__":
    main()
