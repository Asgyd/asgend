import json
import unittest

from helpers import NodeTestCase

from asgend_node.cp_client import CPUnavailable


class EventTest(NodeTestCase):
    def setUp(self):
        super().setUp()
        self.creds = self.enroll()
        self.node_id = self.creds["node_id"]
        self.agent = self.make_agent()
        self.task_id, _ = self.cp.create_task(self.node_id, "analyse logs")
        self.agent.poll_commands_once(wait_ms=0)
        self.core_id = "task-000001"

    def seqs(self):
        return [e["seq"] for e in self.cp.task_events(self.task_id)]

    def test_translation_and_monotonic_seq(self):
        a = self.agent
        a.sync_events_once()
        self.core.set_state(self.core_id, "running", event="task_started", message="started",
                            payload={"attempt": 1, "auth_token": "core-secret-token",
                                     "workspace_path": "/home/user/secret/ws", "note": "see /home/user/x.txt"})
        a.sync_events_once()
        self.core.ask(self.core_id, "Pick a format", [("md", "Markdown"), ("txt", "Text")])
        a.sync_events_once()
        a.sync_events_once()  # idempotent: nothing new
        self.core.set_state(self.core_id, "completed", result={"summary": "all good"}, event="task_completed")
        a.sync_events_once()

        events = self.cp.task_events(self.task_id)
        seqs = [e["seq"] for e in events]
        self.assertEqual(seqs, list(range(1, len(seqs) + 1)), "seq must start at 1 and be gap-free")
        types = [e["type"] for e in events]
        self.assertEqual(types.count("question"), 1, types)
        states = [e["payload"]["state"] for e in events if e["type"] == "state_changed"]
        self.assertEqual(states, ["queued", "running", "waiting_input", "completed"])
        question = next(e for e in events if e["type"] == "question")["payload"]
        self.assertEqual(question["text"], "Pick a format")
        self.assertEqual(question["options"], ["Markdown", "Text"])
        completed = [e for e in events if e["type"] == "state_changed"][-1]["payload"]
        self.assertEqual(completed["result_summary"], "all good")
        core_events = [e for e in events if e["type"] == "core_event"]
        ids = [e["payload"]["core_event_id"] for e in core_events]
        self.assertEqual(ids, sorted(ids))
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(len(ids), len(self.core.tasks[self.core_id]["events"]))
        blob = json.dumps(events)
        self.assertNotIn("core-secret-token", blob)
        self.assertNotIn("/home/user", blob)
        for e in events:
            self.assertTrue(e["occurred_at"].endswith("Z"))
        # local cursor persisted
        row = a.store.get_task(self.task_id)
        self.assertEqual(row["last_core_event_id"], max(ids))
        self.assertEqual(row["next_seq"], len(events) + 1)
        self.assertEqual(row["done"], 1)

    def test_gap_triggers_snapshot(self):
        a = self.agent
        a.sync_events_once()
        self.snapshots_before = len(self.cp.snapshots)
        self.core.set_state(self.core_id, "running", event="task_started")
        self.cp.forget_events(self.task_id)  # server lost seq 1..n
        a.sync_events_once()
        snaps = [s for s in self.cp.snapshots if s["task_id"] == self.task_id]
        self.assertTrue(snaps, "a gap must trigger a snapshot")
        snap = snaps[-1]
        row = a.store.get_task(self.task_id)
        self.assertEqual(snap["last_seq"], row["next_seq"] - 1)
        self.assertEqual(snap["state"], "running")
        self.assertEqual(snap["core_task_id"], self.core_id)
        self.assertEqual(snap["attempts"], 1)
        # after the snapshot the server expects the next seq and accepts new events without gaps
        self.core.set_state(self.core_id, "completed", result={"summary": "ok"}, event="task_completed")
        posts_before = len(self.cp.event_posts)
        a.sync_events_once()
        self.assertGreater(len(self.cp.event_posts), posts_before)
        self.assertEqual(self.cp.tasks[self.task_id]["state"], "completed")
        self.assertEqual(self.cp.tasks[self.task_id]["expected_seq"], a.store.get_task(self.task_id)["next_seq"])

    def test_snapshot_for_waiting_task_contains_question(self):
        self.core.ask(self.core_id, "Continue?", [("y", "Yes")])
        self.agent.pump_events_once()
        self.agent.store.mark_snapshot_needed(self.task_id)
        self.agent.flush_events_once()
        self.agent.send_snapshots_once()
        snap = self.cp.snapshots[-1]
        self.assertEqual(snap["state"], "waiting_input")
        self.assertEqual(snap["question"], {"text": "Continue?", "options": ["Yes"], "option_ids": ["y"]})

    def test_outage_buffers_events_across_restart_then_flushes(self):
        a = self.agent
        a.sync_events_once()
        delivered_before = len(self.cp.task_events(self.task_id))
        self.cp.outage = True
        self.core.set_state(self.core_id, "running", event="task_started")
        self.core.add_event(self.core_id, "agent_step", "thinking")
        a.pump_events_once()
        with self.assertRaises(CPUnavailable):
            a.flush_events_once()
        buffered = a.store.outbox_size()
        self.assertGreaterEqual(buffered, 3)
        # agent restart during the outage: the buffer is durable
        a.stop()
        a.store.close()
        self.agents.remove(a)
        b = self.make_agent()
        self.assertEqual(b.store.outbox_size(), buffered)
        self.core.set_state(self.core_id, "completed", result={"summary": "done"}, event="task_completed")
        b.pump_events_once()
        with self.assertRaises(CPUnavailable):
            b.flush_events_once()
        self.cp.outage = False
        snaps_before = len(self.cp.snapshots)
        b.sync_events_once()
        self.assertEqual(b.store.outbox_size(), 0)
        events = self.cp.task_events(self.task_id)
        self.assertEqual([e["seq"] for e in events], list(range(1, len(events) + 1)))
        self.assertGreater(len(events), delivered_before)
        self.assertEqual(self.cp.tasks[self.task_id]["state"], "completed")
        self.assertGreater(len(self.cp.snapshots), snaps_before, "reconnect must resync via snapshot")

    def test_batches_are_bounded(self):
        for i in range(620):
            self.core.add_event(self.core_id, "agent_step", f"step {i}")
        self.agent.pump_events_once()
        self.agent.sync_events_once()
        self.assertTrue(all(len(p) <= 500 for p in self.cp.event_posts))
        self.assertGreaterEqual(len(self.cp.task_events(self.task_id)), 620)
        self.assertEqual(self.seqs(), list(range(1, len(self.seqs()) + 1)))

    def test_events_for_unknown_task_do_not_block_buffer(self):
        # simulate a task the server no longer knows about
        self.agent.store.upsert_task("rt_GONE", "task-000001x")
        self.agent.store.append_events("rt_GONE", [("core_event", "2026-01-01T00:00:00Z", {"x": 1})])
        self.core.set_state(self.core_id, "running", event="task_started")
        self.agent.pump_events_once()
        self.agent.flush_events_once()
        self.assertEqual(self.agent.store.outbox_size(), 0)
        self.assertEqual(self.cp.tasks[self.task_id]["state"], "running")


if __name__ == "__main__":
    unittest.main()
