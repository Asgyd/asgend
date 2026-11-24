#!/usr/bin/env python3
"""MCP fixture that exits once after discovery, then stays healthy after restart."""
import json
import os
import sys
from pathlib import Path

state_path = Path(sys.argv[1])
first_start = not state_path.exists()
with state_path.open("a", encoding="utf-8") as state:
    state.write("first_start\n" if first_start else "restart_start\n")
    state.flush()

for raw in sys.stdin:
    try:
        message = json.loads(raw)
    except Exception:
        continue
    method = message.get("method")
    request_id = message.get("id")
    if request_id is None:
        continue
    if method == "server/discover":
        response = {"jsonrpc": "2.0", "id": request_id,
                    "error": {"code": -32601, "message": "method not found"}}
    elif method == "initialize":
        response = {"jsonrpc": "2.0", "id": request_id,
                    "result": {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
                               "serverInfo": {"name": "restart-fixture", "version": "1.0"}}}
    elif method == "tools/list":
        response = {"jsonrpc": "2.0", "id": request_id,
                    "result": {"tools": [{"name": "echo", "description": "Echo after supervisor restart",
                                            "inputSchema": {"type": "object",
                                                            "properties": {"text": {"type": "string"}},
                                                            "required": ["text"], "additionalProperties": False}}]}}
        print(json.dumps(response, separators=(",", ":")), flush=True)
        if first_start:
            with state_path.open("a", encoding="utf-8") as state:
                state.write("first_exit\n")
                state.flush()
            os._exit(23)
        with state_path.open("a", encoding="utf-8") as state:
            state.write("restart_ready\n")
            state.flush()
        continue
    elif method == "tools/call":
        text = message.get("params", {}).get("arguments", {}).get("text", "")
        response = {"jsonrpc": "2.0", "id": request_id,
                    "result": {"content": [{"type": "text", "text": "MCP_RESTART_RECOVERED:" + str(text)}],
                               "isError": False}}
    else:
        response = {"jsonrpc": "2.0", "id": request_id,
                    "error": {"code": -32601, "message": "method not found"}}
    print(json.dumps(response, separators=(",", ":")), flush=True)
