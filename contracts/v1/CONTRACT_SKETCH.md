# ASGEND Remote API v1 — binding contract sketch

Status: **binding baseline for all v1 implementations** (Control Plane server, Execution Node agent,
Qt client, Flutter/Android client). The A0 work expands this into JSON Schema + OpenAPI under
`contracts/v1/`; it may add detail but must not contradict the decisions below. If an implementer
needs a change, it is recorded in `contracts/v1/CHANGES.md` and reported, never silently diverged.

## 1. Transport and versioning

- HTTPS + JSON (UTF-8). Plain HTTP is accepted by the server only when bound to loopback or when
  `ASGEND_CP_ALLOW_INSECURE_HTTP=1` is set explicitly (tests/dev, behind a TLS reverse proxy).
- Base path `/v1`. Every response carries header `X-ASGEND-API-Version: 1` and JSON bodies carry
  `"api_version": 1` at top level.
- Compatible clients/servers **ignore unknown fields**. Breaking changes require `/v2`.
- Auth: `Authorization: Bearer <token>`. Token kinds are distinguishable by prefix and are never
  interchangeable: user/device tokens `ucp_…`, node tokens `ncp_…`, download tickets `dlt_…`
  (ticket only valid on `/v1/downloads/{ticket}`). Tokens are stored server-side as SHA-256 hashes only.
- Request body limit 1 MiB for JSON (artifact chunks: 8 MiB per request). Responses never echo tokens.
- Request ID: server returns `X-Request-ID` (accepts a client-supplied one if it matches `^[A-Za-z0-9._-]{1,64}$`).

## 2. Errors

HTTP status + body `{"api_version":1,"error":{"code":"…","message":"…","request_id":"…","details":{}}}`.
Codes: `invalid_request`(400) `unauthorized`(401) `forbidden`(403) `not_found`(404)
`conflict`(409) `gone`(410, expired ticket/command) `payload_too_large`(413) `checksum_mismatch`(422)
`unprocessable`(422) `rate_limited`(429, with `Retry-After`) `quota_exceeded`(429) `internal`(500)
`unavailable`(503). Objects of another workspace return `not_found` (no existence oracle).

## 3. Identifiers

Opaque strings with prefix: `usr_`, `ws_`, `node_`, `rt_` (remote task), `cmd_`, `art_`, `aud_`,
`enr_` (enrollment), `pair_`. Generated as prefix + 26 chars Crockford base32 (ULID-like, time-sortable).
`event_id` is a server-assigned **64-bit integer, strictly increasing per workspace** (used as cursor).
`seq` is a node-assigned per-task integer starting at 1, used for dedupe/gap detection.
`idempotency_key`: client-chosen, `^[A-Za-z0-9._:-]{8,128}$`, unique per (workspace, operation kind).

## 4. Roles (RBAC per workspace)

- `owner`: everything, incl. members, node enrollment/revoke, audit read, pairing codes.
- `operator`: submit/answer/cancel/retry, read tasks/events/nodes, list/download artifacts.
- `viewer`: read tasks/events/nodes, list/download artifacts. No mutations.
Checked on every request. Revoked node/user tokens fail closed with 401.

## 5. Entities (field names are normative)

```json
RemoteTask {
  "task_id":"rt_…", "workspace_id":"ws_…", "node_id":"node_…", "core_task_id":"task-…|null",
  "title":"string(<=200)", "agent":"string|null", "session_id":"string|null", "priority":0-9,
  "state":"pending_delivery|delivered|queued|running|waiting_input|completed|completed_without_model|failed|cancelled|delivery_failed",
  "question": {"text":"…","options":["…"]} | null,
  "attempts":int, "created_at":"RFC3339", "updated_at":"RFC3339", "last_event_id":int,
  "stale":bool,            // true when owning node is offline / last heartbeat older than lease
  "idempotency_key":"…", "result_summary":"string|null", "error":"string|null"
}
Command / CommandEnvelope {
  "command_id":"cmd_…", "task_id":"rt_…", "workspace_id":"ws_…", "node_id":"node_…",
  "type":"submit|answer|cancel|retry", "payload":{…}, "idempotency_key":"…",
  "state":"pending|leased|acked|rejected|expired",
  "created_at", "expires_at", "lease_id":"string|null", "lease_expires_at":"RFC3339|null",
  "delivery_attempts":int, "error":"string|null"
}
  submit payload: {"task":"text", "agent":?, "session_id":?, "priority":?}
  answer payload: {"answer":"text"}     cancel/retry payload: {}
TaskEvent {
  "event_id":int, "task_id":"rt_…", "workspace_id":"ws_…", "node_id":"node_…"|null, "seq":int|null,
  "type":"state_changed|question|answer_submitted|core_event|artifact_available|command_status|delivery_failed",
  "occurred_at":"RFC3339", "received_at":"RFC3339", "schema_version":1, "payload":{…}
}
Node {
  "node_id","workspace_id","name","labels":["…"],"protocol_version":1,"core_version":"string|null",
  "status":"online|offline|revoked","last_seen_at":"RFC3339|null","lease_expires_at":"RFC3339|null",
  "created_at"
}
Artifact {
  "artifact_id","task_id","workspace_id","node_id","name"(basename, no '/' or '\\' or NUL, <=255),
  "mime","size":int,"sha256":"hex64","state":"uploading|available|rejected|expired",
  "created_at","expires_at":"RFC3339|null"
}
AuditEvent { "audit_id","workspace_id","actor":"usr_…|node_…|system","action","target","result":"ok|denied|error","request_id","at","details":{} }
```

## 6. User/client endpoints (Bearer `ucp_…`)

| Method | Path | Role | Notes |
|---|---|---|---|
| GET | `/v1/health` | none | liveness `{status:"ok"}` |
| GET | `/v1/ready` | none | DB reachable + migrations current |
| GET | `/v1/metrics` | `ASGEND_CP_METRICS_TOKEN` bearer | Prometheus text; no prompts/secrets/paths in labels |
| GET | `/v1/me` | any | user, workspaces + roles |
| POST | `/v1/auth/logout` | any | revokes the presenting token |
| POST | `/v1/workspaces/{ws}/pairing-codes` | owner | `{role}` → `{code, expires_at}` (10 min, single use, 8+ chars) |
| POST | `/v1/pairing/redeem` | none | `{code, device_name}` → `{token:"ucp_…", user_id, workspace_id, role}`; rate limited |
| GET | `/v1/workspaces/{ws}/nodes` | viewer | |
| POST | `/v1/workspaces/{ws}/node-enrollments` | owner | `{name?, labels?}` → `{enrollment_code, expires_at}` (15 min, single use) |
| POST | `/v1/nodes/{node}/revoke` | owner | node tokens invalidated, pending commands for it → `expired` w/ error |
| POST | `/v1/workspaces/{ws}/tasks` | operator | `{node_id, task, agent?, session_id?, priority?, idempotency_key, ttl_seconds?(10..86400, default 3600)}` → 201 new / 200 same task on repeated key (409 if same key, different body) |
| GET | `/v1/workspaces/{ws}/tasks` | viewer | `?state=&node_id=&limit=(1..200,50)&cursor=` → `{tasks:[…], next_cursor}` |
| GET | `/v1/tasks/{task}` | viewer | |
| GET | `/v1/tasks/{task}/events` | viewer | `?after_event_id=0&limit=(1..500,100)&wait_ms=(0..30000)` long-poll → `{events:[…], last_event_id}` |
| GET | `/v1/workspaces/{ws}/events` | viewer | same, workspace-wide |
| POST | `/v1/tasks/{task}/answer` | operator | `{answer, idempotency_key}` → Command (202); 409 unless task `waiting_input` |
| POST | `/v1/tasks/{task}/cancel` | operator | `{idempotency_key}` → Command (202). Server never cancels by itself; node executes |
| POST | `/v1/tasks/{task}/retry` | operator | `{idempotency_key}` → Command (202); 409 unless `failed`/`delivery_failed` |
| GET | `/v1/tasks/{task}/commands` | viewer | |
| GET | `/v1/tasks/{task}/artifacts` | viewer | |
| GET | `/v1/artifacts/{art}` | viewer | metadata |
| POST | `/v1/artifacts/{art}/download-ticket` | viewer | → `{ticket:"dlt_…", url:"/v1/downloads/dlt_…", expires_at}` (300 s, bound to artifact+user) |
| GET | `/v1/downloads/{ticket}` | ticket | streams bytes; `Range` supported; headers `Content-Type`, `Content-Length`, `X-Checksum-SHA256`, `Content-Disposition: attachment` |
| GET/PUT | `/v1/workspaces/{ws}/members` | owner | role bindings |
| GET | `/v1/workspaces/{ws}/audit` | owner | `?limit&cursor` |

Task state semantics: on submit the task is `pending_delivery`; `delivered` only after the node acks the
submit command; all further states come **only** from node events/snapshots. TTL expiry of an
undelivered submit → `delivery_failed` + `delivery_failed` event.

## 7. Node channel (Bearer `ncp_…`, except enroll)

| Method | Path | Notes |
|---|---|---|
| POST | `/v1/node/enroll` | `{enrollment_code, name, labels[], protocol_version:1, core_version?}` → `{node_id, workspace_id, node_token:"ncp_…", heartbeat_interval_seconds:15, lease_seconds:45}` |
| POST | `/v1/node/heartbeat` | `{status:"ready|busy|degraded", core_version?, active_tasks:int}` → `{server_time, lease_expires_at}` |
| POST | `/v1/node/token/rotate` | → `{node_token}`; old token valid for 60 s grace |
| POST | `/v1/node/commands/lease` | `{max:1..50, wait_ms:0..30000}` → `{commands:[CommandEnvelope]}`; lease 60 s; un-acked lease expiry → redelivered (at-least-once) |
| POST | `/v1/node/commands/{cmd}/ack` | `{lease_id, outcome:"accepted|rejected", core_task_id?, error?}`; repeated ack with same outcome is idempotent 200 |
| POST | `/v1/node/events` | `{events:[{task_id, seq, type, occurred_at, payload}]}` (≤500) → `{accepted:int, duplicates:int, gaps:[{task_id, expected_seq}]}` |
| POST | `/v1/node/tasks/{task}/snapshot` | `{state, core_task_id, last_seq, attempts, question?, result_summary?, error?}` — authoritative resync after gaps/reconnect |
| POST | `/v1/node/artifacts` | `{task_id, name, mime, size, sha256}` → `{artifact_id, upload_url, chunk_size}`; quota checked here |
| PUT | `/v1/node/artifacts/{art}/content` | raw bytes, header `Content-Range: bytes a-b/total`; resumable; `GET` same path → `{received:int}` |
| POST | `/v1/node/artifacts/{art}/complete` | server verifies SHA-256 + size → `available` or 422 `checksum_mismatch` (object deleted, state `rejected`) |

Nodes may only touch tasks/commands/artifacts that belong to their own `node_id`.
Execution Node must dedupe commands by `command_id` (persisted locally) and pass
`idempotency_key = "cp:" + command_id` to the core `submit` op so a redelivered submit never creates a
second core task.

## 8. Mapping to the existing core TCP/Unix JSON API (docs/PROTOCOL.md)

`submit`→core `submit`; `answer`→core `answer` (`task_id`, `answer`); `cancel`→core `cancel`;
`retry`→core `retry_task`; state from core `status`; incremental events from core `events` with
`after_id` (core event id → node `seq`, monotonically). Artifacts are files of the core task workspace
(`workspace_list` + binary read op `workspace_read_bytes` added to the core in this release:
`{"op":"workspace_read_bytes","task_id","path","offset","limit<=1048576"}` →
`{"data_b64","offset","next_offset","size","eof"}`; plus `{"op":"artifact_list","task_id"}` →
`{"artifacts":[{"path","size","sha256","mime"}]}` limited to regular non-symlink files ≤ 256 MiB).
