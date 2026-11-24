#!/usr/bin/env python3
"""Runtime acceptance for the non-destructive, allow-listed config update RPC."""
from __future__ import annotations

import copy
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
TOKEN = "safe-config-update-test-token"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def rpc(port: int, payload: dict, token: str = TOKEN, timeout: float = 4.0) -> dict:
    request = dict(payload)
    if token is not None:
        request["auth_token"] = token
    data = (json.dumps(request, separators=(",", ":")) + "\n").encode("utf-8")
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(data)
        chunks = bytearray()
        while b"\n" not in chunks:
            chunk = sock.recv(65536)
            if not chunk:
                break
            chunks.extend(chunk)
    line = bytes(chunks).split(b"\n", 1)[0]
    return json.loads(line.decode("utf-8"))


def wait_server(port: int, proc: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + 8
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            stderr = proc.stderr.read() if proc.stderr else ""
            raise AssertionError(f"core exited before startup ({proc.returncode}): {stderr[-4000:]}")
        try:
            response = rpc(port, {"op": "ping"})
            if response.get("ok"):
                return
        except (OSError, TimeoutError, json.JSONDecodeError) as exc:
            last_error = exc
        time.sleep(0.05)
    raise AssertionError(f"core did not become ready: {last_error!r}")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="asgend-safe-config-update-") as td:
        root = Path(td)
        port = free_port()
        config_path = root / "config.json"
        models_path = root / "models.json"
        agents_dir = root / "agents"
        shutil.copytree(PROJECT / "config" / "agents", agents_dir)
        shutil.copyfile(PROJECT / "config" / "models.json", models_path)

        # Model hidden legacy fixed-role profiles that config_get intentionally omits.
        generalist = json.loads((agents_dir / "generalist.json").read_text(encoding="utf-8"))
        for legacy_id in ("coder", "planner", "reviewer"):
            profile = dict(generalist, id=legacy_id, name=legacy_id.title())
            (agents_dir / f"{legacy_id}.json").write_text(json.dumps(profile), encoding="utf-8")
        legacy_before = {name: (agents_dir / f"{name}.json").read_bytes() for name in ("coder", "planner", "reviewer")}

        config = json.loads((PROJECT / "config" / "config.json").read_text(encoding="utf-8"))
        config["server"].update({
            "host": "127.0.0.1", "port": port, "auth_token": TOKEN,
            "max_connections": 24, "request_timeout_seconds": 4,
            "unix_socket": {"enabled": False, "path": str(root / "core.sock")},
        })
        config["runtime"].update({
            "workers": 1, "queue_max_pending": 100,
            "max_retries": 2, "priority_aging_seconds": 2,
            "database_path": str(root / "tasks.sqlite3"),
        })
        config["local_llm"]["api_key"] = "DO_NOT_LEAK_PROVIDER_SECRET"
        config["paths"].update({
            "asgend_home": str(root / "asgend-home"),
            "agents_dir": str(agents_dir),
            "models_config": str(models_path),
            "skills_dir": str(PROJECT / "skills"),
            "knowledge_dir": str(PROJECT / "knowledge"),
            "system_prompts_dir": str(PROJECT / "system_prompts"),
            "tools_dir": str(PROJECT / "tools"),
            "instructions_dir": str(PROJECT / "instructions"),
            "project_root": str(PROJECT),
        })
        config["mcp"] = {"servers": []}
        config["lsp"] = {"servers": []}
        config["sandbox"]["require_landlock"] = False
        config["ui"]["enabled"] = False
        config["ui"]["auth_token"] = "test-ui-token-012345678901234567890123"
        config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
        models_before = models_path.read_bytes()

        proc = subprocess.Popen(
            [str(BINARY), "-c", str(config_path)],
            cwd=PROJECT,
            env=os.environ.copy(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            wait_server(port, proc)
            original_config_bytes = config_path.read_bytes()
            original = json.loads(original_config_bytes)

            unauthorized = rpc(port, {
                "op": "config_update_settings",
                "settings": {"runtime": {"workers": 2}},
            }, token="wrong-token")
            assert unauthorized.get("ok") is False and "unauthorized" in unauthorized.get("error", ""), unauthorized
            assert config_path.read_bytes() == original_config_bytes, "unauthorized request modified config"

            accepted = rpc(port, {
                "op": "config_update_settings",
                "settings": {
                    "server": {"max_connections": 32, "request_timeout_seconds": 17},
                    "runtime": {"workers": 3, "max_retries": 5},
                    "sandbox": {"allow_network": False, "require_landlock": True},
                },
            })
            assert accepted.get("ok") is True and accepted.get("restart_required") is True, accepted
            updated = json.loads(config_path.read_text(encoding="utf-8"))
            assert updated["server"]["max_connections"] == 32
            assert updated["server"]["request_timeout_seconds"] == 17
            assert updated["runtime"]["workers"] == 3 and updated["runtime"]["max_retries"] == 5
            assert updated["sandbox"]["allow_network"] is False and updated["sandbox"]["require_landlock"] is True
            assert updated["server"]["auth_token"] == TOKEN
            assert updated["local_llm"]["api_key"] == "DO_NOT_LEAK_PROVIDER_SECRET"
            assert updated["paths"] == original["paths"] and updated["mcp"] == original["mcp"]
            assert models_path.read_bytes() == models_before, "partial settings update touched the model registry"
            for name, content in legacy_before.items():
                assert (agents_dir / f"{name}.json").read_bytes() == content, f"legacy profile {name} changed"
            rollback_path = Path(str(config_path) + ".android-rollback")
            assert not rollback_path.exists(), "private rollback copy left behind after successful commit"
            assert config_path.stat().st_mode & 0o777 == 0o600, "updated config must be private (0600)"

            # An earlier uncertain commit may leave a recovery copy. New writes
            # must fail closed instead of overwriting the only retained backup.
            before_stale_rollback = config_path.read_bytes()
            rollback_sentinel = b"preserve-unresolved-rollback-copy\n"
            rollback_path.write_bytes(rollback_sentinel)
            blocked = rpc(port, {"op": "config_update_settings", "settings": {"runtime": {"workers": 4}}})
            assert blocked.get("ok") is False and "rollback" in blocked.get("error", "").lower(), blocked
            assert config_path.read_bytes() == before_stale_rollback, "unresolved rollback state allowed config mutation"
            assert rollback_path.read_bytes() == rollback_sentinel, "existing recovery copy was overwritten"
            rollback_path.unlink()

            before_invalid = config_path.read_bytes()
            invalid_cases = [
                {"settings": {"runtime": {"workers": True}}},
                {"settings": {"runtime": {"workers": 1.5}}},
                {"settings": {"runtime": {"workers": 0}}},
                {"settings": {"runtime": {"database_path": str(root / "new-db.sqlite")}}},
                {"settings": {"paths": {"project_root": "/tmp"}}},
                {"settings": {"sandbox": {"enabled": "false"}}},
                {"settings": {"sandbox": {"memory_mb": 999999}}},
                {"settings": {"runtime": {"workers": "DO_NOT_LEAK_PROVIDER_SECRET"}}, "extra": True},
                {"settings": {}},
                {"settings": {"runtime": []}},
            ]
            for bad in invalid_cases:
                response = rpc(port, {"op": "config_update_settings", **bad})
                assert response.get("ok") is False, (bad, response)
                assert "DO_NOT_LEAK_PROVIDER_SECRET" not in json.dumps(response), "secret value leaked through validation error"
                assert config_path.read_bytes() == before_invalid, f"invalid update partially modified config: {bad}"
                assert models_path.read_bytes() == models_before
                for name, content in legacy_before.items():
                    assert (agents_dir / f"{name}.json").read_bytes() == content

            # Legacy full-bundle editor remains compatible but must not delete profiles hidden by config_get.
            bundle = rpc(port, {"op": "config_get"})
            assert bundle.get("ok") is True and all(name not in {a.get("id") for a in bundle.get("agents", [])}
                                                     for name in legacy_before), bundle
            saved = rpc(port, {
                "op": "config_save",
                "main": bundle["main"], "models": bundle["models"], "agents": bundle["agents"],
            })
            assert saved.get("ok") is True, saved
            for name, content in legacy_before.items():
                assert (agents_dir / f"{name}.json").read_bytes() == content, f"legacy profile {name} deleted by full save"

            print("core safe config update runtime tests passed")
        finally:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            if proc.stderr:
                proc.stderr.close()


if __name__ == "__main__":
    main()
