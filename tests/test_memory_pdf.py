#!/usr/bin/env python3
"""Integration checks for the built-in, bounded PDF text extractor."""
from __future__ import annotations

import hashlib
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cli: str, *args: str) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run([cli, *args], text=True, capture_output=True, timeout=30)
    if proc.returncode != 0:
        raise AssertionError(f"command failed: {proc.args}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    return proc


def make_pdf(text: str, include_trailer: bool = True) -> bytes:
    stream = f"BT /F1 12 Tf 72 720 Td ({text}) Tj ET\n".encode("ascii")
    parts = [
        b"%PDF-1.4\n",
        b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
        b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n",
        b"3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n",
        b"4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n",
        f"5 0 obj\n<< /Length {len(stream)} >>\nstream\n".encode("ascii"),
        stream,
        b"endstream\nendobj\n",
    ]
    if include_trailer:
        parts.extend([b"trailer\n<< /Root 1 0 R >>\n", b"%%EOF\n"])
    return b"".join(parts)


def db_at(root: Path) -> sqlite3.Connection:
    return sqlite3.connect(root / ".asgend_mem" / "index.sqlite3")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_pdf.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="asgend-memory-pdf-") as temporary:
        root = Path(temporary) / "workspace"
        root.mkdir()
        valid_bytes = make_pdf("Hello PDF memory")
        partial_stream = b"BT /F1 12 Tf 72 720 Td (Recoverable partial PDF text) Tj ET\n"
        partial_bytes = (b"%PDF-1.4\n99 0 obj\n<< /Length " + str(len(partial_stream)).encode("ascii") +
                         b" >>\nstream\n" + partial_stream + b"endstream\nendobj\n%%EOF\n")
        valid = root / "valid.pdf"
        partial = root / "partial.pdf"
        valid.write_bytes(valid_bytes)
        partial.write_bytes(partial_bytes)
        original = {valid.name: valid.read_bytes(), partial.name: partial.read_bytes()}

        run(cli, "scan", str(root))
        extracted = run(cli, "extract", str(root))
        assert "extracted=2" in extracted.stdout, extracted.stdout

        with db_at(root) as db:
            row = db.execute(
                "SELECT c.content_text,c.extract_status,c.metadata_status,c.extractor_id,c.source_sha256,c.source_bytes "
                "FROM file_contents c JOIN files f USING(file_id) WHERE f.relative_path='valid.pdf'"
            ).fetchone()
            assert row is not None, "valid PDF extraction row missing"
            content, status, metadata_status, extractor, digest, source_bytes = row
            assert "Hello PDF memory" in content, (content, status)
            assert status == "extracted", status
            assert metadata_status == "not_applicable", metadata_status
            assert extractor == "pdf-text-v1", extractor
            assert digest == hashlib.sha256(valid_bytes).hexdigest(), digest
            assert source_bytes == len(valid_bytes), (source_bytes, len(valid_bytes))

            partial_row = db.execute(
                "SELECT content_text,extract_status,metadata_status,extractor_id,c.error_text "
                "FROM file_contents c JOIN files f USING(file_id) WHERE f.relative_path='partial.pdf'"
            ).fetchone()
            assert partial_row is not None, "partial PDF extraction row missing"
            partial_content, partial_status, partial_metadata, partial_extractor, reason = partial_row
            assert "Recoverable partial PDF text" in partial_content, (partial_content, partial_status)
            assert partial_status == "parsed_partial", partial_status
            assert partial_metadata == "parsed_partial", partial_metadata
            assert partial_extractor == "pdf-text-v1", partial_extractor
            assert reason, "partial extraction should explain why parsing was incomplete"

            # Partial text participates in normal retrieval-related inventory queries.
            assert db.execute(
                "SELECT COUNT(*) FROM file_contents WHERE extract_status IN ('extracted','truncated','parsed_partial') "
                "AND content_text IS NOT NULL"
            ).fetchone()[0] == 2
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

        # Re-running extraction is idempotent for the same PDF extractor version.
        second = run(cli, "extract", str(root))
        assert "skipped=2" in second.stdout, second.stdout
        assert valid.read_bytes() == original[valid.name]
        assert partial.read_bytes() == original[partial.name]

        # A damaged/encrypted PDF is reported as unsupported rather than indexed as gibberish.
        encrypted = root / "encrypted.pdf"
        encrypted.write_bytes(b"%PDF-1.4\n1 0 obj << /Type /Catalog >> endobj\n/Encrypt 9 0 R\n")
        run(cli, "scan", str(root))
        run(cli, "extract", str(root))
        with db_at(root) as db:
            assert db.execute(
                "SELECT extract_status FROM file_contents c JOIN files f USING(file_id) "
                "WHERE f.relative_path='encrypted.pdf'"
            ).fetchone()[0] == "unsupported"
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

    print("memory PDF smoke: ok")


if __name__ == "__main__":
    main()
