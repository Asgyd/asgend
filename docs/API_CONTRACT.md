# ASGEND Remote Control Plane API v1 — normative contract

Status: **normative implementation contract** for the Control Plane, Execution Node agent, Qt client and Flutter/Android client. The machine-readable schema is [`contracts/v1/openapi.yaml`](../contracts/v1/openapi.yaml); JSON Schema definitions and examples are in [`contracts/v1/`](../contracts/v1/). This document records behavioral rules that are difficult to express in OpenAPI alone. `CONTRACT_SKETCH.md` is the binding endpoint inventory. If a change is required, update the sketch, schemas, OpenAPI, examples and tests together; do not silently diverge a client or server.

## 1. Transport, compatibility and request identity

- Base path: `/v1`. External traffic **must use HTTPS**. Plain HTTP may be used only on loopback or when `ASGEND_CP_ALLOW_INSECURE_HTTP=1` is explicitly enabled for development/testing behind a TLS-terminating proxy.
- JSON request and response bodies use UTF-8. Every JSON response body includes `api_version: 1`; responses also carry `X-ASGEND-API-Version: 1` and `X-Request-ID`. The server accepts a caller-provided request ID only when it matches `^[A-Za-z0-9._-]{1,64}$`, otherwise it generates one.
- The default JSON body limit is 1 MiB. Binary artifact upload chunks have a separate configured limit (8 MiB by default). Implementations must enforce the active runtime limits, not assume the example defaults are universal.
- Compatible readers ignore unknown properties unless a schema explicitly marks a grant-shaping request as closed. Pairing-code creation, node-enrollment creation and workspace membership replacement reject unknown properties: silently ignoring an unknown permission/role field could broaden access. New optional response fields may be added within v1; breaking meanings, required fields or state enums require a new major API version.
- Tokens are bearer credentials and are never interchangeable: user/device token `ucp_…`, node token `ncp_…`, and download capability ticket `dlt_…`. Store only credential hashes server-side. Never return credentials except at issuance/rotation, and never include them in logs, audit details or error responses.

## 2. Errors and privacy-safe diagnostics

All non-2xx JSON responses use the envelope:

```json
{
  "api_version": 1,
  "error": {
    "code": "conflict",
    "message": "A safe human-readable explanation",
    "request_id": "req_example",
    "details": {}
  }
}
```

Clients must branch on HTTP status and the stable `error.code`, not parse the human-readable message. Known codes are `invalid_request` (400), `unauthorized` (401), `forbidden` (403), `not_found` (404), `conflict` (409), `gone` (410), `payload_too_large` (413), `checksum_mismatch` / `unprocessable` (422), `rate_limited` / `quota_exceeded` (429), `internal` (500), and `unavailable` (503). `429` responses include `Retry-After` when a useful retry delay is known. A future minor revision may introduce additional codes; clients fall back to HTTP status for unknown codes.

Error details must not contain access tokens, secret configuration, prompts/task bodies, arbitrary filesystem paths, or raw exception text. Requests for an object outside the authenticated workspace return `not_found` rather than revealing whether another tenant owns that ID. `X-Request-ID` is the support correlation key.

## 3. Identity, workspace isolation and RBAC

- Every user-facing read and write re-checks current workspace membership. Revoked/disabled tokens fail closed.
- `owner` can manage membership, pairing, node enrollment/revocation and audit, and has operator/viewer abilities. `operator` can submit, answer, cancel and retry tasks, and read tasks/events/nodes/artifacts. `viewer` can read those resources but cannot mutate them.
- Execution Node credentials are scoped to one registered `node_id`. A node can lease only its own commands and can ingest events, snapshots and artifacts only for tasks assigned to it.
- Download authorization is checked against the issuing user, artifact and workspace on every request; membership removal or token/user revocation invalidates the capability even before its expiry.
- The remote Control Plane is not the Execution Plane. It stores and forwards tasks/commands/events and artifact metadata/bytes, but must not run the orchestrator, decision model, LLMs, MCP tools, builds or user programs, and must not mount a user's project workspace.

## 4. Idempotency, command delivery and execution state

- Task submission requires `idempotency_key`. The key is scoped by workspace and operation kind. Same key + identical normalized request returns the original result; same key + a different request body is `409 conflict`.
- Answer, cancel and retry requests also require an idempotency key. They create a durable command/outbox entry and return `202`; this means **accepted by the Control Plane**, not executed by the node.
- Delivery is at-least-once. A command lease expires after 60 seconds and an unacknowledged command may be delivered again with the same `command_id` but a new `lease_id`. A repeated acknowledgment with the same outcome is idempotent; an acknowledgment with a superseded lease is rejected. Per-task command ordering is preserved so a later cancel cannot overtake an undelivered submit.
- Each command has a TTL. If a submit expires before delivery, the task becomes `delivery_failed` and pending dependent commands expire. A node revocation also expires pending commands. The server never invents a `running` or terminal execution result just because a command was accepted.
- The node must persist command deduplication by `command_id` and call the core with `idempotency_key = "cp:" + command_id`. The core idempotency key is a second line of defense if the node restarts after submitting but before recording its local acknowledgment.
- `pending_delivery` means the submit command is still awaiting node acceptance. `delivered` is set only after the node acknowledges the submit command as accepted. After that point, execution states come from node events or a node snapshot. `stale: true` indicates the owning node is offline/its lease expired while the task is nonterminal; it does not rewrite the last known execution state.

## 5. Task state machine

`contracts/v1/state_machine.json` is the machine-readable source. The transitions below define the normative task transitions; unlisted transitions are rejected (`409 conflict` for user operations or reported as a node event conflict). The server may emit `state_changed` only when the state changes; repeated snapshots that do not change state must not manufacture a state transition.

| ID | From | To | By | Trigger |
|---|---|---|---|---|
| T1 | `new` | `pending_delivery` | user | POST /v1/workspaces/{ws}/tasks creates the task and its submit command |
| T2 | `pending_delivery` | `delivered` | node | ack outcome=accepted of the submit command (sets core_task_id) |
| T3 | `pending_delivery` | `delivery_failed` | node | ack outcome=rejected of the submit command |
| T4 | `pending_delivery` | `delivery_failed` | server | submit command expired (TTL) or node revoked |
| T5 | `delivered`, `queued`, `running`, `waiting_input` | `queued`, `running`, `waiting_input`, `completed`, `completed_without_model`, `failed`, `cancelled` | node | node event state_changed or snapshot; intermediate states may be skipped |
| T6 | `failed` | `queued`, `running`, `waiting_input`, `completed`, `completed_without_model`, `cancelled` | node | core retry_task after an acked retry command (event or snapshot) |
| T7 | `delivery_failed` | `pending_delivery` | user | POST /v1/tasks/{task}/retry creates a new submit command with the original payload |


Task terminal states are `completed`, `completed_without_model`, `failed`, `cancelled` and `delivery_failed`. Only `failed` and `delivery_failed` are eligible to leave a terminal state by an explicit retry transition; completed/cancelled tasks are not revived in v1.

## 6. Command state machine

Commands are durable outbox records, not execution results. Leasing is renewable by redelivery, and only the current lease may be acknowledged. The server expires pending/leased work when TTL or node revocation requires it. A command in a terminal state (`acked`, `rejected`, `expired`) is not transitioned back to a live state.

| ID | From | To | By | Trigger |
|---|---|---|---|---|
| C1 | `new` | `pending` | user | submit/answer/cancel/retry API call (outbox insert in the same transaction) |
| C2 | `pending` | `leased` | node | POST /v1/node/commands/lease (new lease_id, delivery_attempts + 1) |
| C3 | `leased` | `pending` | server | lease expired without ack -> redelivery |
| C4 | `leased` | `acked` | node | ack outcome=accepted with the current lease_id |
| C5 | `leased` | `rejected` | node | ack outcome=rejected with the current lease_id |
| C6 | `pending` | `expired` | server | expires_at passed, node revoked, or the task became delivery_failed |
| C7 | `leased` | `expired` | server | node revoked (immediately, lease ignored) |


## 7. Event cursors, ordering and reconnect

- `event_id` is assigned by the server and monotonically increases within a workspace. It is the user/client pagination cursor; it is not promised to be contiguous.
- `seq` is assigned by the node and contiguous per task, beginning at 1. It is the server's deduplication/gap-detection key for node-originated task events.
- `GET /v1/tasks/{task}/events` and `GET /v1/workspaces/{ws}/events` return events after `after_event_id`; `wait_ms` enables long polling. Persist `last_event_id` and resume after reconnect. The final cursor returned by the page is the cursor to persist.
- The node event pump must persist local sequence numbers, resend after uncertain outcomes, and treat duplicate acceptance as success. If the server reports `gaps`, the node must send an authoritative snapshot for each affected task before continuing incremental events for that task.
- Server restore may invalidate an old cursor. A client must detect a cursor reset/gap, refetch task projections, then resume from the new server cursor. There is no total ordering guarantee between different tasks beyond the workspace cursor's event insertion order.
- Event payloads are typed by `type`; compatible consumers should ignore unknown event types/extra payload fields they do not use. For v1 ingest, the server accepts only the documented node-originated event enum and rejects unsupported incoming node event types. A node may not send server-only state or fabricate a server event ID.

## 8. Artifact transfer and integrity

- The node obtains candidates from the core's `artifact_list`, which lists regular non-symlink workspace files and calculates SHA-256, then reads each file through `workspace_read_bytes` in bounded binary chunks. Paths remain workspace-confined; clients never pass absolute paths or traversal components to the core API.
- `POST /v1/node/artifacts` declares artifact name, MIME, byte size and lowercase SHA-256. Repeating an in-progress declaration with the same task/name/size/hash resumes the same upload. The returned `chunk_size` is authoritative for that server deployment.
- `PUT /v1/node/artifacts/{art}/content` sends a raw byte chunk with `Content-Range: bytes start-end/total` and matching `Content-Length`. Chunks must be sequential starting at the server's current `received` offset. After reconnect, query `GET` on the same endpoint and resume at `received`.
- `POST /v1/node/artifacts/{art}/complete` is the commit boundary. The server verifies exact total size and SHA-256 before setting state to `available`; mismatch discards temporary bytes and marks the artifact `rejected`. An available artifact is immutable. Replacing its contents creates a new artifact ID.
- Users first read metadata, request a short-lived `download-ticket`, then download via `/v1/downloads/{ticket}`. A ticket is scoped to its issuing user and one artifact, and expires after 300 seconds by default. Artifact downloads are attachments and include `X-Checksum-SHA256`; clients must verify the complete downloaded file against metadata before opening/sharing it.
- A single RFC 9110 byte range may return `206` with `Content-Range`. Malformed/multiple ranges may be ignored and return a full response; an unsatisfiable valid range returns `416` with `Content-Range: bytes */size`.
- Enforce configured single-artifact and workspace byte quotas, upload chunk limits, retention and temporary-file cleanup. Never include artifact bytes in SQL rows, task event payloads or ordinary request logs.

## 9. Node lifecycle and trust boundary

1. An owner creates a short-lived, single-use enrollment code in a workspace. The node redeems it once, receives a node-specific bearer token, and stores that secret using the operating system's credential store or a permission-restricted local file.
2. The node sends heartbeats (`ready`, `busy` or `degraded`) and renews a server-side lease. `online` is derived from a live lease; loss of heartbeat marks the node offline but does not erase task history.
3. Token rotation returns a new node token and permits a short grace period for the prior token. Revocation immediately disables all credentials for the node.
4. Node enrollment, user pairing, login and credential rotation are rate-limited. Token material is generated with cryptographically secure randomness, stored as a hash, redacted from structured logs, and never reused across credential classes.
5. The service must check `Host`/origin policy, request size, content length, TLS termination assumptions and all workspace/node ownership on each request. Do not trust user-provided `node_id`, `task_id`, `artifact_id`, MIME or file name as proof of access/validity.

## 10. Compliance and changes

- JSON schemas under `contracts/v1/schemas/` define the fields and basic validation. Valid and invalid examples are indexed in `contracts/v1/examples/index.json`; `tests/contracts/test_contracts.py` validates schemas, examples, OpenAPI endpoint coverage, state machine consistency and reference delivery/event scenarios.
- Every contract change updates this document, `CONTRACT_SKETCH.md`, the affected JSON Schema, OpenAPI, indexed examples and tests in the same change set. Record each binding change in [`contracts/v1/CHANGES.md`](../contracts/v1/CHANGES.md).
- Deployment-specific items that cannot be guaranteed by the API document (such as TLS certificate issuance, backups, quota values and PostgreSQL availability) must be covered by the deployment guide and operational acceptance test.
