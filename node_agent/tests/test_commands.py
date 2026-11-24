import os
import unittest

from helpers import NodeTestCase

from mock_cp import iso


class CommandTest(NodeTestCase):
    def setUp(self):
        super().setUp()
        self.creds = self.enroll()
        self.node_id = self.creds["node_id"]

    def submit_and_deliver(self, node_agent, **payload):
        task_id, cmd_id = self.cp.create_task(self.node_id, "write a report", **payload)
        self.assertEqual(node_agent.poll_commands_once(wait_ms=0), 1)
        return task_id, cmd_id

    def test_submit_maps_to_core_with_idempotency_key(self):
        agent = self.make_agent()
        task_id, cmd_id = self.submit_and_deliver(agent, agent="coder", session_id="s-1", priority=7)
        submits = [r for r in self.core.requests if r["op"] == "submit"]
        self.assertEqual(len(submits), 1)
        self.assertEqual(submits[0]["idempotency_key"], "cp:" + cmd_id)
        self.assertEqual(submits[0]["agent"], "coder")
        self.assertEqual(submits[0]["session_id"], "s-1")
        self.assertEqual(submits[0]["priority"], 7)
        cmd = self.cp.commands[cmd_id]
        self.assertEqual(cmd["state"], "acked")
        self.assertEqual(cmd["core_task_id"], "task-000001")
        self.assertEqual(self.cp.tasks[task_id]["state"], "delivered")
        self.assertEqual(agent.store.get_task(task_id)["core_task_id"], "task-000001")

    def test_redelivered_submit_is_acked_again_without_reexecution(self):
        agent = self.make_agent()
        task_id, cmd_id = self.submit_and_deliver(agent)
        self.cp.redeliver(cmd_id)
        self.assertEqual(agent.poll_commands_once(wait_ms=0), 1)
        self.assertEqual(self.core.count("submit"), 1, "redelivery must not reach the core again")
        self.assertEqual(len(self.core.tasks), 1)
        acks = [a for a in self.cp.acks if a["command_id"] == cmd_id]
        self.assertEqual(len(acks), 2)
        self.assertEqual({a["outcome"] for a in acks}, {"accepted"})
        self.assertEqual({a["core_task_id"] for a in acks}, {"task-000001"})
        self.assertEqual(acks[0]["lease_id"] != acks[1]["lease_id"], True)

    def test_lost_local_state_still_does_not_duplicate_core_task(self):
        agent = self.make_agent()
        task_id, cmd_id = self.submit_and_deliver(agent)
        agent.store.close()
        self.agents.remove(agent)
        # wipe node.sqlite3 entirely: dedupe falls back to the core idempotency key
        for suffix in ("", "-wal", "-shm"):
            path = os.path.join(self.state_dir, "node.sqlite3" + suffix)
            if os.path.exists(path):
                os.remove(path)
        fresh = self.make_agent()
        self.assertIsNone(fresh.store.get_command(cmd_id))
        self.cp.redeliver(cmd_id)
        self.assertEqual(fresh.poll_commands_once(wait_ms=0), 1)
        self.assertEqual(self.core.count("submit"), 2)
        self.assertEqual(len(self.core.tasks), 1, "core idempotency key must dedupe")
        acks = [a for a in self.cp.acks if a["command_id"] == cmd_id]
        self.assertEqual([a["core_task_id"] for a in acks], ["task-000001", "task-000001"])
        self.assertEqual(fresh.store.get_task(task_id)["core_task_id"], "task-000001")

    def test_answer_cancel_retry_mapping(self):
        agent = self.make_agent()
        task_id, _ = self.submit_and_deliver(agent)
        core_id = "task-000001"
        self.core.ask(core_id, "Which language?", [("c", "C"), ("py", "Python")])
        ans = self.cp.add_command(self.node_id, task_id, "answer", {"answer": "Python", "option_id": "py"})
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[ans]["state"], "acked")
        req = [r for r in self.core.requests if r["op"] == "answer"][-1]
        self.assertEqual((req["task_id"], req["answer"], req["option_id"]), (core_id, "Python", "py"))
        self.assertEqual(self.core.tasks[core_id]["status"], "queued")

        # answering again while not waiting_input: core validation error -> rejected with error
        ans2 = self.cp.add_command(self.node_id, task_id, "answer", {"answer": "again"})
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[ans2]["state"], "rejected")
        self.assertIn("not waiting", self.cp.commands[ans2]["error"])

        self.core.set_state(core_id, "failed", error="boom")
        retry = self.cp.add_command(self.node_id, task_id, "retry", {})
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[retry]["state"], "acked")
        self.assertEqual([r for r in self.core.requests if r["op"] == "retry_task"][-1]["task_id"], core_id)
        self.assertEqual(self.core.tasks[core_id]["status"], "queued")

        cancel = self.cp.add_command(self.node_id, task_id, "cancel", {})
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[cancel]["state"], "acked")
        self.assertEqual([r for r in self.core.requests if r["op"] == "cancel"][-1]["task_id"], core_id)
        self.assertEqual(self.core.tasks[core_id]["status"], "cancelled")

        # cancel of a terminal task -> rejected by core -> rejected ack
        cancel2 = self.cp.add_command(self.node_id, task_id, "cancel", {})
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[cancel2]["state"], "rejected")

    def test_rejections(self):
        agent = self.make_agent()
        # core validation failure (unknown agent)
        task_id, cmd = self.cp.create_task(self.node_id, "x", agent="no-such-agent")
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[cmd]["state"], "rejected")
        self.assertIn("unknown agent", self.cp.commands[cmd]["error"])
        self.assertIsNone(agent.store.get_task(task_id))
        # expired command
        t2, _ = self.cp.create_task(self.node_id, "y")
        expired = self.cp.add_command(self.node_id, t2, "submit", {"task": "late"}, expires_at=iso(1000))
        self.cp.commands[self.cp.command_order[-2]]["state"] = "acked"  # not under test
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[expired]["state"], "rejected")
        self.assertIn("expired", self.cp.commands[expired]["error"])
        # unknown task for answer
        unknown = self.cp.add_command(self.node_id, "rt_UNKNOWN", "answer", {"answer": "a"})
        self.cp.tasks["rt_UNKNOWN"] = {"task_id": "rt_UNKNOWN", "node_id": self.node_id, "state": "x",
                                       "expected_seq": 1, "events": []}
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[unknown]["state"], "rejected")
        self.assertIn("unknown task", self.cp.commands[unknown]["error"])
        # unknown command type
        weird = self.cp.add_command(self.node_id, t2, "format_disk", {})
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[weird]["state"], "rejected")
        # empty submit
        t3, empty = self.cp.create_task(self.node_id, "")
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[empty]["state"], "rejected")
        self.assertEqual(self.core.count("submit"), 1)

    def test_core_unavailable_leaves_command_for_redelivery(self):
        agent = self.make_agent()
        self.core.down = True
        task_id, cmd = self.cp.create_task(self.node_id, "x")
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[cmd]["state"], "leased")
        self.assertEqual(self.cp.acks, [])
        self.assertIsNone(agent.store.get_command(cmd))
        self.core.down = False
        self.cp.redeliver(cmd)
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[cmd]["state"], "acked")

    def test_retryable_core_error_not_acked(self):
        agent = self.make_agent()
        self.core.fail_next["submit"] = {"ok": False, "error": "task queue is full", "retryable": True}
        _, cmd = self.cp.create_task(self.node_id, "x")
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.acks, [])
        self.cp.redeliver(cmd)
        agent.poll_commands_once(wait_ms=0)
        self.assertEqual(self.cp.commands[cmd]["state"], "acked")


if __name__ == "__main__":
    unittest.main()
