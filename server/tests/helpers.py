"""Test harness: runs the real HTTP server on an ephemeral loopback port (SQLite backend)."""

from __future__ import annotations

import hashlib
import http.client
import io
import json
import logging
import os
import shutil
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER_DIR = os.path.dirname(HERE)
if SERVER_DIR not in sys.path:
    sys.path.insert(0, SERVER_DIR)

from asgend_cp import admin  # noqa: E402
from asgend_cp.cli import build_server  # noqa: E402
from asgend_cp.config import Config  # noqa: E402
from asgend_cp.db import open_storage  # noqa: E402
from asgend_cp.logs import setup_logging  # noqa: E402
from asgend_cp.util import Clock  # noqa: E402

METRICS_TOKEN = "metrics-test-token-0123456789"


class Resp:
    def __init__(self, status, headers, body):
        self.status = status
        self.headers = headers
        self.body = body

    @property
    def json(self):
        return json.loads(self.body.decode("utf-8"))

    def header(self, name):
        for k, v in self.headers:
            if k.lower() == name.lower():
                return v
        return None

    def __repr__(self):
        return f"<Resp {self.status} {self.body[:300]!r}>"


class Server:
    """One control-plane instance bound to 127.0.0.1:0."""

    def __init__(self, tmpdir: str, clock: Clock | None = None, **overrides):
        self.tmpdir = tmpdir
        self.clock = clock or Clock()
        cfg = Config()
        cfg.bind = "127.0.0.1"
        cfg.port = 0
        cfg.db_url = "sqlite:///" + os.path.join(tmpdir, "cp.sqlite3")
        cfg.data_dir = os.path.join(tmpdir, "artifacts")
        cfg.metrics_token = METRICS_TOKEN
        cfg.auto_migrate = True
        cfg.sweep_interval_seconds = 3600  # tests drive maintenance explicitly
        cfg.cleanup_interval_seconds = 3600
        cfg.shutdown_grace_seconds = 5
        cfg.rate_limit_per_minute = 100000
        cfg.rate_limit_burst = 100000
        cfg.auth_rate_limit_per_minute = 100000
        cfg.auth_rate_limit_burst = 100000
        cfg.failed_auth_per_minute = 100000
        for k, v in overrides.items():
            setattr(cfg, k, v)
        self.config = cfg
        self.app, self.httpd = build_server(cfg, clock=self.clock)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.05},
                                       daemon=True)
        self.thread.start()
        self.app.start_background()

    @property
    def storage(self):
        return self.app.storage

    def stop(self):
        self.httpd.graceful_stop(self.config.shutdown_grace_seconds)
        self.app.close()
        self.thread.join(timeout=5)

    def request(self, method, path, token=None, body=None, headers=None, raw=None, timeout=40):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        h = dict(headers or {})
        if token:
            h["Authorization"] = "Bearer " + token
        data = None
        if raw is not None:
            data = raw
        elif body is not None:
            data = json.dumps(body).encode("utf-8")
            h.setdefault("Content-Type", "application/json")
        try:
            conn.request(method, path, body=data, headers=h)
            r = conn.getresponse()
            return Resp(r.status, r.getheaders(), r.read())
        finally:
            conn.close()

    # convenience wrappers
    def get(self, path, token=None, **kw):
        return self.request("GET", path, token, **kw)

    def post(self, path, token=None, body=None, **kw):
        return self.request("POST", path, token, body if body is not None else {}, **kw)

    def put(self, path, token=None, body=None, **kw):
        return self.request("PUT", path, token, body, **kw)


class CPTestCase(unittest.TestCase):
    """Fresh server + bootstrap: workspace A (owner/operator/viewer + node A),
    workspace B (owner + node B)."""

    server_overrides: dict = {}

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="asgend_cp_test_")
        self.log_stream = io.StringIO()
        self._log_handler = setup_logging("DEBUG", self.log_stream)
        self.clock = Clock()
        self.srv = Server(self.tmp, self.clock, **self.server_overrides)
        st = self.srv.storage
        self.ws_a = admin.create_workspace(st, "alpha")
        self.ws_b = admin.create_workspace(st, "beta")
        self.owner = admin.create_user(st, "owner-a", self.ws_a, "owner")
        self.operator = admin.create_user(st, "operator-a", self.ws_a, "operator")
        self.viewer = admin.create_user(st, "viewer-a", self.ws_a, "viewer")
        self.owner_b = admin.create_user(st, "owner-b", self.ws_b, "owner")
        self.node_a = self.enroll(self.owner["token"], self.ws_a, "node-a")
        self.node_b = self.enroll(self.owner_b["token"], self.ws_b, "node-b")

    def tearDown(self):
        try:
            self.srv.stop()
        finally:
            logging.getLogger("asgend_cp").removeHandler(self._log_handler)
            shutil.rmtree(self.tmp, ignore_errors=True)

    # ------------------------------------------------------------ helpers
    def enroll(self, owner_token, ws, name, labels=None):
        r = self.srv.post(f"/v1/workspaces/{ws}/node-enrollments", owner_token,
                          {"name": name, "labels": labels or ["linux"]})
        self.assertEqual(r.status, 201, r)
        code = r.json["enrollment_code"]
        r = self.srv.post("/v1/node/enroll", None, {"enrollment_code": code, "name": name,
                                                     "labels": labels or ["linux"],
                                                     "protocol_version": 1, "core_version": "0.9.9"})
        self.assertEqual(r.status, 201, r)
        return r.json

    def submit(self, token=None, ws=None, node_id=None, key="submit-key-0001", text="hello world",
               expect=201, **extra):
        body = {"node_id": node_id or self.node_a["node_id"], "task": text, "idempotency_key": key}
        body.update(extra)
        r = self.srv.post(f"/v1/workspaces/{ws or self.ws_a}/tasks", token or self.operator["token"], body)
        self.assertEqual(r.status, expect, r)
        return r.json

    def lease(self, node=None, mx=10, wait_ms=0):
        r = self.srv.post("/v1/node/commands/lease", (node or self.node_a)["node_token"],
                          {"max": mx, "wait_ms": wait_ms})
        self.assertEqual(r.status, 200, r)
        return r.json["commands"]

    def ack(self, cmd, node=None, outcome="accepted", expect=200, **extra):
        body = {"lease_id": cmd["lease_id"], "outcome": outcome}
        body.update(extra)
        r = self.srv.post(f"/v1/node/commands/{cmd['command_id']}/ack", (node or self.node_a)["node_token"],
                          body)
        self.assertEqual(r.status, expect, r)
        return r

    def node_events(self, events, node=None, expect=200):
        r = self.srv.post("/v1/node/events", (node or self.node_a)["node_token"], {"events": events})
        self.assertEqual(r.status, expect, r)
        return r.json

    def deliver(self, task):
        cmds = self.lease()
        cmd = [c for c in cmds if c["task_id"] == task["task_id"]][0]
        self.ack(cmd, core_task_id="task-1")
        return cmd

    def events(self, task_id, token=None, after=0, wait_ms=0):
        r = self.srv.get(f"/v1/tasks/{task_id}/events?after_event_id={after}&wait_ms={wait_ms}",
                         token or self.viewer["token"])
        self.assertEqual(r.status, 200, r)
        return r.json

    def upload_artifact(self, task_id, data: bytes, name="report.txt", mime="text/plain", node=None,
                        chunk=None, sha=None):
        node = node or self.node_a
        r = self.srv.post("/v1/node/artifacts", node["node_token"],
                          {"task_id": task_id, "name": name, "mime": mime, "size": len(data),
                           "sha256": sha or hashlib.sha256(data).hexdigest()})
        self.assertIn(r.status, (200, 201), r)
        aid = r.json["artifact_id"]
        chunk = chunk or max(1, len(data))
        off = 0
        while off < len(data):
            part = data[off:off + chunk]
            rr = self.srv.request("PUT", f"/v1/node/artifacts/{aid}/content", node["node_token"], raw=part,
                                  headers={"Content-Range": f"bytes {off}-{off + len(part) - 1}/{len(data)}",
                                           "Content-Type": "application/octet-stream"})
            self.assertEqual(rr.status, 200, rr)
            off += len(part)
        return aid

    def logs(self) -> str:
        return self.log_stream.getvalue()
