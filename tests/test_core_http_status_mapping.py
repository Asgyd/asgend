#!/usr/bin/env python3
"""Runtime regression for REST status mapping with the JSON envelope preserved."""
from __future__ import annotations
import importlib.util
import json
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
script = PROJECT / "tests" / "integration.py"
saved_argv = sys.argv[:]
try:
    sys.argv = [str(script), str(BINARY), str(PROJECT)]
    spec = importlib.util.spec_from_file_location("asgend_core_http_status_integration", script)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load integration helpers")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
finally:
    sys.argv = saved_argv

with tempfile.TemporaryDirectory(prefix="ao-core-http-status-") as td:
    root = Path(td)
    proc = module.start_server(module.free_port(), root / "http.sqlite3", ui_enabled=True, sandbox_enabled=False)
    try:
        port = proc.ui_port
        token = "ui-test-token-with-at-least-32-characters"

        def request(path: str, payload: object | None = None):
            data = None if payload is None else json.dumps(payload).encode()
            req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data,
                headers={"X-AO-Token": token, "Content-Type": "application/json"},
                method="GET" if payload is None else "POST")
            try:
                with urllib.request.urlopen(req, timeout=4) as response:
                    return response.status, json.loads(response.read())
            except urllib.error.HTTPError as exc:
                return exc.code, json.loads(exc.read())

        def request_raw(path: str, data: bytes):
            req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data,
                headers={"X-AO-Token": token, "Content-Type": "application/json"}, method="POST")
            try:
                with urllib.request.urlopen(req, timeout=4) as response:
                    return response.status, json.loads(response.read())
            except urllib.error.HTTPError as exc:
                return exc.code, json.loads(exc.read())

        # Wait for the UI listener to start after the core RPC listener.
        deadline = time.monotonic() + 4
        while True:
            try:
                code, body = request("/api/tasks/not-a-real-task-id")
                break
            except (OSError, TimeoutError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        assert code == 404, (code, body, "unknown task should be HTTP 404")
        code, body = request("/api/tasks", {"task": ""})
        assert code == 400, (code, body)
        assert body.get("ok") is False and "error" in body, body

        # Known POST endpoints with malformed JSON or unsupported actions are
        # client errors (400); a genuinely unknown route remains 404.
        code, body = request_raw("/api/tasks", b"{")
        assert code == 400 and body.get("ok") is False, (code, body)
        code, body = request_raw("/api/plan", b'{"action":"drop-everything"}')
        assert code == 400 and body.get("ok") is False, (code, body)
        code, body = request_raw("/api/not-a-route", b"{}")
        assert code == 404 and body.get("ok") is False, (code, body)
    finally:
        module.stop_server(proc)
print("core HTTP status mapping tests passed")
