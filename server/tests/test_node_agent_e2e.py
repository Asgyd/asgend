"""Live Control Plane + real node-agent acceptance test.

The HTTP Control Plane and node-agent are the real implementations. Only the C core
is replaced by the protocol-faithful MockCore, so the test remains hermetic in CI.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
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

# Load the Control Plane harness by path so its ``helpers`` module cannot collide
# with node_agent/tests/helpers.py during unittest discovery.
_cp_helpers_spec = importlib.util.spec_from_file_location("asgend_cp_test_helpers", HERE / "helpers.py")
assert _cp_helpers_spec and _cp_helpers_spec.loader
_cp_helpers = importlib.util.module_from_spec(_cp_helpers_spec)
sys.modules[_cp_helpers_spec.name] = _cp_helpers
_cp_helpers_spec.loader.exec_module(_cp_helpers)
CPTestCase = _cp_helpers.CPTestCase
from asgend_node.agent import NodeAgent  # noqa: E402
from asgend_node.config import NodeConfig  # noqa: E402
from asgend_node.core_client import CoreClient, CoreEndpoint  # noqa: E402
from asgend_node.cp_client import CPClient  # noqa: E402
from asgend_node.state import Store  # noqa: E402
from mock_core import MockCore  # noqa: E402


class LiveControlPlaneNodeAgentE2E(CPTestCase):
    """Prove command delivery, event projection and artifact upload against live CP."""

    def setUp(self):
        super().setUp()
        self.core_token = "node-e2e-core-token"
        self.core_server = MockCore(auth_token=self.core_token, script="simple", tick=0.01).start()
        self.node_state_dir = tempfile.mkdtemp(prefix="asgend-node-e2e-")
        self.node_store = Store(self.node_state_dir)
        self.node_config = NodeConfig(
            state_dir=self.node_state_dir,
            server_url=f"http://127.0.0.1:{self.srv.port}",
            allow_insecure_http=True,  # test server is bound to loopback only
            core_host="127.0.0.1",
            core_port=self.core_server.port,
            core_auth_token=self.core_token,
            core_timeout=2,
            http_timeout=2,
            lease_wait_ms=0,
            lease_max=10,
            event_poll_interval=0.01,
            event_batch_size=100,
            core_events_page=100,
            artifacts_enabled=True,
            artifact_read_chunk=1024,
            max_artifact_bytes=1024 * 1024,
        )
        self.node_credentials = {
            **self.node_a,
            "server_url": self.node_config.server_url,
            "workspace_id": self.ws_a,
            "name": "node-a",
            "labels": ["linux"],
            "allow_insecure_http": True,
        }
        cp = CPClient(self.node_config.server_url, token=self.node_a["node_token"],
                      allow_insecure_http=True, timeout=2)
        core = CoreClient(CoreEndpoint(host="127.0.0.1", port=self.core_server.port,
                                       auth_token=self.core_token, timeout=2))
        self.agent = NodeAgent(self.node_config, self.node_credentials, self.node_store, core, cp)

    def tearDown(self):
        try:
            if hasattr(self, "agent"):
                self.agent.stop()
            if hasattr(self, "node_store"):
                self.node_store.close()
            if hasattr(self, "core_server"):
                self.core_server.stop()
            if hasattr(self, "node_state_dir"):
                import shutil
                shutil.rmtree(self.node_state_dir, ignore_errors=True)
        finally:
            super().tearDown()

    def test_submit_runs_on_node_events_project_and_artifact_downloads_verified(self):
        heartbeat = self.agent.heartbeat_once()
        self.assertIn("lease_expires_at", heartbeat)

        task = self.submit(key="live-e2e-task-key-01", text="generate a verified report")
        task_id = task["task_id"]
        leased = self.agent.poll_commands_once(wait_ms=0)
        self.assertEqual(leased, 1)

        remote_task = self.srv.get(f"/v1/tasks/{task_id}", self.viewer["token"])
        self.assertEqual(remote_task.status, 200, remote_task)
        self.assertEqual(remote_task.json["state"], "delivered", remote_task.json)

        deadline = time.monotonic() + 8
        artifact = None
        while time.monotonic() < deadline:
            self.agent.sync_events_once()
            self.agent.upload_artifacts_once()
            self.agent.sync_events_once()
            state = self.srv.get(f"/v1/tasks/{task_id}", self.viewer["token"])
            self.assertEqual(state.status, 200, state)
            artifacts = self.srv.get(f"/v1/tasks/{task_id}/artifacts", self.viewer["token"])
            self.assertEqual(artifacts.status, 200, artifacts)
            artifact = next((a for a in artifacts.json["artifacts"]
                             if a["name"] == "report.txt" and a["state"] == "available"), None)
            if state.json["state"] == "completed" and artifact:
                break
            time.sleep(0.025)

        self.assertEqual(state.json["state"], "completed", state.json)
        self.assertIsNotNone(artifact, artifacts.json)
        expected = self.core_server.tasks[self.node_store.get_task(task_id)["core_task_id"]]["files"]["report.txt"]["data"]
        self.assertEqual(artifact["size"], len(expected))
        self.assertEqual(artifact["sha256"], hashlib.sha256(expected).hexdigest())

        ticket = self.srv.post(f"/v1/artifacts/{artifact['artifact_id']}/download-ticket",
                               self.viewer["token"], {})
        self.assertEqual(ticket.status, 201, ticket)
        downloaded = self.srv.get(ticket.json["url"])
        self.assertEqual(downloaded.status, 200, downloaded)
        self.assertEqual(downloaded.body, expected)
        self.assertEqual(downloaded.header("X-Checksum-SHA256"), artifact["sha256"])

        event_reply = self.srv.get(f"/v1/tasks/{task_id}/events?after_event_id=0&wait_ms=0",
                                   self.viewer["token"])
        self.assertEqual(event_reply.status, 200, event_reply)
        event_dump = json.dumps(event_reply.json, ensure_ascii=False)
        self.assertIn("artifact_available", event_dump)
        self.assertNotIn("secret-should-not-leak", event_dump)

        # The symlink fixture is intentionally present in core, but must never be uploaded.
        names = {a["name"] for a in artifacts.json["artifacts"]}
        self.assertNotIn("link-to-secret", names)


if __name__ == "__main__":
    unittest.main()
