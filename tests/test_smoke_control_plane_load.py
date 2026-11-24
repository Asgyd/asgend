from __future__ import annotations

import importlib.util
import json
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "smoke_control_plane_load.py"
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("smoke_control_plane_load", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class LoadProbeTests(unittest.TestCase):
    def test_metrics_and_token_redaction_against_local_mock(self) -> None:
        state = {"status": 200, "hits": 0, "auth": []}
        lock = threading.Lock()

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, _format: str, *args: object) -> None:
                return

            def do_GET(self) -> None:  # noqa: N802
                with lock:
                    state["hits"] += 1
                    state["auth"].append(self.headers.get("Authorization", ""))
                    status = state["status"]
                body = b'{"tasks":[]}' if status == 200 else b'{"error":"not logged"}'
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base_url = f"http://127.0.0.1:{server.server_port}"
        token = "synthetic-secret-token-that-must-not-appear-in-results"
        try:
            result = MODULE.run_load_probe(base_url=base_url, workspace="workspace-1", token=token,
                                           requests=32, workers=4, timeout=2.0, max_error_rate=0.0)
            self.assertTrue(result["passed"], result)
            self.assertEqual(result["status_counts"], {"200": 32})
            self.assertEqual(result["requests"], 32)
            self.assertEqual(result["error_count"], 0)
            self.assertGreater(result["requests_per_second"], 0)
            self.assertLessEqual(result["latency_ms"]["min"], result["latency_ms"]["p50"])
            self.assertLessEqual(result["latency_ms"]["p50"], result["latency_ms"]["p95"])
            if result["latency_ms"]["p99"] is not None:
                self.assertLessEqual(result["latency_ms"]["p95"], result["latency_ms"]["p99"])
            self.assertNotIn(token, json.dumps(result))
            self.assertNotIn("workspace-1", json.dumps(result))
            self.assertEqual(state["hits"], 32)
            self.assertTrue(all(value == f"Bearer {token}" for value in state["auth"]))
            self.assertEqual(result["latency_ms"]["samples"], 32)
            self.assertIsNotNone(result["latency_ms"]["p95"])
            self.assertIsNone(result["latency_ms"]["p99"])
            self.assertEqual(result["latency_ms"]["tail_percentile_status"]["p99"],
                             "not_reported_requires_100_samples")

            state["status"] = 503
            failures = MODULE.run_load_probe(base_url=base_url, workspace="workspace-1", token=token,
                                             requests=5, workers=2, timeout=2.0, max_error_rate=0.0)
            self.assertFalse(failures["passed"])
            self.assertEqual(failures["status_counts"], {"503": 5})
            self.assertEqual(failures["error_count"], 5)
            self.assertNotIn(token, json.dumps(failures))
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_rejects_non_loopback_plain_http_and_unbounded_values(self) -> None:
        with self.assertRaises(ValueError):
            MODULE.run_load_probe(base_url="http://example.com", workspace="w", token="t", requests=1)
        with self.assertRaises(ValueError):
            MODULE.run_load_probe(base_url="http://127.0.0.1", workspace="w", token="t", requests=100001)
        with self.assertRaises(ValueError):
            MODULE.run_load_probe(base_url="http://127.0.0.1", workspace="w", token="t", workers=257)

    def test_tail_percentiles_require_enough_samples(self) -> None:
        short = MODULE._latency_summary([1.0, 2.0, 3.0, 4.0, 5.0])
        self.assertEqual(short["samples"], 5)
        self.assertIsNone(short["p95"])
        self.assertIsNone(short["p99"])
        p95_ready = MODULE._latency_summary([float(n) for n in range(1, 21)])
        self.assertIsNotNone(p95_ready["p95"])
        self.assertIsNone(p95_ready["p99"])
        p99_ready = MODULE._latency_summary([float(n) for n in range(1, 101)])
        self.assertIsNotNone(p99_ready["p99"])


if __name__ == "__main__":
    unittest.main()
