#!/usr/bin/env python3
"""MCP test peer that records its startup environment, then speaks legacy stdio MCP."""
import json
import os
import resource
import socket
import sys
from pathlib import Path

probe_path = Path(sys.argv[1])
try:
    socket.socket(socket.AF_INET, socket.SOCK_STREAM).close()
    network_socket_errno = None
except OSError as exc:
    network_socket_errno = exc.errno

try:
    left, right = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    left.sendall(b"local-ipc")
    unix_socketpair_ok = right.recv(32) == b"local-ipc"
    left.close()
    right.close()
except OSError:
    unix_socketpair_ok = False

open_fds = []
try:
    for entry in os.listdir("/proc/self/fd"):
        try:
            os.readlink(f"/proc/self/fd/{entry}")
            open_fds.append(int(entry))
        except (FileNotFoundError, PermissionError, OSError):
            pass
except OSError:
    # /proc may be unavailable under a restrictive filesystem policy.
    open_fds = None

probe_path.write_text(json.dumps({
    "parent_secret": os.environ.get("ASGEND_TEST_PARENT_SECRET"),
    "explicit_value": os.environ.get("ASGEND_MCP_EXPLICIT_VALUE"),
    "path": os.environ.get("PATH"),
    "home": os.environ.get("HOME"),
    "tmpdir": os.environ.get("TMPDIR"),
    "ld_preload": os.environ.get("LD_PRELOAD"),
    "euid": os.geteuid(),
    "egid": os.getegid(),
    "groups": os.getgroups(),
    "limits": {
        "cpu": list(resource.getrlimit(resource.RLIMIT_CPU)),
        "address_space": list(resource.getrlimit(resource.RLIMIT_AS)),
        "file_size": list(resource.getrlimit(resource.RLIMIT_FSIZE)),
        "open_files": list(resource.getrlimit(resource.RLIMIT_NOFILE)),
        "core": list(resource.getrlimit(resource.RLIMIT_CORE)),
    },
    "network_socket_errno": network_socket_errno,
    "unix_socketpair_ok": unix_socketpair_ok,
    "open_fds": open_fds,
}, separators=(",", ":")), encoding="utf-8")

for line in sys.stdin:
    try:
        request = json.loads(line)
    except Exception:
        continue
    method = request.get("method")
    request_id = request.get("id")
    if request_id is None:
        continue
    if method == "server/discover":
        reply = {"jsonrpc": "2.0", "id": request_id,
                 "error": {"code": -32601, "message": "legacy protocol"}}
    elif method == "initialize":
        reply = {"jsonrpc": "2.0", "id": request_id, "result": {
            "protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
            "serverInfo": {"name": "environment-probe", "version": "1.0"}}}
    elif method == "tools/list":
        reply = {"jsonrpc": "2.0", "id": request_id, "result": {"tools": []}}
    else:
        reply = {"jsonrpc": "2.0", "id": request_id,
                 "error": {"code": -32601, "message": "method not found"}}
    print(json.dumps(reply, separators=(",", ":")), flush=True)
