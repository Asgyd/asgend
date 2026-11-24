from __future__ import annotations

import hashlib
import io
import json
import os
import tempfile
import tarfile
import unittest
import sys

SERVER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if SERVER_DIR not in sys.path:
    sys.path.insert(0, SERVER_DIR)

from asgend_cp import admin, backup
from asgend_cp.blobstore import BlobStore
from asgend_cp.db import SchemaError, open_storage
from asgend_cp.util import new_id


class StorageMigrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="asgend_migrate_test_")
        self.db_path = os.path.join(self.tmp.name, "cp.sqlite3")
        self.storage = open_storage("sqlite:///" + self.db_path, pool_size=2)

    def tearDown(self):
        self.storage.close()
        self.tmp.cleanup()

    def test_migrations_are_ordered_and_idempotent(self):
        self.assertEqual(self.storage.schema_version(), 0)
        self.assertEqual(self.storage.migrate(target=1), [1])
        self.assertEqual(self.storage.schema_version(), 1)
        self.assertEqual(self.storage.migrate(), [2, 3])
        self.assertEqual(self.storage.schema_version(), 3)
        self.assertEqual(self.storage.check_schema(), 3)
        self.assertEqual(self.storage.migrate(), [])

    def test_unknown_or_newer_schema_is_rejected(self):
        with self.assertRaises(SchemaError):
            self.storage.migrate(target=999)
        self.storage.migrate()
        with self.storage.tx() as t:
            t.x("INSERT INTO schema_migrations (version, name, applied_at) VALUES (?, ?, ?)",
                (999, "future", 0))
        with self.assertRaises(SchemaError):
            self.storage.check_schema()

    def test_postgresql_placeholder_translation_preserves_literals(self):
        from asgend_cp.db import translate_pg
        self.assertEqual(translate_pg("SELECT '?' AS q, value % 2 FROM x WHERE id = ?"),
                         "SELECT '?' AS q, value %% 2 FROM x WHERE id = %s")
        self.assertEqual(translate_pg("SELECT '?' -- ignore ? %\nWHERE id = ?"),
                         "SELECT '?' -- ignore ? %%\nWHERE id = %s")
        self.assertEqual(translate_pg("SELECT $$body? 50%$$, ? /* literal ? 10% */"),
                         "SELECT $$body? 50%%$$, %s /* literal ? 10%% */")


class BackupRestoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="asgend_backup_test_")
        self.source_db = os.path.join(self.tmp.name, "source.sqlite3")
        self.source_data = os.path.join(self.tmp.name, "source-data")
        self.backup_dir = os.path.join(self.tmp.name, "backup")
        self.target_db = os.path.join(self.tmp.name, "restored.sqlite3")
        self.target_data = os.path.join(self.tmp.name, "target-data")
        self.storage = open_storage("sqlite:///" + self.source_db, pool_size=2)
        self.storage.migrate()
        ws = admin.create_workspace(self.storage, "backup-workspace")
        user = admin.create_user(self.storage, "backup-owner", ws, "owner")
        now = 1_800_000_000_000
        node_id = "node_backup_fixture"
        task_id = "rt_backup_fixture"
        with self.storage.tx() as t:
            t.x("INSERT INTO nodes (node_id, workspace_id, name, labels_json, protocol_version, created_at) "
                "VALUES (?, ?, ?, '[]', 1, ?)", (node_id, ws, "backup-node", now))
            t.x("INSERT INTO tasks (task_id, workspace_id, node_id, title, state, created_at, updated_at, "
                "idempotency_key) VALUES (?, ?, ?, ?, 'completed', ?, ?, ?)",
                (task_id, ws, node_id, "fixture", now, now, "fixture-key-01"))

        payload = b"ASGEND verified backup artifact\n"
        digest = hashlib.sha256(payload).hexdigest()
        self.artifact_id = new_id("art_")
        self.store = BlobStore(self.source_data)
        self.store.write_chunk(self.artifact_id, 0, io.BytesIO(payload), len(payload))
        self.store.commit(self.artifact_id, digest)
        with self.storage.tx() as t:
            t.x("INSERT INTO artifacts (artifact_id, task_id, workspace_id, node_id, name, mime, size, sha256, "
                "state, received, blob_key, created_at, completed_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'available', ?, ?, ?, ?)",
                (self.artifact_id, task_id, ws, node_id, "fixture.txt", "text/plain", len(payload), digest,
                 len(payload), digest, now, now))
        self.payload = payload
        self.digest = digest

    def tearDown(self):
        self.storage.close()
        self.tmp.cleanup()

    def test_backup_restore_round_trip_and_artifact_verification(self):
        manifest = backup.backup(self.storage, self.source_data, self.backup_dir)
        self.assertEqual(len(manifest["artifacts"]), 1)
        self.assertEqual(manifest["artifacts"][0]["sha256"], self.digest)
        names = backup.verify_sums(self.backup_dir)
        self.assertIn("db.sqlite3", names)
        self.assertIn("manifest.json", names)
        self.assertIn("artifacts.tar.gz", names)
        self.assertEqual(backup.verify_artifacts(self.storage, self.source_data)["problems"], [])

        restored_manifest = backup.restore("sqlite:///" + self.target_db,
                                           self.target_data, self.backup_dir)
        self.assertEqual(restored_manifest["schema_version"], manifest["schema_version"])
        restored = open_storage("sqlite:///" + self.target_db, pool_size=2)
        try:
            self.assertEqual(restored.check_schema(), self.storage.schema_version())
            self.assertEqual(backup.verify_artifacts(restored, self.target_data),
                             {"checked": 1, "problems": []})
            with restored.read() as t:
                row = t.q1("SELECT artifact_id, sha256, size FROM artifacts WHERE artifact_id = ?",
                           (self.artifact_id,))
            self.assertEqual(row["sha256"], self.digest)
            with open(self.store.blob_path(self.digest), "rb") as f:
                self.assertEqual(f.read(), self.payload)
            restored_store = BlobStore(self.target_data)
            with restored_store.open_blob(self.digest) as f:
                self.assertEqual(f.read(), self.payload)
        finally:
            restored.close()

    def test_backup_checksum_detects_tampering_before_restore(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        manifest_path = os.path.join(self.backup_dir, "manifest.json")
        with open(manifest_path, "a", encoding="utf-8") as f:
            f.write("\n")
        with self.assertRaisesRegex(backup.BackupError, "checksum mismatch: manifest.json"):
            backup.restore("sqlite:///" + self.target_db, self.target_data, self.backup_dir)
        self.assertFalse(os.path.exists(self.target_db))

    def test_database_dump_must_be_covered_by_checksum_manifest(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        sums_path = os.path.join(self.backup_dir, backup.SUMS)
        with open(sums_path, "r", encoding="utf-8") as f:
            lines = [line for line in f if not line.rstrip().endswith("  db.sqlite3")]
        with open(sums_path, "w", encoding="utf-8", newline="\n") as f:
            f.writelines(lines)
        with self.assertRaisesRegex(backup.BackupError, "db.sqlite3 not covered"):
            backup.verify_backup(self.backup_dir)

    def test_postgres_dump_must_be_covered_by_checksum_manifest(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        sqlite_dump = os.path.join(self.backup_dir, "db.sqlite3")
        pg_dump = os.path.join(self.backup_dir, "db.pgdump")
        os.replace(sqlite_dump, pg_dump)
        manifest_path = os.path.join(self.backup_dir, "manifest.json")
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        manifest["dialect"] = "postgresql"
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f)
        backup.write_sums(self.backup_dir)
        sums_path = os.path.join(self.backup_dir, backup.SUMS)
        with open(sums_path, "r", encoding="utf-8") as f:
            lines = [line for line in f if not line.rstrip().endswith("  db.pgdump")]
        with open(sums_path, "w", encoding="utf-8", newline="\n") as f:
            f.writelines(lines)
        with self.assertRaisesRegex(backup.BackupError, "db.pgdump not covered"):
            backup.verify_backup(self.backup_dir)

    def test_artifact_content_is_verified_even_if_archive_checksum_is_rewritten(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        archive_path = os.path.join(self.backup_dir, "artifacts.tar.gz")
        replacement = archive_path + ".new"
        with tarfile.open(archive_path, "r:gz") as old, tarfile.open(replacement, "w:gz") as new:
            for member in old.getmembers():
                info = tarfile.TarInfo(member.name)
                info.mode = member.mode
                info.mtime = member.mtime
                if member.isdir():
                    info.type = tarfile.DIRTYPE
                    new.addfile(info)
                    continue
                payload = old.extractfile(member).read()
                if member.name.endswith(self.digest) and payload:
                    payload = bytes([payload[0] ^ 0x01]) + payload[1:]
                info.size = len(payload)
                new.addfile(info, io.BytesIO(payload))
        os.replace(replacement, archive_path)
        backup.write_sums(self.backup_dir)  # simulate a regenerated outer checksum manifest
        with self.assertRaisesRegex(backup.BackupError, "artifact blob checksum mismatch"):
            backup.verify_backup(self.backup_dir)

    def test_sqlite_dump_integrity_is_checked_before_restore_mutation(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        with open(os.path.join(self.backup_dir, "db.sqlite3"), "wb") as f:
            f.write(b"not a sqlite database")
        backup.write_sums(self.backup_dir)  # simulate matching outer checksums after file corruption
        with self.assertRaisesRegex(backup.BackupError, "SQLite database dump is invalid"):
            backup.restore("sqlite:///" + self.target_db, self.target_data, self.backup_dir)
        self.assertFalse(os.path.exists(self.target_db), "invalid backup must not create the target DB")

    def test_restore_preflight_does_not_create_database_for_nonempty_artifact_volume(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        os.makedirs(os.path.join(self.target_data, "blobs", "sha256", "aa"), exist_ok=True)
        sentinel = os.path.join(self.target_data, "blobs", "sha256", "aa", "preserve")
        with open(sentinel, "wb") as f:
            f.write(b"keep existing data")
        with self.assertRaisesRegex(backup.BackupError, "artifact volume not empty"):
            backup.restore("sqlite:///" + self.target_db, self.target_data, self.backup_dir)
        self.assertFalse(os.path.exists(self.target_db), "restore must preflight before DB creation")
        with open(sentinel, "rb") as f:
            self.assertEqual(f.read(), b"keep existing data")

    def test_restore_rejects_symlink_entries_inside_archive(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        archive_path = os.path.join(self.backup_dir, "artifacts.tar.gz")
        replacement = archive_path + ".new"
        with tarfile.open(replacement, "w:gz") as tar:
            link = tarfile.TarInfo("blobs/sha256/aa/bb/" + ("a" * 64))
            link.type = tarfile.SYMTYPE
            link.linkname = "../../../../outside"
            tar.addfile(link)
        os.replace(replacement, archive_path)
        backup.write_sums(self.backup_dir)
        with self.assertRaisesRegex(backup.BackupError, "unexpected entry in artifacts.tar.gz"):
            backup.restore("sqlite:///" + self.target_db, self.target_data, self.backup_dir)
        self.assertFalse(os.path.exists(self.target_db))
        self.assertFalse(os.path.exists(self.target_data))

    def test_restore_rejects_symlink_blob_tree_before_mutation(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        outside = os.path.join(self.tmp.name, "outside")
        os.makedirs(outside)
        os.makedirs(self.target_data, exist_ok=True)
        os.symlink(outside, os.path.join(self.target_data, "blobs"))
        with self.assertRaisesRegex(backup.BackupError, "blob directory must not be a symlink"):
            backup.restore("sqlite:///" + self.target_db, self.target_data, self.backup_dir, force=True)
        self.assertFalse(os.path.exists(self.target_db))
        self.assertEqual(os.listdir(outside), [])

    def test_restore_rejects_dialect_mismatch(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        with self.assertRaisesRegex(backup.BackupError, "dialect does not match"):
            backup.restore("postgresql://user:pass@localhost/db", self.target_data, self.backup_dir)
        self.assertFalse(os.path.exists(self.target_db))

    def test_restore_refuses_to_overwrite_existing_database_without_force(self):
        backup.backup(self.storage, self.source_data, self.backup_dir)
        with open(self.target_db, "wb") as f:
            f.write(b"do not replace")
        with self.assertRaisesRegex(backup.BackupError, "target database exists"):
            backup.restore("sqlite:///" + self.target_db, self.target_data, self.backup_dir)
        with open(self.target_db, "rb") as f:
            self.assertEqual(f.read(), b"do not replace")


if __name__ == "__main__":
    unittest.main()
