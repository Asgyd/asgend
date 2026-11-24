"""End-to-end acceptance test using the compiled C core + live Control Plane.

This deliberately does not require an LLM provider. It proves the real C JSON
RPC protocol, command hand-off, event/status projection, confined workspace
read, and artifact upload/download path. The task itself may fail cleanly when
no model is configured; a test artifact is seeded into its private workspace.

Run with:
  python server/tests/test_node_agent_c_core_e2e.py /path/to/agent-orchestrator
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
NODE_ROOT = PROJECT / "node_agent"
NODE_TESTS = NODE_ROOT / "tests"
for entry in (str(NODE_ROOT), str(NODE_TESTS)):
    if entry not in sys.path:
        sys.path.append(entry)

# unittest discovery imports this file without a positional core binary. In
# that case the test is skipped; CMake/CI invokes it explicitly with the target.
_candidate = os.environ.get("ASGEND_CORE_BINARY")
if not _candidate and len(sys.argv) > 1:
    maybe = Path(sys.argv[1])
    if maybe.is_file():
        _candidate = str(maybe)
CORE_BINARY = Path(_candidate).resolve() if _candidate else None

_cp_helpers_spec = importlib.util.spec_from_file_location("asgend_cp_test_helpers_core_e2e", HERE / "helpers.py")
assert _cp_helpers_spec and _cp_helpers_spec.loader
_cp_helpers = importlib.util.module_from_spec(_cp_helpers_spec)
sys.modules[_cp_helpers_spec.name] = _cp_helpers
_cp_helpers_spec.loader.exec_module(_cp_helpers)
CPTestCase = _cp_helpers.CPTestCase
from asgend_node.agent import NodeAgent  # noqa: E402
from asgend_node.config import NodeConfig  # noqa: E402
from asgend_node.core_client import CoreClient, CoreEndpoint, CoreError, CoreUnavailable  # noqa: E402
from asgend_node.cp_client import CPClient  # noqa: E402
from asgend_node.state import Store  # noqa: E402


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def _wait_for(predicate, timeout: float, description: str):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, ConnectionError, TimeoutError, json.JSONDecodeError, CoreError, CoreUnavailable) as exc:
            last_error = exc
        time.sleep(0.04)
    raise AssertionError(f"timed out waiting for {description}; last_error={last_error!r}")


@unittest.skipUnless(CORE_BINARY and CORE_BINARY.is_file(),
                     "set ASGEND_CORE_BINARY or pass the compiled agent-orchestrator path")
class CompiledCCoreNodeAgentE2E(CPTestCase):
    """Exercise the actual C service against the actual Python node-agent."""

    def setUp(self):
        super().setUp()
        self.core_root = Path(self.tmp) / "real-c-core"
        config_dir = self.core_root / "config"
        config_dir.mkdir(parents=True)
        self.core_home = self.core_root / "asgend-home"
        self.core_home.mkdir(mode=0o700)
        self.core_state = Path(self.tmp) / "node-state-c-core"
        self.core_state.mkdir(mode=0o700)
        self.node_store = None
        self.core_proc = None
        self.agent = None

        agents_dir = config_dir / "agents"
        shutil.copytree(PROJECT / "config" / "agents", agents_dir)
        models_path = config_dir / "models.json"
        shutil.copyfile(PROJECT / "config" / "models.json", models_path)
        (self.core_root / "instructions").mkdir()
        port = _free_port()
        self.core_token = "compiled-c-core-token-for-node-e2e"
        self.core_port = port
        self.config_path = config_dir / "config.json"
        config = {
            "server": {
                "host": "127.0.0.1", "port": port, "auth_token": self.core_token,
                "max_request_bytes": 1048576, "max_connections": 32,
                "request_timeout_seconds": 5,
                "unix_socket": {"enabled": False, "path": str(self.core_root / "runtime.sock")},
            },
            "runtime": {
                "workers": 1, "queue_max_pending": 20, "max_retries": 0,
                "priority_aging_seconds": 10,
                "database_path": str(self.core_root / "tasks.sqlite3"),
            },
            "local_llm": {
                "enabled": False, "base_url": "http://127.0.0.1:11434/v1",
                "model": "unused-in-e2e", "api_key": "", "timeout_seconds": 2,
                "temperature": 0.0,
            },
            "paths": {
                "asgend_home": str(self.core_home),
                "agents_dir": str(agents_dir),
                "models_config": str(models_path),
                "skills_dir": str(PROJECT / "skills"),
                "knowledge_dir": str(PROJECT / "knowledge"),
                "system_prompts_dir": str(PROJECT / "system_prompts"),
                "tools_dir": str(PROJECT / "tools"),
                "instructions_dir": str(self.core_root / "instructions"),
                "project_root": str(PROJECT),
            },
            "mcp": {"servers": []},
            "lsp": {"servers": []},
            "sandbox": {
                "enabled": False, "require_landlock": False, "allow_network": False,
                "cpu_seconds": 5, "memory_mb": 256, "file_size_mb": 8, "open_files": 32,
            },
            "ui": {"enabled": False, "host": "127.0.0.1", "port": _free_port(),
                   "auth_token": "", "directory": str(PROJECT / "ui")},
        }
        self.config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
        env = os.environ.copy()
        # Isolate model/user paths from any developer or CI runner environment.
        env["HOME"] = str(self.core_root / "user-home")
        Path(env["HOME"]).mkdir()
        env.pop("ASGEND_HOME", None)
        self.core_proc = subprocess.Popen(
            [str(CORE_BINARY), "-c", str(self.config_path)], cwd=str(PROJECT), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        self.core = CoreClient(CoreEndpoint(host="127.0.0.1", port=port,
                                            auth_token=self.core_token, timeout=2))
        try:
            ping = _wait_for(lambda: self.core.ping() if self.core_proc.poll() is None else None,
                             8, "compiled C core TCP listener")
            self.assertEqual(ping.get("service"), "agent-orchestrator-c", ping)
            self.assertTrue(ping.get("version"), ping)
        except Exception:
            proc = self.core_proc
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2)
            try:
                _, stderr = proc.communicate(timeout=1) if proc is not None else ("", "")
            except (OSError, subprocess.TimeoutExpired):
                stderr = "<stderr unavailable>"
            self.fail(f"compiled C core failed to start; stderr={stderr[-5000:]!r}")

        self.node_store = Store(str(self.core_state))
        node_config = NodeConfig(
            state_dir=str(self.core_state),
            server_url=f"http://127.0.0.1:{self.srv.port}",
            allow_insecure_http=True, http_timeout=2,
            core_host="127.0.0.1", core_port=port,
            core_auth_token=self.core_token, core_timeout=2,
            lease_wait_ms=0, lease_max=10,
            event_poll_interval=0.02, event_batch_size=100, core_events_page=100,
            artifacts_enabled=True, artifact_read_chunk=1024, max_artifact_bytes=1024 * 1024,
        )
        creds = {
            **self.node_a, "server_url": node_config.server_url, "workspace_id": self.ws_a,
            "name": "node-c-core", "labels": ["linux", "compiled-c-core"],
            "allow_insecure_http": True,
        }
        cp_client = CPClient(node_config.server_url, token=self.node_a["node_token"],
                             allow_insecure_http=True, timeout=2)
        self.agent = NodeAgent(node_config, creds, self.node_store, self.core, cp_client)

    def tearDown(self):
        try:
            if self.agent is not None:
                self.agent.stop()
            if self.node_store is not None:
                self.node_store.close()
            self._stop_core()
        finally:
            super().tearDown()

    def _core_stderr(self) -> str:
        if not self.core_proc or self.core_proc.stderr is None:
            return ""
        if self.core_proc.poll() is None:
            return "<process still running>"
        try:
            return self.core_proc.stderr.read()
        except OSError:
            return "<stderr unavailable>"

    def _stop_core(self):
        proc = self.core_proc
        if proc is None:
            return
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        # Drain both pipes after termination to avoid leaving a child handle open.
        try:
            if proc.stdout:
                proc.stdout.close()
            if proc.stderr:
                proc.stderr.close()
        except OSError:
            pass

    def test_real_c_core_handles_node_command_and_artifact_roundtrip(self):
        heartbeat = self.agent.heartbeat_once()
        self.assertIn("lease_expires_at", heartbeat)

        submitted = self.submit(key="compiled-c-core-e2e-key-01",
                                text="Integration smoke task; no provider is configured")
        remote_task_id = submitted["task_id"]
        self.assertEqual(self.agent.poll_commands_once(wait_ms=0), 1)
        local = self.node_store.get_task(remote_task_id)
        self.assertIsNotNone(local)
        core_task_id = local["core_task_id"]
        self.assertTrue(core_task_id)

        # The returned ID must be understood by the actual C API and the task
        # workspace must be created by the C core, not by a test-side mock.
        initial = self.core.status(core_task_id)
        self.assertTrue(initial.get("ok"), initial)
        workspace = self.core_home / "tasks" / core_task_id
        self.assertTrue(workspace.is_dir(), f"C core did not create workspace {workspace}")
        artifact_bytes = b"ASGEND compiled C core artifact roundtrip\nsha256-verification\n"
        artifact_path = workspace / "compiled-core-report.txt"
        artifact_path.write_bytes(artifact_bytes)
        artifact_path.chmod(0o600)
        outside_secret = self.core_root / "outside-secret.txt"
        outside_secret.write_text("must never be published through a workspace symlink", encoding="utf-8")
        symlink_path = workspace / "link-to-secret"
        try:
            symlink_path.symlink_to(outside_secret)
        except (OSError, NotImplementedError):
            self.skipTest("test filesystem does not support symlinks")

        # With no model/provider, the task should terminate as a clean failure.
        # If it does not, request cancellation via the real C API so terminal
        # status/artifact enumeration is still exercised deterministically.
        states_seen = []
        def is_terminal():
            status_reply = self.core.status(core_task_id)
            state = status_reply.get("status")
            states_seen.append((state, status_reply.get("error")))
            if state in ("completed", "completed_without_model", "failed", "cancelled", "canceled"):
                return state
            return None
        try:
            terminal = _wait_for(is_terminal, 6, "real C task to terminate without a model")
        except AssertionError:
            try:
                self.core.cancel(core_task_id)
            except CoreError:
                pass
            try:
                terminal = _wait_for(is_terminal, 6, "real C task cancellation")
            except AssertionError as exc:
                raise AssertionError(f"{exc}; states_seen={states_seen[-10:]!r}") from exc
        self.assertIn(terminal, ("completed", "completed_without_model", "failed", "cancelled", "canceled"))

        deadline = time.monotonic() + 8
        listed = None
        while time.monotonic() < deadline:
            self.agent.sync_events_once()
            self.agent.upload_artifacts_once()
            self.agent.sync_events_once()
            reply = self.srv.get(f"/v1/tasks/{remote_task_id}/artifacts", self.viewer["token"])
            self.assertEqual(reply.status, 200, reply)
            listed = reply.json.get("artifacts", [])
            found = next((a for a in listed if a.get("name") == artifact_path.name and a.get("state") == "available"), None)
            if found:
                break
            time.sleep(0.03)
        self.assertIsNotNone(found, {"core_status": terminal, "artifacts": listed,
                                     "agent_artifacts": [dict(a) for a in self.node_store.task_artifacts(remote_task_id)]})
        self.assertEqual(found["size"], len(artifact_bytes))
        sha = hashlib.sha256(artifact_bytes).hexdigest()
        self.assertEqual(found["sha256"], sha)
        self.assertNotIn("link-to-secret", {a.get("name") for a in listed},
                         "C core artifact listing must not publish symlinks")

        ticket = self.srv.post(f"/v1/artifacts/{found['artifact_id']}/download-ticket",
                               self.viewer["token"], {})
        self.assertEqual(ticket.status, 201, ticket)
        downloaded = self.srv.get(ticket.json["url"])
        self.assertEqual(downloaded.status, 200, downloaded)
        self.assertEqual(downloaded.body, artifact_bytes)
        self.assertEqual(downloaded.header("X-Checksum-SHA256"), sha)

        event_reply = self.srv.get(f"/v1/tasks/{remote_task_id}/events?after_event_id=0&wait_ms=0",
                                   self.viewer["token"])
        self.assertEqual(event_reply.status, 200, event_reply)
        event_types = {item.get("type") for item in event_reply.json.get("events", [])}
        self.assertIn("state_changed", event_types)


if __name__ == "__main__":
    if CORE_BINARY is None or not CORE_BINARY.is_file():
        raise SystemExit("usage: python test_node_agent_c_core_e2e.py /path/to/agent-orchestrator")
    unittest.main(argv=[sys.argv[0]], verbosity=2)
