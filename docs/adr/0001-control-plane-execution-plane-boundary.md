# ADR-0001: Keep the Control Plane separate from the Execution Plane

- **Status:** Accepted
- **Date:** 2026-10-21
- **Scope:** Remote API v1, Control Plane deployment, Execution Node and desktop/mobile clients

## Context

ASGEND has a local orchestrator that selects models, agents and tools and runs work inside a user's project. Remote clients need durable task state, command delivery, event history and artifact transfer, but moving the execution runtime to the remote server would enlarge the blast radius and would require copying user workspaces and model credentials to infrastructure that does not need them.

The network may redeliver commands after a timeout, clients can disconnect, node credentials can be stolen, and the server may be unavailable or compromised. The design must not claim exactly-once execution across an unreliable network.

## Decision

1. The **Control Plane** owns identity, workspace membership/RBAC, node registration and liveness leases, task projections, event cursors, command outbox, artifact metadata/blob access, audit and operational metrics.
2. The **Execution Plane** remains on a registered user/operator-controlled Execution Node. The node delegates accepted commands to the existing local core API; that core continues to own orchestration, model calls, MCP tools, process execution and workspace reads.
3. The server never runs an orchestrator, model, MCP tool, shell command, build or user program. It does not mount a project workspace. It does not create execution-state transitions merely because it accepted or leased a command.
4. Commands use persistent outbox + TTL + lease/ack and are delivered **at least once**. The node deduplicates by `command_id` in local persistent state and submits to the core with `idempotency_key = "cp:" + command_id`. Events are sequenced per task; gaps trigger snapshot/resync.
5. The server can store artifact bytes as an isolated persistent blob service/volume, but only after node-side declaration and server verification of declared size and SHA-256. The server treats artifact bytes as untrusted and serves them as attachments with checksum metadata.
6. API v1 uses explicit HTTPS deployment at the external boundary; the local core TCP/JSON API remains private to the Execution Node and must not be exposed directly to the Internet.

## Threat model and required controls

| Threat | Required control | Failure behavior |
|---|---|---|
| A client guesses another workspace/task/artifact ID | Check current workspace role and object ownership on every request; conceal cross-workspace existence | `not_found` rather than an existence oracle |
| A stolen user/device token | Separate token class, hash at rest, expiry/revoke, RBAC and audit | Reject revoked/disabled token with 401 |
| A stolen node token | Node credentials scope to one `node_id`; rotate/revoke; verify task/node ownership on each node endpoint | Reject revoked token and isolate all other nodes' tasks/commands/artifacts |
| A lost/replayed command or ack | Durable outbox, idempotency key, 60-second lease, TTL, command-ID dedupe and current-lease validation | Redeliver the same command ID; reject stale lease ack; expire after TTL |
| A malicious or buggy node reports a sequence gap or forged task state | Strict node-event schema, per-task `seq`, dedupe, gap report and authoritative snapshot path; server-only event IDs | Reject invalid transition / return gap; do not infer model execution on the server |
| Path traversal, symlink swap, special file, or hostile filename on the core workspace | Core opens relative paths with no-follow semantics; artifact listing includes regular files only; bounds on count/depth/size/hash budget | Skip or reject unsafe entry without escaping the workspace |
| Artifact corruption or fabricated digest | Verify upload length and SHA-256 before making the artifact available; clients verify download digest | Reject upload with checksum mismatch and discard staged bytes |
| Secret or task text leakage through logs/errors/metrics | Structured logs with token/body redaction; bounded low-cardinality labels; safe error envelope and audit details | Fail closed; never include bearer tokens, prompts or raw exception data |
| Service/database/object-store outage | Readiness check, durable DB/outbox, staged blob writes, backup/restore tests and bounded retries | Report unavailable; do not acknowledge work that was not committed |
| Compromised Control Plane host | Minimize privileges, isolate blob store, non-root/read-only container where supported, network restrictions, secret injection, external backups | Residual risk remains: the server can expose metadata and stored artifacts within its trust domain; deployment hardening is mandatory |

## Authentication and authorization invariants

- `ucp_…` user/device tokens, `ncp_…` node tokens and `dlt_…` download tickets are not interchangeable. Raw tokens are shown only when issued; the database stores hashes.
- Roles are `owner`, `operator` and `viewer`, scoped to a workspace. Role checks apply to reads as well as mutations, including ticket redemption and download.
- Pairing and enrollment codes are short-lived, single-use and rate-limited. Token rotation returns a new credential with a bounded grace period; revocation invalidates credentials and expires pending node commands.
- Grant-shaping inputs (pairing role, enrollment permissions and membership replacement) reject unknown properties. Other compatible API objects ignore unknown fields as documented.
- Every response includes an API-version header and a request correlation ID. Errors and audit details are privacy-safe.

## Consequences

- Local execution remains functional without a Control Plane connection; remote control adds a durable coordination layer but does not move the local runtime.
- Network delivery cannot promise exactly-once behavior. Correctness depends on stable IDs, durable outbox/inbox, local core idempotency and resynchronization.
- The Control Plane is still security-sensitive because it stores task metadata and artifact bytes. TLS, strict authorization, backup access control and operational monitoring remain release requirements.
- New execution features must be implemented on the node/core side and mapped into the versioned remote contract; they must not silently add execution capabilities to the server.
