#!/usr/bin/env python3
"""An idle accepted client must not pin graceful core shutdown until request timeout."""
from __future__ import annotations
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
TOKEN = "core-shutdown-test-token"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def rpc(port: int, body: dict, timeout: float = 2.0) -> dict:
    payload = dict(body, auth_token=TOKEN)
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall((json.dumps(payload, separators=(",", ":")) + "\n").encode())
        data = bytearray()
        while b"\n" not in data:
            block = sock.recv(65536)
            if not block:
                break
            data.extend(block)
        return json.loads(bytes(data).split(b"\n", 1)[0])


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="ao-core-shutdown-") as td:
        root = Path(td)
        port = free_port()
        source = json.loads((PROJECT / "config/config.json").read_text(encoding="utf-8"))
        source["server"].update({
            "host": "127.0.0.1", "port": port, "auth_token": TOKEN,
            "max_request_bytes": 65536, "max_connections": 24,
            "request_timeout_seconds": 60,
            "unix_socket": {"enabled": False, "path": str(root / "runtime.sock")},
        })
        source["runtime"].update({
            "workers": 1, "queue_max_pending": 100, "max_retries": 1,
            "priority_aging_seconds": 2, "database_path": str(root / "tasks.sqlite3"),
        })
        source["paths"].update({
            "asgend_home": str(root / "asgend-home"),
            "agents_dir": str(PROJECT / "config/agents"),
            "models_config": str(PROJECT / "config/models.json"),
            "skills_dir": str(PROJECT / "skills"),
            "knowledge_dir": str(PROJECT / "knowledge"),
            "system_prompts_dir": str(PROJECT / "system_prompts"),
            "tools_dir": str(PROJECT / "tools"),
            "instructions_dir": str(root / "instructions"),
        })
        source["local_llm"].update({"enabled": False, "timeout_seconds": 3})
        source["mcp"] = {"servers": []}
        source["lsp"] = {"servers": []}
        source["sandbox"].update({"enabled": False, "require_landlock": False})
        source["ui"] = {"enabled": False, "host": "127.0.0.1", "port": free_port(), "auth_token": ""}
        config_path = root / "config.json"
        config_path.write_text(json.dumps(source), encoding="utf-8")
        proc = subprocess.Popen([str(BINARY), "-c", str(config_path)], cwd=str(PROJECT),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        idle = None
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise AssertionError(f"core exited during startup: {proc.stderr.read()}")
                try:
                    reply = rpc(port, {"op": "ping"})
                    if reply.get("ok"):
                        break
                except (OSError, ValueError, TimeoutError):
                    time.sleep(0.05)
            else:
                raise AssertionError("core did not start")

            # Establish a socket but do not send a request line. Before the fix,
            # connection_main remained in read_request_line() for up to 60 seconds.
            idle = socket.create_connection(("127.0.0.1", port), timeout=2)
            started = time.monotonic()
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=3)
            elapsed = time.monotonic() - started
            assert elapsed < 2.5, f"shutdown took {elapsed:.2f}s with an idle accepted socket"
            assert proc.returncode == 0, f"unexpected exit code {proc.returncode}: {proc.stderr.read()}"
            print(f"core shutdown idle-client test passed ({elapsed:.3f}s)")
        finally:
            if idle is not None:
                idle.close()
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()


if __name__ == "__main__":
    main()
