#!/usr/bin/env python3
"""Coverage for source-aware chunking, filter constraints and retrieval budget."""
from __future__ import annotations
import json
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cli: str, *args: str, ok: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run([cli, *args], text=True, capture_output=True, timeout=30)
    if ok and proc.returncode != 0:
        raise AssertionError(f"command failed: {proc.args}\ncode={proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    if not ok and proc.returncode == 0:
        raise AssertionError(f"command unexpectedly passed: {proc.args}\nstdout={proc.stdout}")
    return proc


def query(cli: str, root: Path, *args: str) -> dict:
    return json.loads(run(cli, "query", str(root), *args, "--lexical-only").stdout)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_chunk_query.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="asgend-memory-chunks-") as temp:
        root = Path(temp)
        (root / "docs").mkdir()
        header = "---\ntype: guide\ntags: [manual, search]\naudience: internal\n---\n"
        body = "# Source Coordinates\n" + "ordinary text for chunk splitting\n" * 14 + "special chunkneedle marker is here\n" + "tail text\n" * 8
        source_text = header + body
        source = root / "docs" / "manual.md"
        source.write_text(source_text, encoding="utf-8")
        (root / "docs" / "other.md").write_text("# Other\n\nchunkneedle appears in a different document.\n", encoding="utf-8")
        (root / "src").mkdir()
        (root / "src" / "internal.c").write_text("int internal_function(void) { return 42; }\n", encoding="utf-8")
        run(cli, "scan", str(root))
        run(cli, "extract", str(root))
        first = json.loads(run(cli, "chunk", str(root), "--max-chunk-tokens", "32").stdout)
        assert first["status"] == "complete" and first["chunks_created"] > 3, first
        again = json.loads(run(cli, "chunk", str(root), "--max-chunk-tokens", "32").stdout)
        assert again["chunks_created"] == 0 and again["skipped"] >= first["files_chunked"], (first, again)
        needle_byte = len(source_text[:source_text.index("chunkneedle")].encode("utf-8"))
        expected_line = source_text[:source_text.index("chunkneedle")].count("\n") + 1
        with sqlite3.connect(root / ".asgend_mem" / "index.sqlite3") as db:
            assert db.execute("PRAGMA user_version").fetchone()[0] == 9
            source_row = db.execute("SELECT body_start_byte,body_start_line FROM file_contents c JOIN files f ON f.file_id=c.file_id WHERE f.relative_path='docs/manual.md'").fetchone()
            assert source_row == (len(header.encode()), header.count("\n") + 1), source_row
            containing = db.execute("SELECT count(*) FROM file_chunks c JOIN files f ON f.file_id=c.file_id WHERE f.relative_path='docs/manual.md' AND c.start_byte<=? AND c.end_byte>?", (needle_byte, needle_byte)).fetchone()[0]
            assert containing >= 1, containing
        result = query(cli, root, "chunkneedle", "--limit", "10", "--type", "guide", "--tag", "manual", "--metadata", "audience=internal", "--path-prefix", "docs", "--context-tokens", "250", "--chunk-tokens", "32")
        docs = {item["path"]: item for item in result["results"]}
        assert "docs/manual.md" in docs, result
        assert "docs/other.md" not in docs and "src/internal.c" not in docs, result
        hit = docs["docs/manual.md"]
        assert "chunkneedle" in hit["excerpt"], hit
        assert hit["source_start_byte"] <= needle_byte < hit["source_end_byte"], (hit, needle_byte)
        assert hit["source_start_line"] <= expected_line <= hit["source_end_line"], (hit, expected_line)
        assert result["context_tokens_estimated"] <= result["context_token_budget"] == 250, result
        assert result["filters"]["type"] == "guide" and result["filters"]["tag"] == "manual", result
        no_match = query(cli, root, "chunkneedle", "--type", "not-a-guide", "--context-tokens", "250")
        assert no_match["count"] == 0, no_match
        path_filtered = query(cli, root, "chunkneedle", "--path-prefix", "src", "--context-tokens", "300")
        assert all(item["path"].startswith("src/") for item in path_filtered["results"]), path_filtered
        # Query's configured budget trims excerpts and never reports a larger estimated context.
        small = query(cli, root, "ordinary text chunkneedle", "--context-tokens", "180", "--limit", "10", "--chunk-tokens", "32")
        assert small["context_tokens_estimated"] <= 180, small
        run(cli, "query", str(root), "needle", "--metadata", "invalid", ok=False)


if __name__ == "__main__":
    main()
