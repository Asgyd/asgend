#!/usr/bin/env python3
"""Tiny newline-JSON MCP server for integration tests; protocol output goes only to stdout."""
import json
import sys
import socket
import os
import pathlib
import time

MODERN = os.environ.get("ASGEND_MCP_MODERN_MOCK") == "1"

# The runtime's default seccomp profile denies AF_INET/AF_INET6 socket creation.
try:
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.close()
    NETWORK_BLOCKED = False
except OSError:
    NETWORK_BLOCKED = True

for raw in sys.stdin:
    try:
        message = json.loads(raw)
    except Exception:
        continue
    method = message.get("method")
    request_id = message.get("id")
    if method == "notifications/initialized":
        continue
    if request_id is None:
        continue
    if MODERN and method != "server/discover":
        params = message.get("params", {})
        meta = params.get("_meta", {}) if isinstance(params, dict) else {}
        if meta.get("io.modelcontextprotocol/protocolVersion") != "2026-07-28":
            print(json.dumps({"jsonrpc": "2.0", "id": request_id,
                              "error": {"code": -32602, "message": "missing current per-request MCP metadata"}}), flush=True)
            continue
    if MODERN and method == "server/discover":
        params = message.get("params", {})
        meta = params.get("_meta", {}) if isinstance(params, dict) else {}
        if meta.get("io.modelcontextprotocol/protocolVersion") != "2026-07-28":
            print(json.dumps({"jsonrpc": "2.0", "id": request_id,
                              "error": {"code": -32602, "message": "discover requires modern protocol metadata"}}), flush=True)
            continue
        result = {"resultType": "complete", "supportedVersions": ["2026-07-28"],
                  "capabilities": {"tools": {}, "resources": {}, "prompts": {}, "completions": {}},
                  "_meta": {"io.modelcontextprotocol/serverInfo": {"name": "modern-testserver", "version": "1.0"}},
                  "ttlMs": 3600000, "cacheScope": "public"}
    elif method == "initialize":
        result = {"protocolVersion": "2024-11-05", "capabilities": {
                      "tools": {}, "resources": {"listChanged": True}, "prompts": {"listChanged": True},
                      "completions": {}}, "serverInfo": {"name": "testserver", "version": "1.0"}}
    elif method == "tools/list":
        params = message.get("params", {})
        if params.get("cursor") == "tools-page-2":
            result = {"tools": [{"name": "extra_tool", "description": "Tool on a second paginated page.",
                                 "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False}}]}
        else:
            result = {"tools": [{"name": "echo", "description": "Return a marker so the orchestrator can verify tool calls.",
                                 "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}},
                                                 "required": ["text"], "additionalProperties": False}}],
                      "nextCursor": "tools-page-2"}
    elif method == "resources/list":
        params = message.get("params", {})
        if params.get("cursor") == "resources-page-2":
            result = {"resources": [{"uri": "file:///mock/usage.md", "name": "Usage Guide", "mimeType": "text/markdown"}]}
        else:
            result = {"resources": [{"uri": "file:///mock/spec.md", "name": "Protocol Spec", "mimeType": "text/markdown"}],
                      "nextCursor": "resources-page-2"}
    elif method == "resources/read":
        uri = message.get("params", {}).get("uri", "")
        result = {"contents": [{"uri": uri, "mimeType": "text/markdown",
                                 "text": "MOCK_RESOURCE_CONTENT:" + uri}]}
    elif method == "resources/templates/list":
        result = {"resourceTemplates": [{"uriTemplate": "file:///mock/{name}.md", "name": "Mock file template"}]}
    elif method == "prompts/list":
        params = message.get("params", {})
        if params.get("cursor") == "prompts-page-2":
            result = {"prompts": [{"name": "verify", "description": "Verification prompt"}]}
        else:
            result = {"prompts": [{"name": "summarize", "description": "Summarize a resource"}],
                      "nextCursor": "prompts-page-2"}
    elif method == "prompts/get":
        prompt_name = message.get("params", {}).get("name", "unknown")
        result = {"description": "Mock MCP prompt", "messages": [{"role": "user", "content": {
                      "type": "text", "text": "MOCK_PROMPT_CONTENT:" + prompt_name}}]}
    elif method == "completion/complete":
        result = {"completion": {"values": ["mock-completion"], "total": 1, "hasMore": False}}
    elif method == "tools/call":
        params = message.get("params", {})
        text = params.get("arguments", {}).get("text", "")
        if text == "__MCP_CANCEL__":
            # The integration suite observes this file inside the child sandbox
            # to cancel only after the MCP server has received the active call.
            try:
                pathlib.Path(".mcp-cancel-started").write_text("started", encoding="utf-8")
            except OSError:
                pass
            time.sleep(6.0)
        result = {"content": [{"type": "text", "text": "MOCK_MCP_EXECUTED:" + str(text) + ";network_blocked=" + str(NETWORK_BLOCKED).lower()}], "isError": False}
    else:
        print(json.dumps({"jsonrpc": "2.0", "id": request_id,
                          "error": {"code": -32601, "message": "method not found"}}), flush=True)
        continue
    if MODERN and isinstance(result, dict):
        result.setdefault("resultType", "complete")
    print(json.dumps({"jsonrpc": "2.0", "id": request_id, "result": result}, separators=(",", ":")), flush=True)
