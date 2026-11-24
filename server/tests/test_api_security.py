from __future__ import annotations

from helpers import CPTestCase


class ControlPlaneSecurityTests(CPTestCase):
    def test_viewer_is_read_only_and_workspace_isolation_is_enforced(self):
        task = self.submit(key="security-task-key-01", text="workspace private task")
        forbidden = self.srv.post(f"/v1/workspaces/{self.ws_a}/tasks", self.viewer["token"], {
            "node_id": self.node_a["node_id"],
            "task": "viewer must not submit",
            "idempotency_key": "viewer-task-key-01",
        })
        self.assertEqual(forbidden.status, 403, forbidden)

        own = self.srv.get(f"/v1/tasks/{task['task_id']}", self.viewer["token"])
        self.assertEqual(own.status, 200, own)
        foreign = self.srv.get(f"/v1/tasks/{task['task_id']}", self.owner_b["token"])
        self.assertEqual(foreign.status, 404, foreign)

    def test_submit_idempotency_returns_original_and_rejects_key_reuse(self):
        first = self.submit(key="same-request-key-01", text="original task")
        same = self.srv.post(f"/v1/workspaces/{self.ws_a}/tasks", self.operator["token"], {
            "node_id": self.node_a["node_id"],
            "task": "original task",
            "idempotency_key": "same-request-key-01",
        })
        self.assertEqual(same.status, 200, same)
        self.assertEqual(same.json["task_id"], first["task_id"])

        changed = self.srv.post(f"/v1/workspaces/{self.ws_a}/tasks", self.operator["token"], {
            "node_id": self.node_a["node_id"],
            "task": "different task payload",
            "idempotency_key": "same-request-key-01",
        })
        self.assertEqual(changed.status, 409, changed)

    def test_node_revoke_immediately_invalidates_token(self):
        r = self.srv.post(f"/v1/nodes/{self.node_a['node_id']}/revoke", self.owner["token"], {})
        self.assertEqual(r.status, 200, r)
        heartbeat = self.srv.post("/v1/node/heartbeat", self.node_a["node_token"], {"status": "ready"})
        self.assertEqual(heartbeat.status, 401, heartbeat)

    def test_artifact_with_wrong_checksum_is_rejected_and_not_published(self):
        task = self.submit(key="artifact-task-key-01", text="produce artifact")
        # The upload path requires only that the task belongs to this node.
        payload = b"content whose checksum does not match"
        aid = self.upload_artifact(task["task_id"], payload, sha="0" * 64)
        complete = self.srv.post(f"/v1/node/artifacts/{aid}/complete", self.node_a["node_token"], {})
        self.assertEqual(complete.status, 422, complete)
        with self.srv.storage.read() as t:
            row = t.q1("SELECT state FROM artifacts WHERE artifact_id = ?", (aid,))
        self.assertEqual(row["state"], "rejected")


if __name__ == "__main__":
    import unittest
    unittest.main()
