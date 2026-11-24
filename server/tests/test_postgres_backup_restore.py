"""Disposable PostgreSQL pg_dump/pg_restore drill for the Control Plane backup format.

Enabled only for an explicitly supplied disposable ASGEND_TEST_POSTGRES_URL. The
role must have CREATEDB/SUPERUSER so the test can create and drop two temporary DBs.
"""
from __future__ import annotations

import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit, urlunsplit

HERE = Path(__file__).resolve().parent
SERVER_DIR = HERE.parent
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

POSTGRES_URL = os.environ.get("ASGEND_TEST_POSTGRES_URL", "").strip()
PG_TOOLS_AVAILABLE = bool(shutil.which("pg_dump") and shutil.which("pg_restore"))

from asgend_cp import admin, backup  # noqa: E402
from asgend_cp.blobstore import BlobStore  # noqa: E402
from asgend_cp.db import open_storage  # noqa: E402
from asgend_cp.util import new_id  # noqa: E402


def _database_url(base_url: str, database: str, include_password: bool = True) -> str:
    parts = urlsplit(base_url)
    user = unquote(parts.username or "")
    host = parts.hostname or ""
    if ":" in host and not host.startswith("["):
        host = f"[{host}]"
    if parts.port:
        host = f"{host}:{parts.port}"
    auth = quote(user, safe="")
    if include_password and parts.password is not None:
        auth += ":" + quote(unquote(parts.password), safe="")
    netloc = f"{auth}@{host}" if auth else host
    return urlunsplit((parts.scheme, netloc, "/" + quote(database, safe=""), parts.query, ""))


def _pg_env(base_url: str) -> dict:
    env = os.environ.copy()
    parts = urlsplit(base_url)
    if parts.password is not None:
        env["PGPASSWORD"] = unquote(parts.password)
    return env


def _pg_tool_major(tool: str) -> int | None:
    """Major version from e.g. 'pg_restore (PostgreSQL) 16.4'; None when it cannot be parsed."""
    out = subprocess.run([tool, "--version"], capture_output=True, text=True).stdout
    match = re.search(r"\)\s*(\d+)", out)
    return int(match.group(1)) if match else None


def _run_pg_tool(args: list[str], base_url: str) -> None:
    # Use credential-free URLs in argv; passwords are passed through PGPASSWORD.
    safe_args = []
    for arg in args:
        parts = urlsplit(arg)
        if parts.scheme in ("postgres", "postgresql") and parts.password is not None:
            arg = _database_url(arg, parts.path.lstrip("/"), include_password=False)
        safe_args.append(arg)
    result = subprocess.run(safe_args, env=_pg_env(base_url), capture_output=True, text=True)
    if result.returncode:
        # Deliberately omit the full argv to avoid leaking DSNs into unit-test output.
        raise AssertionError(
            f"{os.path.basename(safe_args[0])} failed with exit {result.returncode}: "
            f"{result.stderr[-2000:]}"
        )


@unittest.skipUnless(POSTGRES_URL, "set ASGEND_TEST_POSTGRES_URL to run PostgreSQL backup/restore drill")
@unittest.skipUnless(PG_TOOLS_AVAILABLE, "pg_dump and pg_restore are required for a live restore drill")
class PostgreSQLBackupRestoreDrill(unittest.TestCase):
    def setUp(self):
        try:
            import psycopg
            from psycopg import sql
        except ImportError:
            self.skipTest("psycopg 3 is required for PostgreSQL backup/restore drill")
        self.psycopg = psycopg
        self.sql = sql
        self.tmp = tempfile.TemporaryDirectory(prefix="asgend_pg_restore_drill_")
        self.addCleanup(self.tmp.cleanup)
        self.src_db = "asgend_bksrc_" + uuid.uuid4().hex[:12]
        self.dst_db = "asgend_bkdst_" + uuid.uuid4().hex[:12]

        with psycopg.connect(POSTGRES_URL, autocommit=True) as conn:
            privileges = conn.execute(
                "SELECT rolcreatedb OR rolsuper FROM pg_roles WHERE rolname = current_user"
            ).fetchone()
            if not privileges or not privileges[0]:
                self.skipTest("PostgreSQL role must have CREATEDB or SUPERUSER for disposable restore databases")
            server_major = int(conn.execute("SHOW server_version_num").fetchone()[0]) // 10000
            # A newer pg_dump emits settings (e.g. transaction_timeout) an older server rejects on
            # restore. Production scripts run the tools inside the server container; here, say so.
            client_major = _pg_tool_major("pg_restore")
            if client_major is not None and client_major > server_major:
                self.fail(f"pg_restore {client_major} is newer than PostgreSQL server {server_major}; "
                          f"put matching client tools (version {server_major}) first on PATH")
            conn.execute(sql.SQL("CREATE DATABASE {}").format(sql.Identifier(self.src_db)))
        self.addCleanup(self._drop_database, self.src_db)
        with psycopg.connect(POSTGRES_URL, autocommit=True) as conn:
            conn.execute(sql.SQL("CREATE DATABASE {}").format(sql.Identifier(self.dst_db)))
        self.addCleanup(self._drop_database, self.dst_db)

        self.src_url = _database_url(POSTGRES_URL, self.src_db)
        self.dst_url = _database_url(POSTGRES_URL, self.dst_db)
        self.src_data = os.path.join(self.tmp.name, "source-data")
        self.dst_data = os.path.join(self.tmp.name, "restored-data")
        self.backup_dir = os.path.join(self.tmp.name, "backup")
        self.storage = open_storage(self.src_url, pool_size=2)
        self.addCleanup(self.storage.close)
        self.storage.migrate()

    def _drop_database(self, database: str) -> None:
        try:
            with self.psycopg.connect(POSTGRES_URL, autocommit=True) as conn:
                conn.execute(self.sql.SQL("DROP DATABASE IF EXISTS {} WITH (FORCE)").format(
                    self.sql.Identifier(database)))
        except Exception:
            # Preserve the primary test failure; cleanup is best-effort for disposable DBs only.
            pass

    def test_pg_dump_restore_preserves_database_rows_and_blob_hashes(self):
        workspace_id = admin.create_workspace(self.storage, "backup-restore-fixture")
        now = 1_800_000_000_000
        node_id = "node_backup_" + uuid.uuid4().hex[:12]
        task_id = "rt_backup_" + uuid.uuid4().hex[:12]
        with self.storage.tx() as tx:
            tx.x(
                "INSERT INTO nodes (node_id, workspace_id, name, labels_json, protocol_version, created_at) "
                "VALUES (?, ?, ?, '[]', 1, ?)",
                (node_id, workspace_id, "backup-drill-node", now),
            )
            tx.x(
                "INSERT INTO tasks (task_id, workspace_id, node_id, title, state, created_at, updated_at, "
                "idempotency_key) VALUES (?, ?, ?, ?, 'completed', ?, ?, ?)",
                (task_id, workspace_id, node_id, "backup-drill-task", now, now, "backup-drill-" + uuid.uuid4().hex),
            )

        payload = b"PostgreSQL restore drill: content-addressed artifact.\n"
        digest = hashlib.sha256(payload).hexdigest()
        artifact_id = new_id("art_")
        store = BlobStore(self.src_data)
        store.write_chunk(artifact_id, 0, io.BytesIO(payload), len(payload))
        store.commit(artifact_id, digest)
        with self.storage.tx() as tx:
            tx.x(
                "INSERT INTO artifacts (artifact_id, task_id, workspace_id, node_id, name, mime, size, sha256, "
                "state, received, blob_key, created_at, completed_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'available', ?, ?, ?, ?)",
                (artifact_id, task_id, workspace_id, node_id, "restore-drill.txt", "text/plain", len(payload),
                 digest, len(payload), digest, now, now),
            )

        dump_path = os.path.join(self.backup_dir, "db.pgdump")
        os.makedirs(self.backup_dir, mode=0o700, exist_ok=True)
        _run_pg_tool([
            "pg_dump", "--format=custom", "--no-owner", "--no-privileges",
            "--dbname", self.src_url, "--file", dump_path,
        ], POSTGRES_URL)
        manifest = backup.backup(self.storage, self.src_data, self.backup_dir)
        self.assertEqual(manifest["dialect"], "postgresql")
        self.assertEqual(manifest["artifacts"][0]["sha256"], digest)
        backup.verify_backup(self.backup_dir)
        _run_pg_tool(["pg_restore", "--list", dump_path], POSTGRES_URL)

        # Blob volume restore is performed by the app's verified restore routine;
        # the actual PostgreSQL custom archive is restored by pg_restore.
        backup.restore(self.dst_url, self.dst_data, self.backup_dir, force=True)
        _run_pg_tool([
            "pg_restore", "--exit-on-error", "--clean", "--if-exists", "--no-owner", "--no-privileges",
            "--dbname", self.dst_url, dump_path,
        ], POSTGRES_URL)

        restored = open_storage(self.dst_url, pool_size=2)
        try:
            self.assertEqual(restored.check_schema(), self.storage.schema_version())
            with restored.read() as tx:
                row = tx.q1("SELECT artifact_id, sha256, size FROM artifacts WHERE artifact_id = ?", (artifact_id,))
                task = tx.q1("SELECT title, state FROM tasks WHERE task_id = ?", (task_id,))
            self.assertEqual((row["sha256"], int(row["size"])), (digest, len(payload)))
            self.assertEqual((task["title"], task["state"]), ("backup-drill-task", "completed"))
            self.assertEqual(backup.verify_artifacts(restored, self.dst_data), {"checked": 1, "problems": []})
            with BlobStore(self.dst_data).open_blob(digest) as fh:
                self.assertEqual(fh.read(), payload)
        finally:
            restored.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
