#!/usr/bin/env python3
"""Runtime regression test: task + session enqueue must roll back atomically."""
from __future__ import annotations
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
# Reuse the project integration harness to exercise the real daemon/RPC boundary.
sys.path.insert(0, str(PROJECT / "tests"))
sys.argv = [sys.argv[0], str(BINARY), str(PROJECT)]
import integration as harness  # noqa: E402


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="ao-core-db-atomicity-") as temporary:
        root = Path(temporary)
        db_path = root / "core.sqlite3"
        port = harness.free_port()
        proc = harness.start_server(port, db_path, sandbox_enabled=False)
        try:
            # The injected failure occurs after the task insert, at the session upsert.
            with sqlite3.connect(db_path, timeout=5) as con:
                con.execute("""
                    CREATE TRIGGER fail_test_session_insert
                    BEFORE INSERT ON sessions
                    WHEN NEW.session_id = 'atomicity-failure-session'
                    BEGIN SELECT RAISE(ABORT, 'injected session failure'); END
                """)

            task_marker = "CORE_DB_ATOMICITY_MARKER_20261023"
            tasks_dir = root / "asgend-home" / "tasks"
            tasks_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
            tasks_dir.chmod(0o700)
            before_workspaces = set(tasks_dir.iterdir())

            reply = harness.rpc(port, {
                "op": "submit",
                "task": task_marker,
                "session_id": "atomicity-failure-session",
                "idempotency_key": "atomicity-failure-key",
            })
            assert reply.get("ok") is False, reply

            with sqlite3.connect(db_path, timeout=5) as con:
                rows = con.execute("SELECT id FROM tasks WHERE task_json LIKE ?", (f"%{task_marker}%",)).fetchall()
                assert rows == [], f"task row survived failed session update: {rows!r}"
                # Ensure the transaction was rolled back and the connection remains usable.
                con.execute("DROP TRIGGER fail_test_session_insert")

            after_workspaces = set(tasks_dir.iterdir())
            assert after_workspaces == before_workspaces, (
                f"workspace leaked after failed enqueue: {after_workspaces - before_workspaces!r}"
            )

            # The same idempotency key can be used after rollback; the failed attempt
            # must not have occupied the UNIQUE key.
            accepted = harness.rpc(port, {
                "op": "submit",
                "task": "CORE_DB_ATOMICITY_RECOVERY_MARKER",
                "session_id": "atomicity-recovery-session",
                "idempotency_key": "atomicity-failure-key",
            })
            assert accepted.get("ok") is True, accepted
            assert accepted.get("duplicate") is False, accepted
            print("core db atomicity test passed")
        finally:
            harness.stop_server(proc)


if __name__ == "__main__":
    main()
