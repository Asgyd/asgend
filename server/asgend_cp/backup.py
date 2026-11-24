"""Backup / restore of the control-plane state (no external processes).

A backup directory contains:

* ``db.sqlite3`` (SQLite backend, consistent online copy via the backup API) or
  ``db.pgdump`` (PostgreSQL; produced by ``scripts/backup.sh`` with pg_dump);
* ``artifacts.tar.gz`` -- the content-addressed blob tree;
* ``manifest.json`` -- schema version, dialect, artifact inventory;
* ``SHA256SUMS`` -- ``sha256sum -c`` compatible checksums of all files above.

Restore verifies SHA256SUMS before touching anything, then re-verifies every
available artifact blob against the SHA-256 recorded in the database.
"""

from __future__ import annotations

import hashlib
from contextlib import closing
import json
import os
import re
import shutil
import sqlite3
import tarfile
import tempfile
import time

from pathlib import Path

from .db import SQLiteStorage, Storage, open_storage

SUMS = "SHA256SUMS"
_MANIFEST_MAX_BYTES = 16 * 1024 * 1024
_MAX_ARCHIVE_MEMBERS = 100_000
_BLOB_MEMBER_RE = re.compile(r"^blobs/sha256/([0-9a-f]{2})/([0-9a-f]{2})/([0-9a-f]{64})$")
_BLOB_DIR_RE = re.compile(r"^blobs(/sha256(/[0-9a-f]{2}(/[0-9a-f]{2})?)?)?$")
_HEX_RE = re.compile(r"^[0-9a-f]{64}$")
_ARTIFACT_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,128}$")


class BackupError(Exception):
    pass


def _sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for buf in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(buf)
    return h.hexdigest()


def write_sums(out_dir: str) -> None:
    lines = []
    for name in sorted(os.listdir(out_dir)):
        p = os.path.join(out_dir, name)
        if name == SUMS or os.path.isdir(p):
            continue
        if os.path.islink(p) or not os.path.isfile(p):
            raise BackupError(f"unsupported backup entry: {name}")
        if not re.fullmatch(r"[A-Za-z0-9._-]+", name):
            raise BackupError(f"invalid backup filename: {name}")
        lines.append(f"{_sha256_file(p)}  {name}\n")
    sums_path = os.path.join(out_dir, SUMS)
    if os.path.islink(sums_path):
        raise BackupError("SHA256SUMS must not be a symlink")
    with open(sums_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.writelines(lines)
    try:
        os.chmod(sums_path, 0o600)
    except OSError:
        pass


def _load_manifest(in_dir: str) -> dict:
    path = os.path.join(in_dir, "manifest.json")
    if os.path.islink(path) or not os.path.isfile(path):
        raise BackupError("manifest.json missing or not a regular file")
    if os.path.getsize(path) > _MANIFEST_MAX_BYTES:
        raise BackupError("manifest.json exceeds size limit")
    try:
        with open(path, "r", encoding="utf-8") as fh:
            manifest = json.load(fh)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BackupError("manifest.json is not valid JSON") from exc
    if not isinstance(manifest, dict) or manifest.get("format") != 1:
        raise BackupError("unsupported backup manifest format")
    if manifest.get("dialect") not in ("sqlite", "postgresql"):
        raise BackupError("manifest dialect must be sqlite or postgresql")
    version = manifest.get("schema_version")
    if isinstance(version, bool) or not isinstance(version, int) or version < 1:
        raise BackupError("manifest schema_version is invalid")
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, list):
        raise BackupError("manifest artifacts must be a list")
    seen_ids = set()
    digest_sizes = {}
    for item in artifacts:
        if not isinstance(item, dict):
            raise BackupError("manifest artifact entry is invalid")
        artifact_id = item.get("artifact_id")
        digest = item.get("sha256")
        size = item.get("size")
        if not isinstance(artifact_id, str) or not _ARTIFACT_ID_RE.fullmatch(artifact_id):
            raise BackupError("manifest artifact_id is invalid")
        if artifact_id in seen_ids:
            raise BackupError("manifest contains duplicate artifact_id")
        seen_ids.add(artifact_id)
        if not isinstance(digest, str) or not _HEX_RE.fullmatch(digest):
            raise BackupError("manifest artifact sha256 is invalid")
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise BackupError("manifest artifact size is invalid")
        previous_size = digest_sizes.setdefault(digest, size)
        if previous_size != size:
            raise BackupError("manifest has conflicting sizes for the same blob")
    return manifest


def verify_sums(in_dir: str) -> list:
    """Validate checksum coverage, files, and the backup manifest metadata."""
    path = os.path.join(in_dir, SUMS)
    if os.path.islink(path) or not os.path.isfile(path):
        raise BackupError("SHA256SUMS missing or not a regular file")
    names = []
    seen_names = set()
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if not line:
                    continue
                m = re.match(r"^([0-9a-f]{64})  ([A-Za-z0-9._-]+)$", line)
                if not m:
                    raise BackupError("malformed SHA256SUMS line")
                digest, name = m.groups()
                if name in seen_names:
                    raise BackupError(f"duplicate checksum entry: {name}")
                seen_names.add(name)
                p = os.path.join(in_dir, name)
                if os.path.islink(p) or not os.path.isfile(p):
                    raise BackupError(f"backup file missing or not regular: {name}")
                if _sha256_file(p) != digest:
                    raise BackupError(f"checksum mismatch: {name}")
                names.append(name)
    except UnicodeError as exc:
        raise BackupError("SHA256SUMS is not valid UTF-8") from exc
    for required in ("manifest.json", "artifacts.tar.gz"):
        if required not in seen_names:
            raise BackupError(f"{required} not covered by SHA256SUMS")
    manifest = _load_manifest(in_dir)
    db_file = "db.sqlite3" if manifest["dialect"] == "sqlite" else "db.pgdump"
    other_db_file = "db.pgdump" if db_file == "db.sqlite3" else "db.sqlite3"
    if db_file not in seen_names:
        raise BackupError(f"{db_file} not covered by SHA256SUMS")
    if other_db_file in seen_names or os.path.lexists(os.path.join(in_dir, other_db_file)):
        raise BackupError("backup contains conflicting database dump files")
    return names


def _safe_members(tar: tarfile.TarFile):
    members = tar.getmembers()
    if len(members) > _MAX_ARCHIVE_MEMBERS:
        raise BackupError("artifacts.tar.gz contains too many entries")
    seen = set()
    for member in members:
        name = member.name.rstrip("/")
        if name in seen:
            raise BackupError("duplicate entry in artifacts.tar.gz")
        seen.add(name)
        if member.isdir() and _BLOB_DIR_RE.fullmatch(name):
            yield member
        elif member.isfile() and _BLOB_MEMBER_RE.fullmatch(name):
            match = _BLOB_MEMBER_RE.fullmatch(name)
            digest = match.group(3)
            if name != f"blobs/sha256/{digest[:2]}/{digest[2:4]}/{digest}":
                raise BackupError("artifact blob path does not match its digest")
            yield member
        else:
            raise BackupError("unexpected entry in artifacts.tar.gz")


def _verify_artifact_archive(in_dir: str, manifest: dict) -> None:
    archive_path = os.path.join(in_dir, "artifacts.tar.gz")
    expected = {item["sha256"]: item["size"] for item in manifest["artifacts"]}
    found = {}
    try:
        with tarfile.open(archive_path, "r:gz") as tar:
            members = list(_safe_members(tar))
            for member in members:
                match = _BLOB_MEMBER_RE.fullmatch(member.name.rstrip("/"))
                if not match:
                    continue
                digest = match.group(3)
                if digest in found:
                    raise BackupError("duplicate blob digest in artifacts.tar.gz")
                if digest in expected and member.size != expected[digest]:
                    raise BackupError("artifact blob size differs from manifest")
                stream = tar.extractfile(member)
                if stream is None:
                    raise BackupError("cannot read artifact blob from archive")
                h = hashlib.sha256()
                count = 0
                with stream:
                    for buf in iter(lambda: stream.read(1024 * 1024), b""):
                        h.update(buf)
                        count += len(buf)
                if count != member.size or h.hexdigest() != digest:
                    raise BackupError("artifact blob checksum mismatch")
                found[digest] = count
    except BackupError:
        raise
    except (OSError, tarfile.TarError, EOFError) as exc:
        raise BackupError("artifacts.tar.gz is invalid or truncated") from exc
    missing = sorted(set(expected) - set(found))
    if missing:
        raise BackupError("artifact blob referenced by manifest is missing from archive")


def _verify_sqlite_dump(in_dir: str, manifest: dict) -> None:
    """Check the SQLite file before it can replace a target database."""
    path = os.path.join(in_dir, "db.sqlite3")
    uri = Path(path).resolve().as_uri() + "?mode=ro"
    try:
        with closing(sqlite3.connect(uri, uri=True)) as conn:
            integrity = conn.execute("PRAGMA integrity_check").fetchone()
            if not integrity or integrity[0] != "ok":
                raise BackupError("SQLite database integrity check failed")
            version_row = conn.execute("SELECT MAX(version) FROM schema_migrations").fetchone()
            if not version_row or version_row[0] != manifest["schema_version"]:
                raise BackupError("SQLite schema version differs from manifest")
            rows = conn.execute(
                "SELECT artifact_id, sha256, size FROM artifacts WHERE state = 'available' ORDER BY artifact_id"
            ).fetchall()
    except BackupError:
        raise
    except (sqlite3.DatabaseError, OSError) as exc:
        raise BackupError("SQLite database dump is invalid") from exc
    expected = sorted(
        ((item["artifact_id"], item["sha256"], item["size"]) for item in manifest["artifacts"]),
        key=lambda row: row[0],
    )
    actual = [(row[0], row[1], int(row[2])) for row in rows]
    if actual != expected:
        raise BackupError("SQLite artifacts table differs from manifest")


def verify_backup(in_dir: str) -> dict:
    """Fully verify checksums, manifest, database dump and content-addressed blobs."""
    verify_sums(in_dir)
    manifest = _load_manifest(in_dir)
    _verify_artifact_archive(in_dir, manifest)
    if manifest["dialect"] == "sqlite":
        _verify_sqlite_dump(in_dir, manifest)
    return manifest

def backup(storage: Storage, data_dir: str, out_dir: str) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    if isinstance(storage, SQLiteStorage):
        dest = os.path.join(out_dir, "db.sqlite3")
        if os.path.exists(dest):
            os.remove(dest)
        storage.backup_to(dest)
    elif not os.path.exists(os.path.join(out_dir, "db.pgdump")):
        raise BackupError("PostgreSQL backend: run pg_dump into db.pgdump first (scripts/backup.sh)")
    blobs_root = os.path.join(os.path.abspath(data_dir), "blobs")
    tar_path = os.path.join(out_dir, "artifacts.tar.gz")
    with tarfile.open(tar_path, "w:gz") as tar:
        if os.path.isdir(blobs_root):
            tar.add(blobs_root, arcname="blobs")
    with storage.read() as t:
        arts = t.q("SELECT artifact_id, sha256, size FROM artifacts WHERE state = 'available' "
                   "ORDER BY artifact_id")
    manifest = {
        "format": 1,
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "dialect": storage.dialect,
        "schema_version": storage.schema_version(),
        "artifacts": [{"artifact_id": a["artifact_id"], "sha256": a["sha256"], "size": int(a["size"])}
                      for a in arts],
    }
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, indent=1)
    write_sums(out_dir)
    # Do not report success unless the completed backup can pass the same
    # verification path used before restore.
    verify_backup(out_dir)
    return manifest


def _ensure_safe_artifact_tree(data_dir: str) -> None:
    """Reject symlinks in the extraction tree so force-restore cannot follow them."""
    root = os.path.abspath(data_dir)
    if os.path.islink(root):
        raise BackupError("restore data directory must not be a symlink")
    blobs_root = os.path.join(root, "blobs")
    if os.path.islink(blobs_root):
        raise BackupError("artifact blob directory must not be a symlink")
    if not os.path.isdir(blobs_root):
        return
    for current, dirs, files in os.walk(blobs_root, followlinks=False):
        for name in dirs + files:
            if os.path.islink(os.path.join(current, name)):
                raise BackupError("artifact tree contains a symlink")


def _merge_artifact_archive(data_dir: str, in_dir: str, manifest: dict) -> None:
    """Extract to a private staging directory, verify again, then atomically promote blobs."""
    root = os.path.abspath(data_dir)
    archive_path = os.path.join(in_dir, "artifacts.tar.gz")
    expected = {item["sha256"]: item["size"] for item in manifest["artifacts"]}
    os.makedirs(root, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".asgend-restore-", dir=root) as temp_dir:
        staging = os.path.join(temp_dir, "root")
        os.makedirs(staging)
        try:
            with tarfile.open(archive_path, "r:gz") as tar:
                members = list(_safe_members(tar))
                tar.extractall(staging, members=members, filter="data")
        except BackupError:
            raise
        except (OSError, tarfile.TarError, EOFError) as exc:
            raise BackupError("artifacts.tar.gz could not be staged") from exc

        staged_files = {}
        # Verify extracted bytes and preflight every destination before promoting anything.
        for member in members:
            match = _BLOB_MEMBER_RE.fullmatch(member.name.rstrip("/"))
            if not match:
                continue
            digest = match.group(3)
            source = os.path.join(staging, member.name)
            if os.path.islink(source) or not os.path.isfile(source):
                raise BackupError("staged artifact is not a regular file")
            if digest in expected and os.path.getsize(source) != expected[digest]:
                raise BackupError("artifact blob size differs from manifest")
            if _sha256_file(source) != digest:
                raise BackupError("artifact blob checksum mismatch after staging")
            staged_files[digest] = source

        missing = sorted(set(expected) - set(staged_files))
        if missing:
            raise BackupError("artifact blob referenced by manifest is missing from staged archive")

        destinations = {}
        for digest, source in staged_files.items():
            dest = os.path.join(root, "blobs", "sha256", digest[:2], digest[2:4], digest)
            parent = os.path.dirname(dest)
            current = root
            relative_parent = os.path.relpath(parent, root)
            for part in relative_parent.split(os.sep):
                if part == ".":
                    continue
                current = os.path.join(current, part)
                if os.path.islink(current):
                    raise BackupError("artifact destination contains a symlink")
                if os.path.exists(current) and not os.path.isdir(current):
                    raise BackupError("artifact destination path is not a directory")
            if os.path.islink(dest) or (os.path.exists(dest) and not os.path.isfile(dest)):
                raise BackupError("artifact destination is not a regular file")
            destinations[digest] = dest

        for digest, source in staged_files.items():
            dest = destinations[digest]
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if os.path.isfile(dest) and not os.path.islink(dest):
                if os.path.getsize(dest) == os.path.getsize(source) and _sha256_file(dest) == digest:
                    continue
            os.replace(source, dest)


def restore(db_url: str, data_dir: str, in_dir: str, force: bool = False, password: str = "") -> dict:
    # Finish all backup validation and target preflight before changing the DB or blob volume.
    manifest = verify_backup(in_dir)
    is_sqlite_target = db_url.startswith("sqlite")
    expected_dialect = "sqlite" if is_sqlite_target else "postgresql"
    if manifest["dialect"] != expected_dialect:
        raise BackupError("backup database dialect does not match restore target")

    db_path = None
    if is_sqlite_target:
        db_path = (db_url.split("sqlite:///", 1)[1]
                   if db_url.startswith("sqlite:///") else db_url[9:])
        if os.path.exists(db_path) and not force:
            raise BackupError("target database exists (use --force to overwrite)")

    _ensure_safe_artifact_tree(data_dir)
    blobs_root = os.path.join(os.path.abspath(data_dir), "blobs")
    if os.path.isdir(blobs_root) and os.listdir(blobs_root) and not force:
        raise BackupError("artifact volume not empty (use --force to merge)")

    # Promote blobs first. A failed DB write then leaves only content-addressed
    # extra blobs, never a DB whose expected artifacts were not restored.
    _merge_artifact_archive(data_dir, in_dir, manifest)

    if is_sqlite_target:
        assert db_path is not None
        db_parent = os.path.dirname(os.path.abspath(db_path))
        os.makedirs(db_parent, exist_ok=True)
        fd, staged_db = tempfile.mkstemp(prefix=".asgend-db-restore-", dir=db_parent)
        os.close(fd)
        try:
            shutil.copyfile(os.path.join(in_dir, "db.sqlite3"), staged_db)
            # Re-validate the staged file to guard against a changed source backup.
            check_uri = Path(staged_db).resolve().as_uri() + "?mode=ro"
            with closing(sqlite3.connect(check_uri, uri=True)) as conn:
                integrity = conn.execute("PRAGMA integrity_check").fetchone()
                if not integrity or integrity[0] != "ok":
                    raise BackupError("staged SQLite database failed integrity check")
            for suffix in ("-wal", "-shm"):
                sidecar = db_path + suffix
                if os.path.exists(sidecar) or os.path.islink(sidecar):
                    os.remove(sidecar)
            os.replace(staged_db, db_path)
            staged_db = ""
        finally:
            if staged_db and os.path.exists(staged_db):
                os.remove(staged_db)

    # PostgreSQL: scripts/restore.sh performs pg_restore after this verified preflight.
    return manifest

def verify_artifacts(storage: Storage, data_dir: str) -> dict:
    """Re-hash every available artifact blob against the DB."""
    from .blobstore import BlobStore
    store = BlobStore(data_dir)
    problems = []
    with storage.read() as t:
        arts = t.q("SELECT artifact_id, sha256, size, blob_key FROM artifacts WHERE state = 'available'")
    for a in arts:
        key = a["blob_key"] or a["sha256"]
        if not store.blob_exists(key):
            problems.append({"artifact_id": a["artifact_id"], "problem": "missing"})
        elif not store.verify_blob(key) or os.path.getsize(store.blob_path(key)) != int(a["size"]):
            problems.append({"artifact_id": a["artifact_id"], "problem": "checksum_mismatch"})
    return {"checked": len(arts), "problems": problems}


def open_for(db_url: str, password: str = "") -> Storage:
    return open_storage(db_url, password, 2)
