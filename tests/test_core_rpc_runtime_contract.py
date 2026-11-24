#!/usr/bin/env python3
"""Runtime request/response contract smoke tests for local Core JSON-line RPC.

Usage: test_core_rpc_runtime_contract.py <agent-orchestrator binary> <source root>

This deliberately exercises the live TCP RPC path. Mutating operations are limited
 to an isolated temporary config/database, a task-local workspace file, and a
 throwaway global plan item that is deleted in a finally block.
"""
from __future__ import annotations

import base64
import importlib.util
import json
import sys
import tempfile
from pathlib import Path
from typing import Any

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
TOKEN = "core-rpc-contract-stage15-token"

# Reuse the project's existing disposable server setup and newline-delimited TCP
# helper so this suite tests the same real server transport as its integration tests.
_INTEGRATION_PATH = PROJECT / "tests" / "integration.py"
_saved_argv = sys.argv[:]
try:
    sys.argv = [str(_INTEGRATION_PATH), str(BINARY), str(PROJECT)]
    _spec = importlib.util.spec_from_file_location("asgend_core_rpc_runtime_contract_integration", _INTEGRATION_PATH)
    if _spec is None or _spec.loader is None:
        raise RuntimeError("cannot load integration helpers")
    integration = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(integration)
finally:
    sys.argv = _saved_argv


def require(condition: bool, message: str, value: Any = None) -> None:
    if not condition:
        raise AssertionError(f"{message}: {value!r}")


def is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def expect_error(reply: dict[str, Any], message: str) -> None:
    require(reply.get("ok") is False, message + " should set ok=false", reply)
    require(isinstance(reply.get("error"), str) and bool(reply["error"]), message + " should have a non-empty error", reply)


def expect_ok(reply: dict[str, Any], message: str) -> None:
    require(reply.get("ok") is True, message + " should set ok=true", reply)


def run_contracts(port: int) -> None:
    def rpc(payload: dict[str, Any]) -> dict[str, Any]:
        result = integration.rpc(port, payload, token=TOKEN, timeout=5)
        require(isinstance(result, dict), "RPC response must be a JSON object", result)
        require(isinstance(result.get("ok"), bool), "RPC response must contain boolean ok", result)
        return result

    # One real task lets the tests reach the second-layer task/path validators
    # after the dispatcher's common task-ID resolver has run.
    submitted = rpc({"op": "submit", "task": "Stage 15 RPC contract smoke", "idempotency_key": "stage15-contract-smoke", "priority": 4})
    expect_ok(submitted, "submit")
    require(submitted.get("accepted") is True, "submit.accepted must be true", submitted)
    task_id = submitted.get("task_id")
    require(isinstance(task_id, str) and bool(task_id), "submit.task_id must be a non-empty string", submitted)
    require(submitted.get("status") in {"queued", "existing"}, "submit.status must identify accepted task", submitted)
    require(isinstance(submitted.get("duplicate"), bool), "submit.duplicate must be boolean", submitted)
    require(is_int(submitted.get("priority")), "submit.priority must be integer", submitted)

    # All documented dispatch operations get a negative request-envelope check
    # where the dispatcher promises a required field or a typed parameter.
    missing_or_invalid: list[tuple[str, dict[str, Any], str]] = [
        ("answer task_id", {"op": "answer"}, "task_id is required"),
        ("answer answer", {"op": "answer", "task_id": task_id}, "answer is required"),
        ("artifact_list task_id", {"op": "artifact_list"}, "task_id is required"),
        ("cancel task_id", {"op": "cancel"}, "task_id is required"),
        ("config_save bundle", {"op": "config_save"}, "main object"),
        ("config_update_settings settings", {"op": "config_update_settings"}, "settings object"),
        ("events task_id", {"op": "events"}, "task_id is required"),
        ("lsp_diagnostics file_path", {"op": "lsp_diagnostics"}, "file_path is required"),
        ("model_disable model_id", {"op": "model_disable"}, "model_id is required"),
        ("model_enable model_id", {"op": "model_enable"}, "model_id is required"),
        ("model_start model_id", {"op": "model_start"}, "model_id is required"),
        ("model_stop model_id", {"op": "model_stop"}, "model_id is required"),
        ("plan_add kind/title", {"op": "plan_add", "scope_id": "global"}, "kind and title are required"),
        ("plan_delete item_id", {"op": "plan_delete", "scope_id": "global"}, "item_id is required"),
        ("plan_update item_id", {"op": "plan_update", "scope_id": "global"}, "item_id is required"),
        ("retry_task task_id", {"op": "retry_task"}, "task_id is required"),
        ("status task_id", {"op": "status"}, "task_id is required"),
        ("submit task/prompt", {"op": "submit"}, "task is required"),
        ("workspace_list task_id", {"op": "workspace_list"}, "task_id is required"),
        ("workspace_read task_id", {"op": "workspace_read"}, "task_id is required"),
        ("workspace_read path", {"op": "workspace_read", "task_id": task_id}, "path is required"),
        ("workspace_read_bytes task_id", {"op": "workspace_read_bytes"}, "task_id is required"),
        ("workspace_read_bytes path", {"op": "workspace_read_bytes", "task_id": task_id}, "path is required"),
        ("workspace_read_chunk task_id", {"op": "workspace_read_chunk"}, "task_id is required"),
        ("workspace_read_chunk path", {"op": "workspace_read_chunk", "task_id": task_id}, "path is required"),
        ("workspace_write task_id", {"op": "workspace_write"}, "task_id is required"),
        ("workspace_write path", {"op": "workspace_write", "task_id": task_id, "content": "x"}, "path is required"),
        ("list_tasks limit type", {"op": "list_tasks", "limit": "not-an-integer"}, "limit must be an integer"),
        ("dead_letters limit type", {"op": "dead_letters", "limit": "not-an-integer"}, "limit must be an integer"),
        ("plan_wait revision type", {"op": "plan_wait", "revision": "zero", "timeout_ms": 0}, "revision must be a non-negative integer"),
        ("plan_wait timeout type", {"op": "plan_wait", "revision": 0, "timeout_ms": "zero"}, "timeout_ms must be an integer"),
    ]
    for label, request, expected in missing_or_invalid:
        reply = rpc(request)
        expect_error(reply, label)
        require(expected.lower() in reply["error"].lower(), f"{label} error must explain the violated request contract", reply)

    # Operation-specific success response shapes on safe/read-only calls.
    ping = rpc({"op": "ping"})
    expect_ok(ping, "ping")
    require(isinstance(ping.get("service"), str) and isinstance(ping.get("version"), str), "ping service/version must be strings", ping)

    health = rpc({"op": "health"})
    expect_ok(health, "health")
    require(isinstance(health.get("tasks_by_status"), dict), "health.tasks_by_status must be an object", health)
    require(is_int(health.get("active_connections")) and is_int(health.get("workers")), "health counters must be integers", health)
    require(isinstance(health.get("draining"), bool), "health.draining must be boolean", health)
    require(isinstance(health.get("unix_socket_enabled"), bool) and isinstance(health.get("ui_enabled"), bool), "health listener flags must be boolean", health)

    agents = rpc({"op": "list_agents"})
    expect_ok(agents, "list_agents")
    require(isinstance(agents.get("agents"), list), "list_agents.agents must be an array", agents)
    require(all(isinstance(item, dict) and isinstance(item.get("id"), str) for item in agents["agents"]), "agent rows must expose string IDs", agents)

    tasks = rpc({"op": "list_tasks", "limit": 10})
    expect_ok(tasks, "list_tasks")
    require(isinstance(tasks.get("tasks"), list), "list_tasks.tasks must be an array", tasks)
    require(any(item.get("task_id") == task_id for item in tasks["tasks"]), "list_tasks must include the accepted task", tasks)

    dead = rpc({"op": "dead_letters", "limit": 10})
    expect_ok(dead, "dead_letters")
    require(isinstance(dead.get("dead_letters"), list), "dead_letters.dead_letters must be an array", dead)

    config = rpc({"op": "config_get"})
    expect_ok(config, "config_get")
    require(isinstance(config.get("main"), dict) and isinstance(config.get("models"), dict), "config_get.main/models must be objects", config)
    require(isinstance(config.get("agents"), list) and isinstance(config.get("restart_required"), bool), "config_get agents/restart_required shape mismatch", config)

    models = rpc({"op": "models"})
    expect_ok(models, "models")
    require("model_status" in models and isinstance(models.get("resources"), (dict, list)), "models must include model_status and resources", models)

    resources = rpc({"op": "resources"})
    expect_ok(resources, "resources")
    require(isinstance(resources.get("directories"), dict) and isinstance(resources.get("files"), dict), "resources directories/files must be objects", resources)
    require(isinstance(resources.get("decision_model_id"), str) and isinstance(resources.get("classification_parallel"), bool), "resources metadata shape mismatch", resources)

    mcp = rpc({"op": "mcp_status"})
    expect_ok(mcp, "mcp_status")
    require(isinstance(mcp.get("servers"), list), "mcp_status.servers must be an array", mcp)

    plan = rpc({"op": "plan_list", "scope_id": "global"})
    expect_ok(plan, "plan_list")
    require(plan.get("scope_id") == "global" and isinstance(plan.get("items"), list), "plan_list scope/items shape mismatch", plan)

    wait = rpc({"op": "plan_wait", "revision": 0, "timeout_ms": 0})
    expect_ok(wait, "plan_wait")
    require(is_int(wait.get("revision")) and isinstance(wait.get("changed"), bool) and isinstance(wait.get("stopping"), bool), "plan_wait response shape mismatch", wait)

    # Full success-path shape checks for task status/events/workspace/artifacts.
    status = rpc({"op": "status", "task_id": task_id})
    expect_ok(status, "status")
    require(status.get("task_id") == task_id and isinstance(status.get("status"), str), "status.task_id/status shape mismatch", status)
    require(is_int(status.get("attempts")) and is_int(status.get("priority")) and isinstance(status.get("cancel_requested"), bool), "status counters/flags shape mismatch", status)

    events = rpc({"op": "events", "task_id": task_id, "after_id": 0, "limit": 50})
    expect_ok(events, "events")
    require(isinstance(events.get("events"), list), "events.events must be an array", events)
    require(all(isinstance(event, dict) for event in events["events"]), "events entries must be objects", events)

    content = "stage15 RPC payload\nsecond line\n"
    path = "stage15-rpc-contract.txt"
    written = rpc({"op": "workspace_write", "task_id": task_id, "path": path, "content": content})
    expect_ok(written, "workspace_write")
    require(written.get("path") == path and written.get("size") == len(content.encode("utf-8")), "workspace_write path/size mismatch", written)

    listing = rpc({"op": "workspace_list", "task_id": task_id, "path": ""})
    expect_ok(listing, "workspace_list")
    require(isinstance(listing.get("entries"), list) and isinstance(listing.get("truncated"), bool), "workspace_list entries/truncated shape mismatch", listing)
    require(any(entry.get("name") == path and entry.get("kind") == "file" for entry in listing["entries"]), "workspace_list must include written file", listing)

    read = rpc({"op": "workspace_read", "task_id": task_id, "path": path})
    expect_ok(read, "workspace_read")
    require(read.get("path") == path and read.get("content") == content and read.get("size") == len(content.encode("utf-8")), "workspace_read content contract mismatch", read)

    chunk = rpc({"op": "workspace_read_chunk", "task_id": task_id, "path": path, "offset": 0, "limit": 8})
    expect_ok(chunk, "workspace_read_chunk")
    require(chunk.get("path") == path and isinstance(chunk.get("content"), str), "workspace_read_chunk path/content shape mismatch", chunk)
    require(is_int(chunk.get("size")) and chunk.get("offset") == 0 and is_int(chunk.get("next_offset")) and isinstance(chunk.get("eof"), bool), "workspace_read_chunk offsets/size/eof shape mismatch", chunk)
    require(chunk["next_offset"] > 0 and chunk["next_offset"] <= chunk["size"], "workspace_read_chunk must make bounded progress", chunk)

    byte_chunk = rpc({"op": "workspace_read_bytes", "task_id": task_id, "path": path, "offset": 0, "limit": 5})
    expect_ok(byte_chunk, "workspace_read_bytes")
    require(byte_chunk.get("path") == path and isinstance(byte_chunk.get("data_b64"), str), "workspace_read_bytes path/data_b64 shape mismatch", byte_chunk)
    raw = base64.b64decode(byte_chunk["data_b64"], validate=True)
    require(raw == content.encode("utf-8")[:5], "workspace_read_bytes base64 payload mismatch", byte_chunk)
    require(byte_chunk.get("offset") == 0 and byte_chunk.get("next_offset") == 5 and byte_chunk.get("size") == len(content.encode("utf-8")) and byte_chunk.get("eof") is False, "workspace_read_bytes offsets/size/eof mismatch", byte_chunk)

    artifacts = rpc({"op": "artifact_list", "task_id": task_id})
    expect_ok(artifacts, "artifact_list")
    require(isinstance(artifacts.get("artifacts"), list), "artifact_list.artifacts must be an array", artifacts)
    require(any(item.get("path") == path for item in artifacts["artifacts"]), "artifact_list must include workspace file", artifacts)

    # The plan CRUD response envelope is part of the public contract. Cleanup is
    # guaranteed even if any later assertion fails.
    marker = f"stage15-rpc-contract-{task_id[:12]}"
    created: dict[str, Any] | None = None
    try:
        created = rpc({"op": "plan_add", "scope_id": "global", "kind": "todo", "title": marker})
        expect_ok(created, "plan_add")
        require(is_int(created.get("item_id")) and created.get("item_id", 0) > 0 and created.get("scope_id") == "global", "plan_add item_id/scope_id shape mismatch", created)
        item_id = created["item_id"]
        updated = rpc({"op": "plan_update", "scope_id": "global", "item_id": item_id, "status": "done"})
        expect_ok(updated, "plan_update")
        require(updated.get("item_id") == item_id and updated.get("scope_id") == "global", "plan_update item_id/scope_id shape mismatch", updated)
        current = rpc({"op": "plan_list", "scope_id": "global", "kind": "todo"})
        expect_ok(current, "plan_list after add/update")
        matches = [item for item in current["items"] if item.get("item_id") == item_id]
        require(len(matches) == 1 and matches[0].get("status") == "done", "plan item update should be visible in list", current)
        deleted = rpc({"op": "plan_delete", "scope_id": "global", "item_id": item_id})
        expect_ok(deleted, "plan_delete")
        require(deleted.get("item_id") == item_id and deleted.get("scope_id") == "global", "plan_delete item_id/scope_id shape mismatch", deleted)
        created = None
    finally:
        if created and is_int(created.get("item_id")):
            cleanup = rpc({"op": "plan_delete", "scope_id": "global", "item_id": created["item_id"]})
            # A successful delete above makes the cleanup a deliberate no-op/error;
            # no assertion is made here, keeping cleanup idempotent from test's view.
            _ = cleanup

    unknown = rpc({"op": "not_a_core_rpc_operation"})
    expect_error(unknown, "unknown operation")
    require("unsupported op" in unknown["error"].lower(), "unknown operation must fail closed", unknown)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="asgend-core-rpc-contract-") as td:
        root = Path(td)
        port = integration.free_port()
        proc = integration.start_server(port, root / "tasks.sqlite3", token=TOKEN, sandbox_enabled=False)
        try:
            run_contracts(port)
        finally:
            integration.stop_server(proc)
    print("core RPC runtime contract tests passed")


if __name__ == "__main__":
    main()
