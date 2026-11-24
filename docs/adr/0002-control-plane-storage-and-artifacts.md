# ADR-0002: Separate relational control state from artifact blobs

- **Status:** Accepted
- **Date:** 2026-10-21
- **Scope:** Control Plane persistence, migrations, artifact transfer and backup/restore

## Context

Remote task state and command delivery require transactions, uniqueness constraints and ordered cursors. Artifact payloads can be large and must be streamed, hashed, quota-checked and downloaded with ranges. Storing bytes directly in relational rows would inflate backups and make resumable transfer and retention more difficult.

The codebase supports SQLite for local development/tests and PostgreSQL for production deployments. Artifact storage can use a persistent local volume or an S3-compatible object store behind the same blob-store interface.

## Decision

1. Use **SQLite in development/tests** and **PostgreSQL in production** for users, workspaces, memberships, token hashes, nodes/leases, task projections, event history, command outbox/inbox metadata, artifact metadata, download-ticket hashes and audit events.
2. Keep artifact bytes in a separate persistent blob store. The relational record stores the artifact ID, workspace/node/task scope, sanitized name, declared MIME, byte size, SHA-256, lifecycle state, received offset, storage key and timestamps—not the content.
3. Use ordered schema migrations and a schema-version guard. Startup/readiness must fail closed when the database is unreachable or migrations are not current. Migrations must be repeatable only when designed to be so; schema changes are not applied silently by client processes.
4. Uploads begin as `uploading` metadata plus a private staging object. Chunks are sequential and resumable by `received` offset. The server computes the final byte count and SHA-256 before commit. Only verified data becomes `available`; incomplete, invalid or checksum-mismatched data is not downloadable.
5. Available artifacts are immutable. A new upload cannot replace an existing available object's bytes under the same artifact ID. Expiry/retention deletes blobs and updates metadata through explicit maintenance logic.
6. Backups cover the relational database and blob store as one logical recovery set. A restore check validates schema/migration version, object references, sizes and SHA-256 values before the deployment is declared healthy.
7. External production deployments use separate persistent volumes/services, non-root processes, narrow filesystem permissions, resource limits and injected runtime secrets. No credential, token or artifact content is written into build images or source control.

## Consequences

- Metadata queries and command/event transactions remain small and database-native; large file transfer streams independently and supports HTTP Range downloads.
- A database-only backup is incomplete. Operational readiness requires coordinated database and blob backup, restore drills, and orphan reconciliation.
- Local volume storage is simpler for single-instance deployments, while object storage supports larger/distributed deployments; changing the backend does not change the v1 artifact contract.
- Quotas, retention policy and chunk limits remain deployment-configurable and must be surfaced in the deployment guide and metrics rather than assumed by clients.
