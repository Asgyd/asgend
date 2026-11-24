#!/usr/bin/env python3
"""Probe file-system decisions from inside a real MCP Landlock child."""
from __future__ import annotations
import errno
import json
import os
import subprocess
import sys
from pathlib import Path

probe_name = sys.argv[1]

def read_file(path_value: str) -> dict:
    try:
        return {"ok": True, "text": Path(path_value).read_text(encoding="utf-8")}
    except OSError as exc:
        return {"ok": False, "errno": exc.errno}

def write_file(path_value: str, text: str) -> dict:
    try:
        Path(path_value).write_text(text, encoding="utf-8")
        return {"ok": True}
    except OSError as exc:
        return {"ok": False, "errno": exc.errno}

allowed_file = os.environ["ASGEND_TEST_ALLOWED_FILE"]
denied_file = os.environ["ASGEND_TEST_DENIED_FILE"]
readonly_dir = Path(os.environ["ASGEND_TEST_READONLY_DIR"])
outside_write = os.environ["ASGEND_TEST_OUTSIDE_WRITE"]
execute_file = os.environ["ASGEND_TEST_EXEC_FILE"]

try:
    execution = subprocess.run([execute_file], capture_output=True, text=True, timeout=2, check=False)
    exec_result = {"allowed": True, "returncode": execution.returncode,
                   "stdout": execution.stdout.strip()}
except OSError as exc:
    exec_result = {"allowed": False, "errno": exc.errno}
except subprocess.TimeoutExpired:
    exec_result = {"allowed": True, "timed_out": True}

result = {
    "allowed_read": read_file(allowed_file),
    "denied_read": read_file(denied_file),
    "workspace_write": write_file("workspace-write.txt", "workspace-ok"),
    "readonly_write": write_file(str(readonly_dir / "landlock-write-attempt.txt"), "must-be-denied"),
    "outside_write": write_file(outside_write, "must-be-denied"),
    "execute_readonly_path": exec_result,
    "euid": os.geteuid(),
}
Path(probe_name).write_text(json.dumps(result, separators=(",", ":")), encoding="utf-8")

for raw in sys.stdin:
    try:
        request = json.loads(raw)
    except Exception:
        continue
    request_id = request.get("id")
    if request_id is None:
        continue
    method = request.get("method")
    if method == "server/discover":
        reply = {"jsonrpc": "2.0", "id": request_id,
                 "error": {"code": -32601, "message": "legacy protocol"}}
    elif method == "initialize":
        reply = {"jsonrpc": "2.0", "id": request_id, "result": {
            "protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
            "serverInfo": {"name": "landlock-probe", "version": "1.0"}}}
    elif method == "tools/list":
        reply = {"jsonrpc": "2.0", "id": request_id, "result": {"tools": []}}
    else:
        reply = {"jsonrpc": "2.0", "id": request_id,
                 "error": {"code": -32601, "message": "method not found"}}
    print(json.dumps(reply, separators=(",", ":")), flush=True)
