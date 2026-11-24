#!/usr/bin/env python3
"""Smoke/security tests for built-in DOCX/XLSX/PPTX text extraction."""
from __future__ import annotations

import hashlib
import io
import sqlite3
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def run(cli: str, *args: str) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run([cli, *args], text=True, capture_output=True, timeout=30)
    if proc.returncode != 0:
        raise AssertionError(f"command failed: {proc.args}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    return proc


def make_zip(parts: dict[str, bytes], compression: int = zipfile.ZIP_DEFLATED) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=compression) as archive:
        for name, data in parts.items():
            archive.writestr(name, data)
    return buf.getvalue()


def db_at(root: Path) -> sqlite3.Connection:
    return sqlite3.connect(root / ".asgend_mem" / "index.sqlite3")


def corrupt_central_crc(archive_bytes: bytes, entry_name: str) -> bytes:
    data = bytearray(archive_bytes)
    position = 0
    signature = b"PK\x01\x02"
    while True:
        position = data.find(signature, position)
        if position < 0:
            raise AssertionError(f"central directory entry not found: {entry_name}")
        name_len = int.from_bytes(data[position + 28:position + 30], "little")
        extra_len = int.from_bytes(data[position + 30:position + 32], "little")
        comment_len = int.from_bytes(data[position + 32:position + 34], "little")
        name = bytes(data[position + 46:position + 46 + name_len]).decode("utf-8")
        if name == entry_name:
            data[position + 16] ^= 0x01
            return bytes(data)
        position += 46 + name_len + extra_len + comment_len


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_ooxml.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]

    content_types = b'<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"></Types>'
    docx = make_zip({
        "[Content_Types].xml": content_types,
        "word/document.xml": (b'<?xml version="1.0"?><w:document xmlns:w="urn:w"><w:body>'
                               b'<w:p><w:r><w:t>Hello &amp; welcome</w:t></w:r></w:p>'
                               b'<w:p><w:r><w:t>Second paragraph &#x1F680;</w:t></w:r></w:p>'
                               b'</w:body></w:document>'),
        "word/header1.xml": b'<w:hdr xmlns:w="urn:w"><w:p><w:r><w:t>Header phrase</w:t></w:r></w:p></w:hdr>',
    })
    xlsx = make_zip({
        "[Content_Types].xml": content_types,
        "xl/workbook.xml": b'<workbook xmlns="urn:x"><sheets/></workbook>',
        "xl/sharedStrings.xml": (b'<sst xmlns="urn:x" count="2"><si><t>Shared Alpha</t></si>'
                                  b'<si><r><t>Shared </t></r><r><t>Beta</t></r></si></sst>'),
        "xl/worksheets/sheet1.xml": (b'<worksheet xmlns="urn:x"><sheetData><row r="1">'
                                      b'<c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c>'
                                      b'</row><row r="2"><c r="A2"><v>123.5</v></c>'
                                      b'<c r="B2" t="inlineStr"><is><t>Inline Value</t></is></c>'
                                      b'</row></sheetData></worksheet>'),
    })
    partial_docx = make_zip({
        "[Content_Types].xml": content_types,
        "word/document.xml": b'<w:document><w:body><w:p><w:t>Recoverable Office text',
    })
    bad_crc_docx = corrupt_central_crc(make_zip({
        "[Content_Types].xml": content_types,
        "word/document.xml": b'<w:document><w:body><w:p><w:t>CRC must be checked</w:t></w:p></w:body></w:document>',
    }), "word/document.xml")
    pptx = make_zip({
        "[Content_Types].xml": content_types,
        "ppt/presentation.xml": b'<p:presentation xmlns:p="urn:p"><p:sldIdLst/></p:presentation>',
        "ppt/slides/slide1.xml": (b'<p:sld xmlns:p="urn:p" xmlns:a="urn:a"><p:spTree>'
                                  b'<p:sp><p:txBody><a:p><a:r><a:t>Slide One</a:t></a:r></a:p>'
                                  b'<a:p><a:r><a:t>Key point</a:t></a:r></a:p></p:txBody></p:sp>'
                                  b'</p:spTree></p:sld>'),
        "ppt/slides/slide2.xml": b'<p:sld xmlns:p="urn:p" xmlns:a="urn:a"><a:t>Slide Two</a:t></p:sld>',
    })

    with tempfile.TemporaryDirectory(prefix="asgend-memory-ooxml-") as temporary:
        root = Path(temporary) / "workspace"
        root.mkdir()
        sources = {"sample.docx": docx, "sample.xlsx": xlsx, "sample.pptx": pptx,
                   "partial.docx": partial_docx, "bad-crc.docx": bad_crc_docx,
                   "not-office.docx": b"not a zip package"}
        for name, content in sources.items():
            (root / name).write_bytes(content)

        run(cli, "scan", str(root))
        output = run(cli, "extract", str(root)).stdout
        assert "extracted=4" in output, output
        with db_at(root) as db:
            rows = {row[0]: row[1:] for row in db.execute(
                "SELECT f.relative_path,c.content_text,c.extract_status,c.extractor_id,c.source_sha256,c.error_text "
                "FROM file_contents c JOIN files f USING(file_id)"
            )}
            doc_text, doc_status, doc_extractor, doc_hash, _ = rows["sample.docx"]
            assert "Hello & welcome" in doc_text, doc_text
            assert "Second paragraph" in doc_text and "Header phrase" in doc_text, doc_text
            assert doc_status == "extracted" and doc_extractor == "ooxml-text-v1", (doc_status, doc_extractor)
            assert doc_hash == hashlib.sha256(docx).hexdigest()

            sheet_text, sheet_status, sheet_extractor, _, _ = rows["sample.xlsx"]
            for expected in ("Shared Alpha", "Shared Beta", "123.5", "Inline Value"):
                assert expected in sheet_text, (expected, sheet_text)
            assert sheet_status == "extracted" and sheet_extractor == "ooxml-text-v1", (sheet_status, sheet_extractor)

            slide_text, slide_status, slide_extractor, _, _ = rows["sample.pptx"]
            assert "Slide One" in slide_text and "Key point" in slide_text and "Slide Two" in slide_text, slide_text
            assert slide_status == "extracted" and slide_extractor == "ooxml-text-v1", (slide_status, slide_extractor)
            partial_text, partial_status, partial_extractor, _, partial_reason = rows["partial.docx"]
            assert "Recoverable Office text" in partial_text, partial_text
            assert partial_status == "parsed_partial" and partial_extractor == "ooxml-text-v1", (partial_status, partial_extractor)
            assert partial_reason, partial_reason
            bad_status = rows["not-office.docx"][1]
            assert bad_status == "unsupported", bad_status
            crc_status, crc_reason = rows["bad-crc.docx"][1], rows["bad-crc.docx"][4]
            assert crc_status == "unsupported", crc_status
            assert crc_reason and "corrupt" in crc_reason.lower(), crc_reason
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

        second = run(cli, "extract", str(root)).stdout
        assert "skipped=6" in second, second
        for name, content in sources.items():
            assert (root / name).read_bytes() == content, name

        # A decompression bomb in a relevant XML member is skipped safely and marked partial/unsupported,
        # rather than being expanded without a bound. A 16 MiB uncompressed part exceeds the per-part cap.
        bomb_xml = b"<w:document><w:body><w:p><w:r><w:t>" + (b"A" * (16 * 1024 * 1024)) + b"</w:t></w:r></w:p></w:body></w:document>"
        bomb = make_zip({"[Content_Types].xml": content_types, "word/document.xml": bomb_xml})
        (root / "large.docx").write_bytes(bomb)
        run(cli, "scan", str(root))
        run(cli, "extract", str(root))
        with db_at(root) as db:
            row = db.execute(
                "SELECT c.extract_status,c.error_text FROM file_contents c JOIN files f USING(file_id) "
                "WHERE f.relative_path='large.docx'"
            ).fetchone()
            assert row is not None and row[0] in ("parsed_partial", "unsupported"), row
            assert row[1], row
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

    print("memory OOXML smoke: ok")


if __name__ == "__main__":
    main()
