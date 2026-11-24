#!/usr/bin/env python3
"""Regression tests for memory stats and safe garbage collection."""
from __future__ import annotations

import hashlib
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


def digest(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_gc.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="asgend-memory-gc-") as temp:
        root = Path(temp) / "workspace"
        root.mkdir()
        keep = root / "keep.md"
        gone = root / "gone.md"
        keep.write_text("# Keep\nCurrent content stays searchable.\n" * 5, encoding="utf-8")
        gone_text = "# Gone\nThis should remain only in historical events.\n" * 5
        gone.write_text(gone_text, encoding="utf-8")
        run(cli, "scan", str(root))
        run(cli, "extract", str(root))
        run(cli, "chunk", str(root), "--max-chunk-tokens", "16")

        database = root / ".asgend_mem" / "index.sqlite3"
        with sqlite3.connect(database) as db:
            keep_id = db.execute("SELECT file_id FROM files WHERE relative_path='keep.md'").fetchone()[0]
            gone_row = db.execute("SELECT file_id,sha256 FROM files WHERE relative_path='gone.md'").fetchone()
            gone_id, gone_hash = int(gone_row[0]), str(gone_row[1])
            # A stale chunk and vector emulate interrupted/replaced extraction output.
            db.execute(
                "INSERT INTO file_chunks(file_id,source_sha256,chunk_index,start_byte,end_byte,start_line,end_line,chunk_text,relative_path,title,description,metadata_text,chunker_version) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (keep_id, "stale-source-hash", 99, 0, 12, 1, 1, "obsolete gc marker", "keep.md", "Keep", "", "", "gc-fixture-v1"),
            )
            stale_chunk_id = db.execute("SELECT chunk_id FROM file_chunks WHERE file_id=? AND chunker_version='gc-fixture-v1'", (keep_id,)).fetchone()[0]
            db.execute(
                "INSERT INTO chunk_embeddings(chunk_id,file_id,source_sha256,input_sha256,model_id,dimensions,vector_json) VALUES(?,?,?,?,?,?,?)",
                (stale_chunk_id, keep_id, "stale-source-hash", digest("obsolete"), "gc-fixture", 1, "[1.0]"),
            )
            # A tombstoned document vector must be removed, while its description history remains.
            db.execute(
                "INSERT INTO descriptions(file_id,source_sha256,model_id,prompt_version,title,description,status) VALUES(?,?,?,?,?,?,?)",
                (gone_id, gone_hash, "gc-fixture", "v1", "Gone", "Historical description", "complete"),
            )
            description_id = db.execute("SELECT description_id FROM descriptions WHERE file_id=? AND model_id='gc-fixture'", (gone_id,)).fetchone()[0]
            db.execute(
                "INSERT INTO embeddings(description_id,file_id,input_sha256,model_id,dimensions,vector_json) VALUES(?,?,?,?,?,?)",
                (description_id, gone_id, digest("Gone\nHistorical description"), "gc-fixture", 1, "[1.0]"),
            )
            db.execute(
                "INSERT INTO pipeline_jobs(file_id,stage,source_sha256,model_id,prompt_version,state) VALUES(?,?,?,?,?,?)",
                (keep_id, "embedding", "stale-source-hash", "gc-fixture", "v1", "failed"),
            )
            db.commit()

        stats_before = json.loads(run(cli, "stats", str(root)).stdout)
        assert stats_before["schema_version"] == 9, stats_before
        assert stats_before["files"]["active"] == 2 and stats_before["files"]["deleted"] == 0, stats_before
        assert stats_before["chunks"]["stale"] >= 1, stats_before
        assert stats_before["vectors"]["chunk_stale"] == 1, stats_before
        assert stats_before["vectors"]["document_current"] == 1, stats_before

        gone.unlink()
        run(cli, "scan", str(root))
        stats_tombstone = json.loads(run(cli, "stats", str(root)).stdout)
        assert stats_tombstone["files"]["deleted"] == 1, stats_tombstone
        assert stats_tombstone["vectors"]["document_stale"] == 1, stats_tombstone

        gc_result = json.loads(run(cli, "gc", str(root), "--vacuum").stdout)
        assert gc_result["status"] == "ok" and gc_result["vacuum"] is True, gc_result
        assert gc_result["stale_chunks_removed"] >= 1, gc_result
        assert gc_result["stale_chunk_vectors_removed"] == 1, gc_result
        assert gc_result["stale_document_vectors_removed"] == 1, gc_result
        assert gc_result["stale_pipeline_jobs_removed"] == 1, gc_result
        assert gc_result["stale_search_docs_removed"] == 1, gc_result

        with sqlite3.connect(database) as db:
            assert db.execute("SELECT COUNT(*) FROM files WHERE relative_path='gone.md' AND deleted_at IS NOT NULL").fetchone()[0] == 1
            assert db.execute("SELECT COUNT(*) FROM file_events WHERE event_type='deleted'").fetchone()[0] == 1
            assert db.execute("SELECT COUNT(*) FROM descriptions WHERE file_id=? AND model_id='gc-fixture'", (gone_id,)).fetchone()[0] == 1
            assert db.execute("SELECT COUNT(*) FROM embeddings WHERE file_id=?", (gone_id,)).fetchone()[0] == 0
            assert db.execute("SELECT COUNT(*) FROM memory_search_docs WHERE file_id=?", (gone_id,)).fetchone()[0] == 0
            assert db.execute("SELECT COUNT(*) FROM file_chunks WHERE chunker_version='gc-fixture-v1'").fetchone()[0] == 0
            assert db.execute("SELECT COUNT(*) FROM chunk_embeddings WHERE model_id='gc-fixture'").fetchone()[0] == 0
            assert db.execute("SELECT COUNT(*) FROM pipeline_jobs WHERE source_sha256='stale-source-hash'").fetchone()[0] == 0
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

        stats_after = json.loads(run(cli, "stats", str(root)).stdout)
        assert stats_after["files"]["total"] == 2 and stats_after["files"]["deleted"] == 1, stats_after
        assert stats_after["chunks"]["stale"] == 0, stats_after
        assert stats_after["vectors"]["chunk_stale"] == 0 and stats_after["vectors"]["document_stale"] == 0, stats_after
        assert stats_after["storage_bytes"] >= 0, stats_after

        # Invalid CLI flags fail clearly and don't perform maintenance.
        run(cli, "gc", str(root), "--unknown", ok=False)

    print("memory stats/gc tests passed")


if __name__ == "__main__":
    main()
