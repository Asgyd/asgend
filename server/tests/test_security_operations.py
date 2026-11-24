from __future__ import annotations

import concurrent.futures
import json
import logging
import os
import tempfile
import threading
import time
import unittest
from unittest import mock

from helpers import CPTestCase  # adds server package to sys.path
from asgend_cp.config import Config, ConfigError
from asgend_cp.logs import JsonFormatter
from asgend_cp.ratelimit import RateLimiter
from scripts.smoke_control_plane_load import run_load_probe


class RateLimiterSafetyTests(unittest.TestCase):
    def test_unique_key_churn_cannot_grow_bucket_table_without_bound(self):
        limiter = RateLimiter(max_keys=16)
        outcomes = [limiter.hit(f"remote:{i}", per_minute=0.0001, burst=2) for i in range(2000)]
        self.assertEqual(len(outcomes), 2000)
        # Fifteen per-key bursts and two tokens in the shared overflow bucket
        # are available; all further unique keys are blocked by that bucket.
        self.assertEqual(sum(wait == 0 for wait in outcomes), 17)
        self.assertGreater(outcomes[-1], 0)
        self.assertLessEqual(len(limiter._buckets), 16)
        # One opaque object key is the shared overflow bucket; no more than 15
        # user-supplied keys can be retained for a 16-slot limit.
        self.assertLessEqual(sum(isinstance(k, str) for k in limiter._buckets), 15)
        self.assertLessEqual(sum(not isinstance(k, str) for k in limiter._buckets), 1)

    def test_unique_key_churn_does_not_scan_full_table_per_request(self):
        limiter = RateLimiter(max_keys=8)
        for i in range(7):
            limiter.hit(f"known:{i}", per_minute=60, burst=2)
        with mock.patch.object(limiter, "_prune", wraps=limiter._prune) as prune:
            for i in range(500):
                limiter.hit(f"unseen:{i}", per_minute=60, burst=2)
        self.assertLessEqual(prune.call_count, 1)
        self.assertLessEqual(len(limiter._buckets), 8)

    def test_concurrent_hits_do_not_exceed_burst(self):
        limiter = RateLimiter(max_keys=32)
        barrier = threading.Barrier(32)

        def hit_one(_index):
            barrier.wait(timeout=3)
            return limiter.hit("same-principal", per_minute=0.0001, burst=5)

        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
            results = list(pool.map(hit_one, range(32)))
        self.assertEqual(sum(result == 0 for result in results), 5)
        self.assertEqual(len(limiter._buckets), 1)

    def test_invalid_cost_cannot_mint_tokens(self):
        limiter = RateLimiter(max_keys=4)
        with self.assertRaises(ValueError):
            limiter.hit("client", per_minute=60, burst=3, cost=-2)
        with self.assertRaises(ValueError):
            limiter.hit("client", per_minute=60, burst=3, cost=4)
        # Failed calls did not create a bucket or consume state.
        self.assertEqual(limiter._buckets, {})


class SecretFileDiagnosticsTests(unittest.TestCase):
    @unittest.skipIf(not hasattr(os, "geteuid") or os.geteuid() == 0, "root bypasses file permissions")
    def test_unreadable_secret_file_names_cause_without_leaking_contents(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "pw.txt")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("never-print-this-password")
            os.chmod(path, 0)
            with self.assertRaises(ConfigError) as ctx:
                Config.load(env={"ASGEND_CP_DB_PASSWORD_FILE": path})
        message = str(ctx.exception)
        self.assertIn("Permission denied", message)
        self.assertIn(f"UID {os.getuid()}", message)
        self.assertNotIn("never-print-this-password", message)


class SecurityOperationsTests(CPTestCase):
    def test_rate_limit_and_active_task_quota_are_visible_in_metrics(self):
        self.srv.config.max_active_tasks = 2
        first = self.submit(key="ops-load-task-001", text="first active task")
        second = self.submit(key="ops-load-task-002", text="second active task")
        rejected = self.srv.post(f"/v1/workspaces/{self.ws_a}/tasks", self.operator["token"], {
            "node_id": self.node_a["node_id"], "task": "quota overflow",
            "idempotency_key": "ops-load-task-003",
        })
        self.assertEqual(rejected.status, 429, rejected)
        self.assertEqual(rejected.json["error"]["code"], "quota_exceeded")

        self.srv.config.rate_limit_per_minute = 0.0001
        self.srv.config.rate_limit_burst = 2
        statuses = [self.srv.get(f"/v1/workspaces/{self.ws_a}/tasks", self.viewer["token"]).status
                    for _ in range(4)]
        self.assertEqual(statuses[:2], [200, 200])
        self.assertEqual(statuses[2:], [429, 429])

        metrics = self.srv.get("/v1/metrics", headers={"Authorization": f"Bearer {self.srv.config.metrics_token}"})
        self.assertEqual(metrics.status, 200, metrics)
        text = metrics.body.decode("utf-8")
        self.assertIn('asgend_cp_quota_rejections_total{quota="active_tasks"} 1', text)
        self.assertIn('asgend_cp_rate_limited_total{scope="api"}', text)
        self.assertNotIn(first["task_id"], text)
        self.assertNotIn(second["task_id"], text)

    def test_artifact_quota_rejects_before_reserving_second_upload(self):
        self.srv.config.workspace_artifact_quota_bytes = 4
        task = self.submit(key="ops-artifact-task-01", text="create small artifact")
        first = self.srv.post("/v1/node/artifacts", self.node_a["node_token"], {
            "task_id": task["task_id"], "name": "small.bin", "mime": "application/octet-stream",
            "size": 4, "sha256": "a" * 64,
        })
        self.assertEqual(first.status, 201, first)
        second = self.srv.post("/v1/node/artifacts", self.node_a["node_token"], {
            "task_id": task["task_id"], "name": "extra.bin", "mime": "application/octet-stream",
            "size": 1, "sha256": "b" * 64,
        })
        self.assertEqual(second.status, 429, second)
        self.assertEqual(second.json["error"]["code"], "quota_exceeded")

    def test_load_probe_rejects_plain_http_for_non_loopback_and_bad_limits(self):
        with self.assertRaises(ValueError):
            run_load_probe(base_url="http://staging.example", workspace="ws_test", token="x", requests=1)
        with self.assertRaises(ValueError):
            run_load_probe(base_url="https://staging.example", workspace="ws_test", token="x", requests=1.5)
        with self.assertRaises(ValueError):
            run_load_probe(base_url="https://staging.example", workspace="ws_test", token="bad\r\nAuthorization: Bearer injected", requests=1)

    def test_load_probe_runs_against_local_control_plane_without_logging_secrets(self):
        result = run_load_probe(
            base_url=f"http://127.0.0.1:{self.srv.port}", workspace=self.ws_a,
            token=self.viewer["token"], requests=30, workers=6, timeout=5,
        )
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["requests"], 30)
        self.assertEqual(result["status_counts"], {"200": 30})
        self.assertEqual(result["error_count"], 0)
        self.assertGreaterEqual(result["latency_ms"]["p95"], 0)
        self.assertNotIn(self.viewer["token"], json.dumps(result))

    def test_json_logs_redact_secrets_from_fields_and_message(self):
        record = logging.LogRecord(
            name="asgend_cp.test", level=logging.INFO, pathname=__file__, lineno=1,
            msg="request had Bearer ucp_super-secret-token",
            args=(), exc_info=None,
        )
        record.fields = {
            "authorization": "Bearer ucp_super-secret-token",
            "db_password": "db-top-secret",
            "url": "postgresql://admin:url-secret@db.invalid/app",
            "details": {"answer": "a-private-answer", "safe_code": "not-a-secret-key"},
        }
        rendered = JsonFormatter().format(record)
        payload = json.loads(rendered)
        for secret in ("super-secret-token", "db-top-secret", "url-secret", "a-private-answer"):
            self.assertNotIn(secret, rendered)
        self.assertEqual(payload["authorization"], "[REDACTED]")
        self.assertEqual(payload["db_password"], "[REDACTED]")
        self.assertEqual(payload["details"]["answer"], "[REDACTED]")
        self.assertEqual(payload["details"]["safe_code"], "not-a-secret-key")

    def test_concurrent_read_smoke_stays_available_and_preserves_response_contract(self):
        path = f"/v1/workspaces/{self.ws_a}/tasks"
        started = time.monotonic()
        workers, requests = 12, 120

        def call(_index):
            response = self.srv.get(path, self.viewer["token"], timeout=10)
            return response.status, response.header("X-Request-ID"), response.json

        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(call, range(requests)))
        elapsed = time.monotonic() - started
        self.assertEqual(len(results), requests)
        self.assertTrue(all(status == 200 for status, _, _ in results))
        self.assertTrue(all(request_id for _, request_id, _ in results))
        self.assertTrue(all("tasks" in payload for _, _, payload in results))
        # This is a deterministic availability smoke test, not a throughput SLO.
        # It records elapsed time in failure diagnostics without a flaky latency cap.
        self.assertGreater(elapsed, 0)


if __name__ == "__main__":
    unittest.main()
