import hashlib
import os
import unittest

from helpers import NodeTestCase

from asgend_node.cp_client import CPUnavailable


class ArtifactTest(NodeTestCase):
    chunk_size = 1000

    def setUp(self):
        super().setUp()
        self.creds = self.enroll()
        self.node_id = self.creds["node_id"]
        self.agent = self.make_agent(artifact_read_chunk=300)
        self.task_id, _ = self.cp.create_task(self.node_id, "build")
        self.agent.poll_commands_once(wait_ms=0)
        self.core_id = "task-000001"
        self.data = os.urandom(5500)
        self.core.add_file(self.core_id, "out/result.bin", self.data)
        self.core.add_file(self.core_id, "notes.md", b"# notes\n")
        self.core.add_file(self.core_id, "empty.txt", b"")
        self.core.add_file(self.core_id, "evil-link", b"x", symlink=True)

    def finish(self):
        self.core.set_state(self.core_id, "completed", result={"summary": "built"}, event="task_completed")
        self.agent.sync_events_once()

    def arts(self):
        return {a["name"]: a for a in self.cp.artifacts.values()}

    def test_chunked_upload_with_content_range_and_checksum(self):
        self.finish()
        self.assertEqual(self.agent.upload_artifacts_once(), 3)
        arts = self.arts()
        self.assertEqual(set(arts), {"result.bin", "notes.md", "empty.txt"}, "symlinks are skipped")
        big = arts["result.bin"]
        self.assertEqual(big["state"], "available")
        self.assertEqual(bytes(big["data"]), self.data)
        self.assertEqual(big["sha256"], hashlib.sha256(self.data).hexdigest())
        self.assertEqual(big["ranges"], [(0, 999), (1000, 1999), (2000, 2999), (3000, 3999), (4000, 4999),
                                         (5000, 5499)])
        self.assertEqual(arts["empty.txt"]["state"], "available")
        # artifact_available events reach the Control Plane
        self.agent.sync_events_once()
        avail = [e for e in self.cp.task_events(self.task_id) if e["type"] == "artifact_available"]
        self.assertEqual({e["payload"]["name"] for e in avail}, {"result.bin", "notes.md", "empty.txt"})
        ev = next(e for e in avail if e["payload"]["name"] == "result.bin")["payload"]
        self.assertEqual(ev["artifact_id"], big["artifact_id"])
        self.assertEqual(ev["sha256"], big["sha256"])
        self.assertEqual(ev["size"], 5500)
        self.assertEqual(self.agent.store.get_task(self.task_id)["done"], 1)
        # reads from the core were bounded by artifact_read_chunk
        limits = [r["limit"] for r in self.core.requests if r["op"] == "workspace_read_bytes"]
        self.assertTrue(limits and max(limits) <= 300)

    def test_resume_after_interruption(self):
        self.core.tasks[self.core_id]["files"].pop("notes.md")
        self.core.tasks[self.core_id]["files"].pop("empty.txt")
        self.finish()
        self.cp.lose_put_response_after = 2  # third PUT is stored, but its reply is lost
        with self.assertRaises(CPUnavailable):
            self.agent._upload_artifact(self.agent.store.artifacts_to_upload()[0], self.core_id)
        art = next(iter(self.cp.artifacts.values()))
        self.assertEqual(len(art["data"]), 3000)
        row = self.agent.store.task_artifacts(self.task_id)[0]
        self.assertEqual(row["uploaded"], 2000, "local progress lags the server")
        self.assertEqual(row["state"], "uploading")
        # agent restart: progress and artifact id survive in SQLite, resume uses GET received
        self.agent.store.close()
        self.agents.remove(self.agent)
        agent = self.make_agent(artifact_read_chunk=300)
        self.assertEqual(agent.upload_artifacts_once(), 1)
        self.assertEqual(len(self.cp.artifacts), 1, "resume must not re-init the upload")
        self.assertEqual(bytes(art["data"]), self.data)
        self.assertEqual(art["state"], "available")
        self.assertEqual(art["ranges"][3][0], 3000, "upload resumed at the server offset")
        self.assertIn(("GET", f"/v1/node/artifacts/{art['artifact_id']}/content"), self.cp.requests)

    def test_server_checksum_mismatch_reuploads(self):
        self.finish()
        self.cp.corrupt_uploads = True
        self.agent.upload_artifacts_once()
        rows = {r["name"]: r for r in self.agent.store.task_artifacts(self.task_id)}
        self.assertEqual(rows["result.bin"]["state"], "pending")
        self.assertEqual(rows["result.bin"]["error"], "checksum_mismatch")
        self.cp.corrupt_uploads = False
        self.agent.upload_artifacts_once()
        rows = {r["name"]: r for r in self.agent.store.task_artifacts(self.task_id)}
        self.assertEqual(rows["result.bin"]["state"], "available")

    def test_local_checksum_mismatch_never_completes(self):
        self.core.tasks[self.core_id]["files"]["out/result.bin"]["sha_override"] = "0" * 64
        self.finish()
        self.agent.upload_artifacts_once()
        rows = {r["name"]: r for r in self.agent.store.task_artifacts(self.task_id)}
        self.assertEqual(rows["result.bin"]["state"], "rejected")
        self.assertNotIn(("POST", f"/v1/node/artifacts/{rows['result.bin']['artifact_id']}/complete"),
                         self.cp.requests)

    def test_size_limit(self):
        self.agent.store.close()
        self.agents.remove(self.agent)
        self.agent = self.make_agent(max_artifact_bytes=1000)
        self.finish()
        self.agent.upload_artifacts_once()
        rows = {r["name"]: r for r in self.agent.store.task_artifacts(self.task_id)}
        self.assertEqual(rows["result.bin"]["state"], "skipped")
        self.assertEqual(rows["notes.md"]["state"], "available")
        self.assertNotIn("result.bin", self.arts())

    def test_unsafe_paths_skipped(self):
        self.core.add_file(self.core_id, "../escape.txt", b"nope")
        self.finish()
        self.agent.upload_artifacts_once()
        self.assertNotIn("escape.txt", self.arts())


if __name__ == "__main__":
    unittest.main()
