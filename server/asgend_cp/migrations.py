"""Versioned schema migrations shared by the SQLite and PostgreSQL backends.

Statements are written in the portable SQL subset understood by both engines
(TEXT/INTEGER/BIGINT, composite primary keys, ``CREATE INDEX IF NOT EXISTS``,
``ALTER TABLE .. ADD COLUMN``). Timestamps are BIGINT epoch milliseconds,
JSON documents are TEXT. Migrations are expand-only (additive) so a backup
taken before ``migrate`` is the rollback path.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Migration:
    version: int
    name: str
    statements: tuple


MIGRATIONS: tuple = (
    Migration(1, "core identity, nodes, tasks, outbox, events, audit", (
        """CREATE TABLE users (
            user_id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            created_at BIGINT NOT NULL,
            disabled INTEGER NOT NULL DEFAULT 0
        )""",
        """CREATE TABLE workspaces (
            workspace_id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            created_at BIGINT NOT NULL,
            event_seq BIGINT NOT NULL DEFAULT 0
        )""",
        """CREATE TABLE memberships (
            workspace_id TEXT NOT NULL REFERENCES workspaces(workspace_id),
            user_id TEXT NOT NULL REFERENCES users(user_id),
            role TEXT NOT NULL,
            created_at BIGINT NOT NULL,
            PRIMARY KEY (workspace_id, user_id)
        )""",
        """CREATE TABLE tokens (
            token_hash TEXT PRIMARY KEY,
            token_id TEXT NOT NULL UNIQUE,
            kind TEXT NOT NULL,
            user_id TEXT,
            node_id TEXT,
            name TEXT,
            created_at BIGINT NOT NULL,
            expires_at BIGINT,
            revoked_at BIGINT,
            last_used_at BIGINT
        )""",
        "CREATE INDEX IF NOT EXISTS tokens_user_idx ON tokens (user_id)",
        "CREATE INDEX IF NOT EXISTS tokens_node_idx ON tokens (node_id)",
        """CREATE TABLE pairing_codes (
            pairing_id TEXT PRIMARY KEY,
            code_hash TEXT NOT NULL UNIQUE,
            workspace_id TEXT NOT NULL,
            role TEXT NOT NULL,
            created_by TEXT NOT NULL,
            created_at BIGINT NOT NULL,
            expires_at BIGINT NOT NULL,
            used_at BIGINT,
            used_by TEXT
        )""",
        """CREATE TABLE enrollment_codes (
            enrollment_id TEXT PRIMARY KEY,
            code_hash TEXT NOT NULL UNIQUE,
            workspace_id TEXT NOT NULL,
            name TEXT,
            labels_json TEXT NOT NULL DEFAULT '[]',
            created_by TEXT NOT NULL,
            created_at BIGINT NOT NULL,
            expires_at BIGINT NOT NULL,
            used_at BIGINT,
            node_id TEXT
        )""",
        """CREATE TABLE nodes (
            node_id TEXT PRIMARY KEY,
            workspace_id TEXT NOT NULL REFERENCES workspaces(workspace_id),
            name TEXT NOT NULL,
            labels_json TEXT NOT NULL DEFAULT '[]',
            protocol_version INTEGER NOT NULL DEFAULT 1,
            core_version TEXT,
            revoked_at BIGINT,
            last_seen_at BIGINT,
            lease_expires_at BIGINT,
            created_at BIGINT NOT NULL
        )""",
        "CREATE INDEX IF NOT EXISTS nodes_ws_idx ON nodes (workspace_id)",
        """CREATE TABLE tasks (
            task_id TEXT PRIMARY KEY,
            workspace_id TEXT NOT NULL REFERENCES workspaces(workspace_id),
            node_id TEXT NOT NULL REFERENCES nodes(node_id),
            core_task_id TEXT,
            title TEXT NOT NULL,
            agent TEXT,
            session_id TEXT,
            priority INTEGER NOT NULL DEFAULT 5,
            state TEXT NOT NULL,
            question_json TEXT,
            attempts INTEGER NOT NULL DEFAULT 0,
            created_at BIGINT NOT NULL,
            updated_at BIGINT NOT NULL,
            last_event_id BIGINT NOT NULL DEFAULT 0,
            last_seq BIGINT NOT NULL DEFAULT 0,
            idempotency_key TEXT NOT NULL,
            result_summary TEXT,
            error TEXT
        )""",
        "CREATE INDEX IF NOT EXISTS tasks_ws_idx ON tasks (workspace_id, task_id)",
        """CREATE TABLE commands (
            command_id TEXT PRIMARY KEY,
            task_id TEXT NOT NULL REFERENCES tasks(task_id),
            workspace_id TEXT NOT NULL,
            node_id TEXT NOT NULL,
            type TEXT NOT NULL,
            payload_json TEXT NOT NULL,
            idempotency_key TEXT NOT NULL,
            state TEXT NOT NULL,
            created_at BIGINT NOT NULL,
            expires_at BIGINT NOT NULL,
            lease_id TEXT,
            lease_expires_at BIGINT,
            delivery_attempts INTEGER NOT NULL DEFAULT 0,
            error TEXT,
            acked_at BIGINT,
            outcome TEXT
        )""",
        "CREATE INDEX IF NOT EXISTS commands_task_idx ON commands (task_id, command_id)",
        """CREATE TABLE events (
            workspace_id TEXT NOT NULL,
            event_id BIGINT NOT NULL,
            task_id TEXT NOT NULL,
            node_id TEXT,
            seq BIGINT,
            type TEXT NOT NULL,
            occurred_at BIGINT NOT NULL,
            received_at BIGINT NOT NULL,
            schema_version INTEGER NOT NULL DEFAULT 1,
            payload_json TEXT NOT NULL,
            PRIMARY KEY (workspace_id, event_id)
        )""",
        "CREATE INDEX IF NOT EXISTS events_task_idx ON events (task_id, event_id)",
        "CREATE UNIQUE INDEX IF NOT EXISTS events_task_seq_uq ON events (task_id, seq)",
        """CREATE TABLE idempotency (
            workspace_id TEXT NOT NULL,
            kind TEXT NOT NULL,
            idem_key TEXT NOT NULL,
            fingerprint TEXT NOT NULL,
            result_id TEXT NOT NULL,
            created_at BIGINT NOT NULL,
            PRIMARY KEY (workspace_id, kind, idem_key)
        )""",
        """CREATE TABLE audit (
            audit_id TEXT PRIMARY KEY,
            workspace_id TEXT,
            actor TEXT NOT NULL,
            action TEXT NOT NULL,
            target TEXT,
            result TEXT NOT NULL,
            request_id TEXT,
            created_at BIGINT NOT NULL,
            details_json TEXT NOT NULL DEFAULT '{}'
        )""",
        "CREATE INDEX IF NOT EXISTS audit_ws_idx ON audit (workspace_id, audit_id)",
    )),
    Migration(2, "artifacts and download tickets", (
        """CREATE TABLE artifacts (
            artifact_id TEXT PRIMARY KEY,
            task_id TEXT NOT NULL REFERENCES tasks(task_id),
            workspace_id TEXT NOT NULL,
            node_id TEXT NOT NULL,
            name TEXT NOT NULL,
            mime TEXT NOT NULL,
            size BIGINT NOT NULL,
            sha256 TEXT NOT NULL,
            state TEXT NOT NULL,
            received BIGINT NOT NULL DEFAULT 0,
            blob_key TEXT,
            created_at BIGINT NOT NULL,
            completed_at BIGINT,
            expires_at BIGINT
        )""",
        "CREATE INDEX IF NOT EXISTS artifacts_task_idx ON artifacts (task_id, artifact_id)",
        "CREATE INDEX IF NOT EXISTS artifacts_ws_idx ON artifacts (workspace_id, state)",
        """CREATE TABLE download_tickets (
            ticket_hash TEXT PRIMARY KEY,
            artifact_id TEXT NOT NULL,
            user_id TEXT NOT NULL,
            workspace_id TEXT NOT NULL,
            created_at BIGINT NOT NULL,
            expires_at BIGINT NOT NULL
        )""",
    )),
    Migration(3, "node presence detail, delivery latency, retention indexes", (
        "ALTER TABLE nodes ADD COLUMN node_state TEXT",
        "ALTER TABLE nodes ADD COLUMN active_tasks INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE commands ADD COLUMN first_leased_at BIGINT",
        "CREATE INDEX IF NOT EXISTS commands_node_state_idx ON commands (node_id, state, command_id)",
        "CREATE INDEX IF NOT EXISTS commands_expiry_idx ON commands (state, expires_at)",
        "CREATE INDEX IF NOT EXISTS artifacts_expiry_idx ON artifacts (state, expires_at)",
        "CREATE INDEX IF NOT EXISTS tickets_expiry_idx ON download_tickets (expires_at)",
        "CREATE INDEX IF NOT EXISTS tasks_ws_state_idx ON tasks (workspace_id, state)",
    )),
)

LATEST_VERSION = MIGRATIONS[-1].version

SCHEMA_TABLE = """CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    applied_at BIGINT NOT NULL
)"""
