"""Live PostgreSQL acceptance: migration upgrade, server restart, durable idempotency and lease retry.

The test is enabled only when ASGEND_TEST_POSTGRES_URL points at a disposable PostgreSQL database.
It creates an isolated schema per test and uses the real Control Plane HTTP server against it.
"""
from __future__ import annotations

import os
import sys
import unittest
import uuid
from pathlib import Path
from urllib.parse import parse_qsl, quote, urlencode, urlsplit, urlunsplit

HERE = Path(__file__).resolve().parent
SERVER_DIR = HERE.parent
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

POSTGRES_URL = os.environ.get("ASGEND_TEST_POSTGRES_URL", "").strip()

from asgend_cp.db import open_storage  # noqa: E402
from asgend_cp.migrations import LATEST_VERSION  # noqa: E402
from helpers import CPTestCase, Server  # noqa: E402


def _schema_url(base_url: str, schema: str) -> str:
    """Add libpq search_path options without losing SSL or other connection query options."""
    parts = urlsplit(base_url)
    query = [(k, v) for k, v in parse_qsl(parts.query, keep_blank_values=True) if k != "options"]
    query.append(("options", f"-c search_path={schema}"))
    return urlunsplit((parts.scheme, parts.netloc, parts.path,
                       urlencode(query, doseq=True, quote_via=quote), parts.fragment))


class PostgreSQLHarnessConfigTests(unittest.TestCase):
    def test_schema_url_encodes_search_path_and_preserves_existing_options(self):
        url = _schema_url("postgresql://asgend:secret@localhost/asgend_test?sslmode=require",
                          "asgend_it_0123456789abcdef")
        parts = urlsplit(url)
        query = dict(parse_qsl(parts.query, keep_blank_values=True))
        self.assertEqual(parts.scheme, "postgresql")
        self.assertEqual(parts.netloc, "asgend:secret@localhost")
        self.assertEqual(query["sslmode"], "require")
        self.assertEqual(query["options"], "-c search_path=asgend_it_0123456789abcdef")


@unittest.skipUnless(POSTGRES_URL, "set ASGEND_TEST_POSTGRES_URL to run live PostgreSQL acceptance")
class PostgreSQLRestartAcceptance(CPTestCase):
    """Run the same HTTP API with PostgreSQL and restart it without losing command state."""

    def setUp(self):
        try:
            import psycopg  # noqa: F401
            from psycopg import sql
        except ImportError:
            self.skipTest("psycopg 3 is required for live PostgreSQL acceptance")

        self._schema = "asgend_it_" + uuid.uuid4().hex[:16]
        # Create/drop the isolated schema through an admin connection. No test touches public tables.
        with psycopg.connect(POSTGRES_URL, autocommit=True) as conn:
            conn.execute(sql.SQL("CREATE SCHEMA {}").format(sql.Identifier(self._schema)))
        self.addCleanup(self._drop_schema)
        self._db_url = _schema_url(POSTGRES_URL, self._schema)

        # Start from an intentionally older schema. Control Plane startup must finish the upgrade.
        storage = open_storage(self._db_url, pool_size=2)
        try:
            self.assertEqual(storage.schema_version(), 0)
            self.assertEqual(storage.migrate(target=1), [1])
            self.assertEqual(storage.schema_version(), 1)
        finally:
            storage.close()

        self.server_overrides = {
            "db_url": self._db_url,
            "auto_migrate": True,
            "command_lease_seconds": 5,
        }
        super().setUp()

    def _drop_schema(self):
        try:
            import psycopg
            from psycopg import sql
            with psycopg.connect(POSTGRES_URL, autocommit=True) as conn:
                conn.execute(sql.SQL("DROP SCHEMA IF EXISTS {} CASCADE").format(
                    sql.Identifier(self._schema)))
        except Exception:
            # Preserve the original test failure if the temporary DB becomes unavailable.
            pass

    def _restart_control_plane(self):
        self.srv.stop()
        self.srv = Server(self.tmp, self.clock, **self.server_overrides)

    def test_migrations_idempotency_and_command_redelivery_survive_restart(self):
        st = self.srv.storage
        self.assertEqual(st.dialect, "postgresql")
        self.assertTrue(st.ping())
        self.assertEqual(st.check_schema(), LATEST_VERSION)
        self.assertEqual([int(row["version"]) for row in st.applied_migrations()],
                         list(range(1, LATEST_VERSION + 1)))
        self.assertEqual(st.migrate(), [], "re-running current migrations must be a no-op")

        # Submit one idempotent command and lease it, simulating a node that goes away before ack.
        task = self.submit(key="pg-restart-idempotency-001", text="PostgreSQL restart acceptance")
        original = next(c for c in self.lease() if c["task_id"] == task["task_id"])
        self.assertEqual(original["delivery_attempts"], 1)
        self.assertEqual(original["state"], "leased")

        # Restart the HTTP service with the same database and artifact volume.
        self.clock.advance(6)
        self._restart_control_plane()
        after_restart = next(c for c in self.lease() if c["task_id"] == task["task_id"])
        self.assertEqual(after_restart["command_id"], original["command_id"])
        self.assertNotEqual(after_restart["lease_id"], original["lease_id"])
        self.assertEqual(after_restart["delivery_attempts"], 2)
        self.assertEqual(after_restart["state"], "leased")

        ack = self.ack(after_restart, outcome="accepted", core_task_id="pg-core-task-01")
        self.assertEqual(ack.status, 200, ack)
        duplicate = self.submit(key="pg-restart-idempotency-001",
                                text="PostgreSQL restart acceptance", expect=200)
        self.assertEqual(duplicate["task_id"], task["task_id"])

        # A second restart must retain the ack, task projection and ordered event cursor.
        self._restart_control_plane()
        current = self.srv.get(f"/v1/tasks/{task['task_id']}", self.viewer["token"])
        self.assertEqual(current.status, 200, current)
        self.assertEqual(current.json["state"], "delivered", current.json)
        self.assertEqual(current.json["core_task_id"], "pg-core-task-01")
        events = self.events(task["task_id"], after=0)
        ids = [int(e["event_id"]) for e in events["events"]]
        self.assertTrue(ids, "events should remain queryable after a restart")
        self.assertEqual(ids, sorted(set(ids)), "workspace event IDs must be ordered and unique")

        with self.srv.storage.read() as tx:
            row = tx.q1("SELECT state, delivery_attempts, outcome FROM commands WHERE command_id = ?",
                        (original["command_id"],))
        self.assertEqual(row["state"], "acked")
        self.assertEqual(int(row["delivery_attempts"]), 2)
        self.assertEqual(row["outcome"], "accepted")


if __name__ == "__main__":
    unittest.main(verbosity=2)
