#!/usr/bin/env python3
"""End-to-end tests for the C-backed ASGEND M1 memory index CLI."""
from __future__ import annotations

import hashlib
import json
import os
import pwd
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cli: str, *args: str, ok: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run([cli, *args], text=True, capture_output=True, timeout=20)
    if ok and proc.returncode != 0:
        raise AssertionError(f"command failed: {proc.args}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    if not ok and proc.returncode == 0:
        raise AssertionError(f"command unexpectedly succeeded: {proc.args}\nstdout={proc.stdout}")
    return proc


def db_at(root: Path) -> sqlite3.Connection:
    db = root / ".asgend_mem" / "index.sqlite3"
    assert db.is_file(), f"missing memory database: {db}"
    return sqlite3.connect(db)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_index.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="asgend-memory-test-") as temporary:
        root = Path(temporary) / "workspace"
        root.mkdir()
        (root / "docs").mkdir()
        (root / "src").mkdir()
        intro = root / "docs" / "intro.md"
        intro.write_text("ASGEND memory\n", encoding="utf-8")
        code = root / "src" / "main.c"
        code.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        old = root / "old.txt"
        old_content = "uniquely rename this file\n"
        old.write_text(old_content, encoding="utf-8")
        binary = root / "unknown.bin"
        binary.write_bytes(b"\x00\x01\x02\xff")
        external = Path(temporary) / "outside-secret.txt"
        external.write_text("must not be followed\n", encoding="utf-8")
        try:
            (root / "external-link").symlink_to(external)
            symlink_supported = True
        except (OSError, NotImplementedError):
            symlink_supported = False

        source_before = {str(p.relative_to(root)): p.read_bytes() for p in root.rglob("*") if p.is_file() and ".asgend_mem" not in p.parts}
        first = run(cli, "scan", str(root))
        assert "status=complete" in first.stdout, first.stdout
        assert "added=4" in first.stdout, first.stdout
        if symlink_supported:
            assert "skipped_symlinks=1" in first.stdout, first.stdout
        with db_at(root) as db:
            paths = {row[0] for row in db.execute("SELECT relative_path FROM files WHERE deleted_at IS NULL")}
            expected = {"docs/intro.md", "src/main.c", "old.txt", "unknown.bin"}
            assert paths == expected, (paths, expected)
            rows = dict((path, digest) for path, digest in db.execute("SELECT relative_path,sha256 FROM files"))
            assert rows["old.txt"] == hashlib.sha256(old_content.encode()).hexdigest()
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
            old_id = db.execute("SELECT file_id FROM files WHERE relative_path='old.txt'").fetchone()[0]
            assert db.execute("SELECT status FROM files WHERE relative_path='unknown.bin'").fetchone()[0] == "indexed"

        second = run(cli, "scan", str(root))
        assert "added=0" in second.stdout and "modified=0" in second.stdout, second.stdout
        assert "deleted=0" in second.stdout and "renamed=0" in second.stdout, second.stdout

        # A unique content move should preserve file_id and produce a rename event.
        old.rename(root / "renamed.txt")
        intro.write_text("ASGEND MEMORY\n", encoding="utf-8")
        (root / "new.txt").write_text("new content\n", encoding="utf-8")
        code.unlink()
        changed = run(cli, "scan", str(root))
        assert "modified=1" in changed.stdout, changed.stdout
        assert "deleted=1" in changed.stdout, changed.stdout
        assert "renamed=1" in changed.stdout, changed.stdout
        assert "added=1" in changed.stdout, changed.stdout
        with db_at(root) as db:
            new_id = db.execute("SELECT file_id FROM files WHERE relative_path='renamed.txt'").fetchone()[0]
            assert new_id == old_id, (new_id, old_id)
            assert db.execute("SELECT COUNT(*) FROM file_events WHERE event_type='renamed' AND old_path='old.txt' AND new_path='renamed.txt'").fetchone()[0] == 1
            assert db.execute("SELECT status FROM files WHERE relative_path='src/main.c'").fetchone()[0] == "deleted"

        # The index lives under .asgend_mem and never mutates source files.
        for relative, contents in source_before.items():
            if relative in ("old.txt", "src/main.c", "docs/intro.md"):
                continue  # These have intentional test mutations/removal above.
            p = root / relative
            assert p.read_bytes() == contents, relative
        assert not (root / "external-link").exists() or symlink_supported
        with db_at(root) as db:
            assert db.execute("SELECT COUNT(*) FROM files WHERE relative_path LIKE '.asgend_mem/%'").fetchone()[0] == 0

    # --verify must detect same-length content changes even if the mtime is preserved.
    with tempfile.TemporaryDirectory(prefix="asgend-memory-verify-") as temporary:
        root = Path(temporary)
        file = root / "same.txt"
        file.write_text("alpha\n", encoding="utf-8")
        run(cli, "scan", str(root))
        original = file.stat()
        file.write_text("bravo\n", encoding="utf-8")
        os.utime(file, ns=(original.st_atime_ns, original.st_mtime_ns))
        fast = run(cli, "scan", str(root))
        assert "modified=0" in fast.stdout, fast.stdout
        verified = run(cli, "verify", str(root))
        assert "modified=1" in verified.stdout, verified.stdout
        with db_at(root) as db:
            assert db.execute("SELECT sha256 FROM files WHERE relative_path='same.txt'").fetchone()[0] == hashlib.sha256(b"bravo\n").hexdigest()

    # Deletion/recreation of the same path preserves its file identity as a restore.
    with tempfile.TemporaryDirectory(prefix="asgend-memory-restore-") as temporary:
        root = Path(temporary)
        item = root / "restore-me.md"
        item.write_text("version one\n", encoding="utf-8")
        run(cli, "scan", str(root))
        with db_at(root) as db:
            original_id = db.execute("SELECT file_id FROM files WHERE relative_path='restore-me.md'").fetchone()[0]
        item.unlink()
        deleted = run(cli, "scan", str(root))
        assert "deleted=1" in deleted.stdout, deleted.stdout
        item.write_text("version two\n", encoding="utf-8")
        restored = run(cli, "scan", str(root))
        assert "restored=1" in restored.stdout, restored.stdout
        with db_at(root) as db:
            restored_id = db.execute("SELECT file_id FROM files WHERE relative_path='restore-me.md'").fetchone()[0]
            assert restored_id == original_id
            assert db.execute("SELECT COUNT(*) FROM file_events WHERE event_type='restored'").fetchone()[0] == 1

    # A traversal error is partial; unvisited files must not be marked deleted.
    # When tests run as root, drop the scanner child to the nobody account to
    # exercise a real EACCES path reliably instead of relying on chmod semantics for root.
    with tempfile.TemporaryDirectory(prefix="asgend-memory-partial-") as temporary:
        base = Path(temporary)
        root = base / "workspace"
        root.mkdir()
        locked = root / "private"
        locked.mkdir()
        (locked / "kept.txt").write_text("still exists\n", encoding="utf-8")
        (root / "visible.txt").write_text("visible\n", encoding="utf-8")
        run(cli, "scan", str(root))
        scan_uid = os.geteuid()
        scan_gid = os.getegid()
        drop_privileges = False
        if scan_uid == 0:
            try:
                account = pwd.getpwnam("nobody")
                scan_uid, scan_gid = account.pw_uid, account.pw_gid
                drop_privileges = True
            except KeyError:
                pass
        if drop_privileges:
            os.chown(base, scan_uid, scan_gid)
            os.chmod(base, 0o755)
            for path in root.rglob("*"):
                os.chown(path, scan_uid, scan_gid, follow_symlinks=False)
            os.chown(root, scan_uid, scan_gid)
            os.chmod(root, 0o755)
        os.chmod(locked, 0)
        def scan_partial() -> subprocess.CompletedProcess[str]:
            if drop_privileges:
                def demote() -> None:
                    os.setgid(scan_gid)
                    os.setuid(scan_uid)
                proc = subprocess.run([cli, "scan", str(root)], text=True, capture_output=True, timeout=20, preexec_fn=demote)
                if proc.returncode != 0:
                    raise AssertionError(f"partial scan command failed: {proc.stderr}")
                return proc
            return run(cli, "scan", str(root))
        partial = scan_partial()
        assert "status=partial" in partial.stdout, partial.stdout
        assert "walk_errors=" in partial.stdout and not partial.stdout.endswith("walk_errors=0\n"), partial.stdout
        with db_at(root) as db:
            row = db.execute("SELECT status,deleted_at FROM files WHERE relative_path='private/kept.txt'").fetchone()
            assert row and row[0] != "deleted" and row[1] is None, row

    # Size limits index oversized files without reading/copying their contents into SQLite.
    with tempfile.TemporaryDirectory(prefix="asgend-memory-limit-") as temporary:
        root = Path(temporary)
        (root / "large.dat").write_bytes(b"123456789")
        limited = run(cli, "scan", str(root), "--max-file-size", "4")
        assert "file_errors=1" in limited.stdout, limited.stdout
        with db_at(root) as db:
            row = db.execute("SELECT status,sha256 FROM files WHERE relative_path='large.dat'").fetchone()
            assert row == ("too_large", None), row

    # Existing database and path guards are explicit failures, not silent redirects.
    with tempfile.TemporaryDirectory(prefix="asgend-memory-symlink-") as temporary:
        base = Path(temporary)
        target = base / "target"
        target.mkdir()
        decoy = base / "decoy"
        decoy.mkdir()
        (decoy / "keep.txt").write_text("keep\n", encoding="utf-8")
        try:
            (target / ".asgend_mem").symlink_to(decoy, target_is_directory=True)
            proc = run(cli, "scan", str(target), ok=False)
            assert "symlink" in proc.stderr.lower(), proc.stderr
        except (OSError, NotImplementedError):
            pass

    # M2 extraction stores UTF-8 content and read-only OKF metadata in SQLite.
    with tempfile.TemporaryDirectory(prefix="asgend-memory-extract-") as temporary:
        root = Path(temporary)
        (root / "docs").mkdir()
        guide = root / "docs" / "guide.md"
        guide_bytes = (
            "---\n"
            "type: Playbook\n"
            "title: \"Deploy guide\"\n"
            "description: A guide for deployment\n"
            "tags:\n"
            "  - deployment\n"
            "  - operations\n"
            "sources:\n"
            "  - id: source-one\n"
            "    resource: https://example.test/source-one\n"
            "    title: Example source\n"
            "---\n"
            "# Guide body\n\nUse the deployment checklist.\n"
        ).encode("utf-8")
        guide.write_bytes(guide_bytes)
        plain = root / "plain.md"
        plain.write_text("# Markdown without frontmatter\n\nBody is retained.\n", encoding="utf-8")
        code = root / "src.c"
        code.write_text("int answer(void) { return 42; }\n", encoding="utf-8")
        sniffed = root / "notes.unknownext"
        sniffed.write_text("Plain UTF-8 text without a known extension.\n", encoding="utf-8")
        pdf = root / "fixture.pdf"
        pdf.write_bytes(b"%PDF-1.7\nnot parsed in this build\n")
        binary = root / "blob.unknown"
        binary.write_bytes(b"\x00\x01\xff\x00")
        source_bytes = {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*") if p.is_file()}

        run(cli, "scan", str(root))
        extracted = run(cli, "extract", str(root))
        assert "status=complete" in extracted.stdout, extracted.stdout
        assert "extracted=4" in extracted.stdout, extracted.stdout
        assert "unsupported=2" in extracted.stdout, extracted.stdout
        with db_at(root) as db:
            assert db.execute("PRAGMA user_version").fetchone()[0] == 9
            guide_id = db.execute("SELECT file_id FROM files WHERE relative_path='docs/guide.md'").fetchone()[0]
            content = db.execute("SELECT content_text,frontmatter_raw,extract_status,metadata_status,source_sha256 FROM file_contents WHERE file_id=?", (guide_id,)).fetchone()
            assert content[0] == "# Guide body\n\nUse the deployment checklist.\n", content
            assert "type: Playbook" in content[1] and "tags:" in content[1], content[1]
            assert content[2:4] == ("extracted", "parsed"), content
            assert content[4] == hashlib.sha256(guide_bytes).hexdigest(), content[4]
            metadata = {key: json.loads(value) for key, value in db.execute("SELECT metadata_key,value_json FROM file_metadata WHERE file_id=?", (guide_id,))}
            assert metadata["type"] == "Playbook", metadata
            assert metadata["title"] == "Deploy guide", metadata
            assert metadata["tags"] == ["deployment", "operations"], metadata
            assert metadata["sources"][0]["resource"] == "https://example.test/source-one", metadata
            plain_row = db.execute("SELECT c.content_text,c.metadata_status,(SELECT COUNT(*) FROM file_metadata m WHERE m.file_id=f.file_id) FROM files f JOIN file_contents c USING(file_id) WHERE f.relative_path='plain.md'").fetchone()
            assert plain_row == ("# Markdown without frontmatter\n\nBody is retained.\n", "none", 0), plain_row
            assert db.execute("SELECT extract_status FROM files f JOIN file_contents c USING(file_id) WHERE relative_path='fixture.pdf'").fetchone()[0] == "unsupported"
            assert db.execute("SELECT extract_status FROM files f JOIN file_contents c USING(file_id) WHERE relative_path='blob.unknown'").fetchone()[0] == "binary"

        repeated = run(cli, "extract", str(root))
        assert "skipped=6" in repeated.stdout and "extracted=0" in repeated.stdout, repeated.stdout
        # Updating the source only re-extracts that file and refreshes its DB metadata.
        updated_bytes = guide_bytes.replace(b"Deploy guide", b"Release guide").replace(b"deployment checklist", b"release checklist")
        guide.write_bytes(updated_bytes)
        run(cli, "scan", str(root))
        update_result = run(cli, "extract", str(root))
        assert "extracted=1" in update_result.stdout and "skipped=5" in update_result.stdout, update_result.stdout
        with db_at(root) as db:
            guide_id = db.execute("SELECT file_id FROM files WHERE relative_path='docs/guide.md'").fetchone()[0]
            metadata = {key: json.loads(value) for key, value in db.execute("SELECT metadata_key,value_json FROM file_metadata WHERE file_id=?", (guide_id,))}
            assert metadata["title"] == "Release guide", metadata
            assert db.execute("SELECT content_text FROM file_contents WHERE file_id=?", (guide_id,)).fetchone()[0].endswith("Use the release checklist.\n")
        # Extraction must not write any bytes back to indexed documents.
        for relative, before in source_bytes.items():
            if relative == "docs/guide.md":
                assert (root / relative).read_bytes() == updated_bytes
            else:
                assert (root / relative).read_bytes() == before, relative

    # Large text returns a UTF-8-boundary-safe prefix with an explicit truncated status.
    with tempfile.TemporaryDirectory(prefix="asgend-memory-truncate-") as temporary:
        root = Path(temporary)
        item = root / "unicode.txt"
        item.write_text("abcde🙂tail\n", encoding="utf-8")
        run(cli, "scan", str(root))
        truncated = run(cli, "extract", str(root), "--max-text-bytes", "7")
        assert "truncated=1" in truncated.stdout, truncated.stdout
        with db_at(root) as db:
            row = db.execute("SELECT content_text,extract_status FROM file_contents c JOIN files f USING(file_id) WHERE f.relative_path='unicode.txt'").fetchone()
            assert row == ("abcde", "truncated"), row
        # A repeated normal pass is idempotent; --retry applies a larger new text limit.
        second = run(cli, "extract", str(root))
        assert "skipped=1" in second.stdout, second.stdout
        retried = run(cli, "extract", str(root), "--max-text-bytes", "32", "--retry")
        assert "extracted=1" in retried.stdout and "truncated=0" in retried.stdout, retried.stdout
        with db_at(root) as db:
            row = db.execute("SELECT content_text,extract_status FROM file_contents c JOIN files f USING(file_id) WHERE f.relative_path='unicode.txt'").fetchone()
            assert row == ("abcde🙂tail\n", "extracted"), row

    # Opening a path that changed to a symlink after scan never follows the link.
    with tempfile.TemporaryDirectory(prefix="asgend-memory-extract-symlink-") as temporary:
        base = Path(temporary)
        root = base / "workspace"
        root.mkdir()
        external = base / "external.txt"
        external.write_text("outside secret", encoding="utf-8")
        target = root / "target.txt"
        target.write_text("indexed text", encoding="utf-8")
        run(cli, "scan", str(root))
        target.unlink()
        target.symlink_to(external)
        result = run(cli, "extract", str(root))
        assert "file_errors=1" in result.stdout, result.stdout
        assert external.read_text(encoding="utf-8") == "outside secret"
        with db_at(root) as db:
            assert db.execute("SELECT extract_status FROM files f JOIN file_contents c USING(file_id) WHERE f.relative_path='target.txt'").fetchone()[0] == "read_error"

    # A database at schema v1 is upgraded transactionally to v9 without losing file rows.
    with tempfile.TemporaryDirectory(prefix="asgend-memory-migration-") as temporary:
        root = Path(temporary)
        (root / "keep.md").write_text("keep content\n", encoding="utf-8")
        run(cli, "scan", str(root))
        with db_at(root) as db:
            kept_id = db.execute("SELECT file_id FROM files WHERE relative_path='keep.md'").fetchone()[0]
            db.execute("DROP TABLE file_metadata")
            db.execute("DROP TABLE file_contents")
            db.execute("DELETE FROM schema_migrations WHERE version > 1")
            db.execute("PRAGMA user_version=1")
            db.commit()
        run(cli, "status", str(root))
        with db_at(root) as db:
            assert db.execute("PRAGMA user_version").fetchone()[0] == 9
            assert db.execute("SELECT file_id FROM files WHERE relative_path='keep.md'").fetchone()[0] == kept_id
            assert db.execute("SELECT version FROM schema_migrations ORDER BY version").fetchall() == [(1,), (2,), (3,), (4,), (5,), (6,), (7,), (8,), (9,)]
            tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            assert {"file_contents", "file_metadata", "pipeline_jobs", "model_metadata", "descriptions", "embeddings", "memory_search_docs", "memory_fts", "file_edges", "file_chunks", "file_chunk_state", "memory_chunk_fts", "chunk_embeddings"} <= tables

    print("memory index tests passed")


if __name__ == "__main__":
    main()
