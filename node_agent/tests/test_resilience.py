import json
import os
import socket
import threading
import time
import unittest

from helpers import NodeTestCase

from asgend_node.agent import Backoff, NodeRevoked
from asgend_node.core_client import CoreClient, CoreEndpoint, CoreUnavailable
from asgend_node.cp_client import RateLimited


class RevokeAndBackoffTest(NodeTestCase):
    def setUp(self):
        super().setUp()
        self.creds = self.enroll()
        self.node_id = self.creds["node_id"]

    def test_401_stops_agent_and_persists_revocation(self):
        agent = self.make_agent()
        agent.heartbeat_once()
        self.assertEqual(self.cp.heartbeats[-1]["status"], "ready")
        self.assertEqual(self.cp.heartbeats[-1]["core_version"], "0.9.9-mock")
        self.cp.revoke(self.node_id)
        with self.assertRaises(NodeRevoked):
            agent.heartbeat_once()
        self.assertTrue(agent.revoked)
        self.assertTrue(agent.stop_event.is_set())
        with open(os.path.join(self.state_dir, "credentials.json"), encoding="utf-8") as fh:
            self.assertTrue(json.load(fh)["revoked"])
        # no further requests once revoked
        n = len(self.cp.requests)
        with self.assertRaises(NodeRevoked):
            agent.poll_commands_once(wait_ms=0)
        self.assertEqual(len(self.cp.requests), n)
        # run refuses to start a revoked node
        rc, _, err = self.run_cli("run", "--state-dir", self.state_dir, "--once")
        self.assertEqual(rc, 3)
        self.assertIn("revoked", err)
        rc, out, _ = self.run_cli("status", "--state-dir", self.state_dir)
        self.assertEqual(rc, 3)
        self.assertNotIn(self.creds["node_token"], out + self.logs.text)

    def test_revocation_during_threaded_run_exits_with_code_3(self):
        agent = self.make_agent(heartbeat_interval_seconds=0.05)
        result = {}
        t = threading.Thread(target=lambda: result.setdefault("rc", agent.run()))
        t.start()
        time.sleep(0.3)
        self.cp.revoke(self.node_id)
        t.join(10)
        self.assertFalse(t.is_alive())
        self.assertEqual(result["rc"], 3)
        self.assertNotIn(self.creds["node_token"], self.logs.text)

    def test_429_retry_after_is_honoured(self):
        agent = self.make_agent()
        self.cp.rate_limit_remaining = 1
        self.cp.rate_limit_retry_after = "2"
        with self.assertRaises(RateLimited) as ctx:
            agent.heartbeat_once()
        self.assertEqual(ctx.exception.retry_after, 2.0)
        # the loop sleeps exactly Retry-After (no tight retry)
        self.cp.rate_limit_remaining = 1
        self.cp.rate_limit_retry_after = "0.3"
        waits = []
        calls = []

        def step():
            calls.append(time.monotonic())
            agent.heartbeat_once()
            if len(calls) >= 2:
                agent.stop_event.set()
            return 0.0

        real_wait = agent.stop_event.wait

        def recording_wait(timeout=None):
            waits.append(timeout)
            return real_wait(timeout)

        agent.stop_event.wait = recording_wait
        agent._loop("test", step)
        self.assertEqual(len(calls), 2)
        self.assertAlmostEqual(waits[0], 0.3, places=3)
        self.assertGreaterEqual(calls[1] - calls[0], 0.29)

    def test_cp_outage_uses_jittered_backoff(self):
        agent = self.make_agent(backoff_initial=0.01, backoff_max=0.05)
        self.cp.outage = True
        waits = []
        attempts = []

        def step():
            attempts.append(1)
            if len(attempts) >= 5:
                agent.stop_event.set()
            agent.heartbeat_once()
            return 0.0

        real_wait = agent.stop_event.wait
        agent.stop_event.wait = lambda timeout=None: (waits.append(timeout), real_wait(timeout))[1]
        agent._loop("test", step)
        self.assertGreaterEqual(len(waits), 4)
        self.assertTrue(all(0.005 <= w <= 0.05 for w in waits), waits)
        self.assertFalse(agent.revoked)

    def test_backoff_bounds(self):
        b = Backoff(1.0, 8.0, rng=lambda: 1.0)
        self.assertEqual([b.next() for _ in range(6)], [1.0, 2.0, 4.0, 8.0, 8.0, 8.0])
        b = Backoff(1.0, 8.0, rng=lambda: 0.0)
        self.assertEqual([b.next() for _ in range(4)], [0.5, 1.0, 2.0, 4.0])
        b.reset()
        self.assertEqual(b.next(), 0.5)

    def test_degraded_heartbeat_when_core_down(self):
        agent = self.make_agent()
        self.core.down = True
        agent.heartbeat_once()
        self.assertEqual(self.cp.heartbeats[-1]["status"], "degraded")

    def test_graceful_stop(self):
        agent = self.make_agent(lease_wait_ms=200)
        t = threading.Thread(target=agent.run)
        t.start()
        time.sleep(0.3)
        t0 = time.monotonic()
        agent.stop()
        t.join(10)
        self.assertFalse(t.is_alive())
        self.assertLess(time.monotonic() - t0, 5)


class CoreClientTest(unittest.TestCase):
    def _server(self, reply_fn):
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(5)
        received = []

        def serve():
            while True:
                try:
                    conn, _ = srv.accept()
                except OSError:
                    return
                with conn:
                    data = b""
                    while not data.endswith(b"\n"):
                        chunk = conn.recv(65536)
                        if not chunk:
                            break
                        data += chunk
                    received.append(data)
                    out = reply_fn(data)
                    if out is not None:
                        try:
                            conn.sendall(out)
                        except OSError:
                            pass

        threading.Thread(target=serve, daemon=True).start()
        self.addCleanup(srv.close)
        return srv.getsockname()[1], received

    def test_one_json_line_with_auth_token(self):
        port, received = self._server(lambda d: b'{"ok":true,"version":"x"}\n')
        client = CoreClient(CoreEndpoint(port=port, auth_token="tok-123", timeout=2))
        self.assertEqual(client.ping()["version"], "x")
        req = received[0]
        self.assertTrue(req.endswith(b"\n"))
        self.assertEqual(req.count(b"\n"), 1)
        self.assertEqual(json.loads(req), {"op": "ping", "auth_token": "tok-123"})

    def test_bounded_response(self):
        port, _ = self._server(lambda d: b'{"ok":true,"x":"' + b"a" * 5000 + b'"}\n')
        client = CoreClient(CoreEndpoint(port=port, timeout=2, max_response_bytes=1024))
        with self.assertRaises(CoreUnavailable):
            client.ping()

    def test_timeout(self):
        port, _ = self._server(lambda d: (time.sleep(1.5), None)[1])
        client = CoreClient(CoreEndpoint(port=port, timeout=0.3))
        t0 = time.monotonic()
        with self.assertRaises(CoreUnavailable):
            client.ping()
        self.assertLess(time.monotonic() - t0, 1.4)

    def test_invalid_json_and_refused(self):
        port, _ = self._server(lambda d: b"not json\n")
        with self.assertRaises(CoreUnavailable):
            CoreClient(CoreEndpoint(port=port, timeout=2)).ping()

    @unittest.skipUnless(hasattr(socket, "AF_UNIX") and os.name == "posix", "AF_UNIX not available")
    def test_unix_socket(self):
        import tempfile

        path = os.path.join(tempfile.mkdtemp(), "core.sock")
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
        srv.listen(1)
        self.addCleanup(srv.close)

        def serve():
            conn, _ = srv.accept()
            with conn:
                conn.recv(65536)
                conn.sendall(b'{"ok":true,"version":"unix"}\n')

        threading.Thread(target=serve, daemon=True).start()
        self.assertEqual(CoreClient(CoreEndpoint(unix_socket=path, timeout=2)).ping()["version"], "unix")


if __name__ == "__main__":
    unittest.main()
