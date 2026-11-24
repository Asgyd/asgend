#!/usr/bin/env python3
"""Protocol client for the Compose backup/restore drill (stdlib only).

It talks to a disposable stack through the public NGINX TLS endpoint and acts
as both a user and an Execution Node, so the drill covers the real proxy, API,
PostgreSQL and artifact volume. Phases share a private JSON state file:

  seed    enroll a node, submit/lease/ack a task, post events, upload an artifact
  mutate  submit a second task that a restore must roll back
  verify  check the seeded task/events/artifact bytes and that the mutation is gone

The owner token is read from ASGEND_DRILL_OWNER_TOKEN, never from argv. Never
point this at a production deployment: it creates nodes and tasks.
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import ssl
import sys
import time
import urllib.parse
from datetime import datetime, timezone

ARTIFACT = b"ASGEND compose drill artifact\n" + bytes(range(256)) * 64


class DrillError(RuntimeError):
    pass


class Client:
    def __init__(self, base_url: str, ca_file: str, host_header: str | None):
        parts = urllib.parse.urlsplit(base_url)
        if parts.scheme != "https":
            raise DrillError("the drill must go through the TLS proxy (https://)")
        self.host = parts.hostname or "127.0.0.1"
        self.port = parts.port or 443
        self.host_header = host_header or parts.netloc
        self.ctx = ssl.create_default_context(cafile=ca_file)
        # The drill certificate is issued for the public name, while the socket may target loopback.
        self.ctx.check_hostname = False

    def request(self, method, path, token=None, body=None, raw=None, headers=None, expect=None):
        hdrs = {"Host": self.host_header, "Accept": "application/json"}
        if token:
            hdrs["Authorization"] = f"Bearer {token}"
        data = raw
        if body is not None:
            data = json.dumps(body).encode()
            hdrs["Content-Type"] = "application/json"
        hdrs.update(headers or {})
        conn = http.client.HTTPSConnection(self.host, self.port, timeout=30, context=self.ctx)
        try:
            conn.request(method, path, body=data, headers=hdrs)
            resp = conn.getresponse()
            payload = resp.read(16 * 1024 * 1024)
            status, rheaders = resp.status, {k.lower(): v for k, v in resp.getheaders()}
        finally:
            conn.close()
        if expect is not None and status not in (expect if isinstance(expect, tuple) else (expect,)):
            snippet = payload[:300].decode("utf-8", "replace")
            raise DrillError(f"{method} {path.split('?')[0]} -> HTTP {status}, expected {expect}: {snippet}")
        ctype = rheaders.get("content-type", "")
        parsed = json.loads(payload) if payload and ctype.startswith("application/json") else None
        return status, parsed, payload, rheaders


def now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")


def save_state(path: str, state: dict) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        json.dump(state, fh)


def load_state(path: str) -> dict:
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def submit(c: Client, owner: str, ws: str, node_id: str, key: str, text: str) -> str:
    _, task, _, _ = c.request("POST", f"/v1/workspaces/{ws}/tasks", owner,
                              {"node_id": node_id, "task": text, "idempotency_key": key}, expect=201)
    return task["task_id"]


def phase_seed(c: Client, state_path: str, ws: str, owner: str) -> None:
    _, ready, _, _ = c.request("GET", "/v1/ready", expect=200)
    _, enr, _, _ = c.request("POST", f"/v1/workspaces/{ws}/node-enrollments", owner,
                             {"name": "drill-node", "labels": ["linux"]}, expect=201)
    _, node, _, _ = c.request("POST", "/v1/node/enroll", None,
                              {"enrollment_code": enr["enrollment_code"], "name": "drill-node",
                               "labels": ["linux"], "protocol_version": 1, "core_version": "0.9.9"},
                              expect=201)
    ntok = node["node_token"]
    task_id = submit(c, owner, ws, node["node_id"], "compose-drill-seed-0001", "compose drill seed task")
    # Idempotent resubmission must return the same task.
    _, again, _, _ = c.request("POST", f"/v1/workspaces/{ws}/tasks", owner,
                               {"node_id": node["node_id"], "task": "compose drill seed task",
                                "idempotency_key": "compose-drill-seed-0001"}, expect=(200, 201))
    if again["task_id"] != task_id:
        raise DrillError("idempotent resubmission created a different task")
    _, leased, _, _ = c.request("POST", "/v1/node/commands/lease", ntok, {"max": 10, "wait_ms": 0}, expect=200)
    cmd = next((x for x in leased["commands"] if x["task_id"] == task_id), None)
    if cmd is None:
        raise DrillError("seeded submit command was not leased")
    c.request("POST", f"/v1/node/commands/{cmd['command_id']}/ack", ntok,
              {"lease_id": cmd["lease_id"], "outcome": "accepted", "core_task_id": "drill-core-1"}, expect=200)
    events = [
        {"task_id": task_id, "seq": 1, "type": "state_changed", "occurred_at": now_iso(),
         "payload": {"state": "running", "previous_state": "delivered", "attempts": 1}},
        {"task_id": task_id, "seq": 2, "type": "core_event", "occurred_at": now_iso(),
         "payload": {"core_event_id": 1, "core_type": "log", "actor": "drill", "message": "drill marker"}},
    ]
    _, ingest, _, _ = c.request("POST", "/v1/node/events", ntok, {"events": events}, expect=200)
    if ingest.get("accepted") != 2:
        raise DrillError(f"event ingest accepted {ingest.get('accepted')} of 2")
    sha = hashlib.sha256(ARTIFACT).hexdigest()
    _, init, _, _ = c.request("POST", "/v1/node/artifacts", ntok,
                              {"task_id": task_id, "name": "drill-report.bin",
                               "mime": "application/octet-stream", "size": len(ARTIFACT), "sha256": sha},
                              expect=(200, 201))
    aid = init["artifact_id"]
    half = len(ARTIFACT) // 2
    for off, part in ((0, ARTIFACT[:half]), (half, ARTIFACT[half:])):
        c.request("PUT", f"/v1/node/artifacts/{aid}/content", ntok, raw=part,
                  headers={"Content-Type": "application/octet-stream",
                           "Content-Range": f"bytes {off}-{off + len(part) - 1}/{len(ARTIFACT)}"},
                  expect=200)
    c.request("POST", f"/v1/node/artifacts/{aid}/complete", ntok, {}, expect=(200, 201))
    check_artifact(c, owner, aid, sha)
    save_state(state_path, {"workspace_id": ws, "node_id": node["node_id"], "node_token": ntok,
                            "task_id": task_id, "artifact_id": aid, "sha256": sha,
                            "schema_version": ready.get("schema_version")})
    print(json.dumps({"phase": "seed", "ok": True, "task_id": task_id, "artifact_id": aid}))


def check_artifact(c: Client, owner: str, aid: str, sha: str) -> None:
    _, ticket, _, _ = c.request("POST", f"/v1/artifacts/{aid}/download-ticket", owner, {}, expect=201)
    _, _, body, hdrs = c.request("GET", ticket["url"], expect=200)
    if hashlib.sha256(body).hexdigest() != sha or body != ARTIFACT:
        raise DrillError("downloaded artifact bytes do not match the uploaded SHA-256")
    if hdrs.get("x-checksum-sha256") != sha:
        raise DrillError("download is missing the expected X-Checksum-SHA256 header")


def phase_mutate(c: Client, state_path: str, owner: str) -> None:
    st = load_state(state_path)
    st["mutation_task_id"] = submit(c, owner, st["workspace_id"], st["node_id"],
                                    "compose-drill-mutation-0001", "created after the backup")
    save_state(state_path, st)
    print(json.dumps({"phase": "mutate", "ok": True, "task_id": st["mutation_task_id"]}))


def phase_verify(c: Client, state_path: str, owner: str, expect_mutation: bool) -> None:
    st = load_state(state_path)
    _, ready, _, _ = c.request("GET", "/v1/ready", expect=200)
    if ready.get("schema_version") != st["schema_version"]:
        raise DrillError(f"schema version changed: {st['schema_version']} -> {ready.get('schema_version')}")
    _, task, _, _ = c.request("GET", f"/v1/tasks/{st['task_id']}", owner, expect=200)
    if task.get("state") != "running":
        raise DrillError(f"seeded task state is {task.get('state')!r}, expected 'running'")
    _, evs, _, _ = c.request("GET", f"/v1/tasks/{st['task_id']}/events?after_event_id=0&wait_ms=0",
                             owner, expect=200)
    if "drill marker" not in json.dumps(evs):
        raise DrillError("seeded core event is missing from the event log")
    _, arts, _, _ = c.request("GET", f"/v1/tasks/{st['task_id']}/artifacts", owner, expect=200)
    if not any(a["artifact_id"] == st["artifact_id"] and a["state"] == "available" for a in arts["artifacts"]):
        raise DrillError("seeded artifact is not listed as available")
    check_artifact(c, owner, st["artifact_id"], st["sha256"])
    # The node token issued before the backup must keep working after a restore.
    c.request("POST", "/v1/node/heartbeat", st["node_token"], {"status": "busy", "active_tasks": 1}, expect=200)
    if "mutation_task_id" in st:
        status, _, _, _ = c.request("GET", f"/v1/tasks/{st['mutation_task_id']}", owner)
        if expect_mutation and status != 200:
            raise DrillError(f"post-backup task should still exist before restore (HTTP {status})")
        if not expect_mutation and status != 404:
            raise DrillError(f"post-backup task survived the restore (HTTP {status})")
    print(json.dumps({"phase": "verify", "ok": True, "mutation_present": expect_mutation}))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("phase", choices=("seed", "mutate", "verify"))
    ap.add_argument("--url", required=True, help="public https:// base URL of the disposable stack")
    ap.add_argument("--host-header", help="Host header to send (must be in ASGEND_CP_ALLOWED_HOSTS)")
    ap.add_argument("--ca-file", required=True, help="CA/certificate that signed the proxy certificate")
    ap.add_argument("--state", required=True, help="private JSON state file shared by the phases")
    ap.add_argument("--workspace", help="workspace id (seed)")
    ap.add_argument("--expect-mutation", action="store_true", help="verify: post-backup task must exist")
    ap.add_argument("--retries", type=int, default=30, help="seconds to wait for /v1/ready first")
    args = ap.parse_args(argv)
    owner = os.environ.get("ASGEND_DRILL_OWNER_TOKEN", "").strip()
    if not owner:
        print("ASGEND_DRILL_OWNER_TOKEN is required", file=sys.stderr)
        return 2
    c = Client(args.url, args.ca_file, args.host_header)
    for _ in range(max(1, args.retries)):
        try:
            if c.request("GET", "/v1/ready")[0] == 200:
                break
        except OSError:
            pass
        time.sleep(1)
    try:
        if args.phase == "seed":
            if not args.workspace:
                raise DrillError("--workspace is required for seed")
            phase_seed(c, args.state, args.workspace, owner)
        elif args.phase == "mutate":
            phase_mutate(c, args.state, owner)
        else:
            phase_verify(c, args.state, owner, args.expect_mutation)
    except (DrillError, OSError, KeyError, ValueError) as exc:
        print(json.dumps({"phase": args.phase, "ok": False, "error": str(exc)}), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
