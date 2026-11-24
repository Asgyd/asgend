#!/usr/bin/env python3
"""Integration tests for resumable embeddings and hybrid BM25/semantic retrieval."""
from __future__ import annotations

import json
import os
import sqlite3
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


class State:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.calls: list[dict[str, Any]] = []
        self.active = 0
        self.max_active = 0
        self.dimension = 3
        self.fail = False


def vector_for(text: str, dimensions: int) -> list[float]:
    lowered = text.lower()
    values = [0.0] * dimensions
    if "cat" in lowered or "feline" in lowered or "felidae" in lowered:
        values[0] = 1.0
    elif "dog" in lowered or "canine" in lowered:
        values[min(1, dimensions - 1)] = 1.0
    else:
        values[min(2, dimensions - 1)] = 1.0
    return values


class Handler(BaseHTTPRequestHandler):
    state: State

    def log_message(self, _format: str, *args: object) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802
        size = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(size))
        with self.state.lock:
            self.state.calls.append({"path": self.path, "model": payload.get("model"),
                                     "auth": self.headers.get("Authorization"), "input": payload.get("input")})
            self.state.active += 1
            self.state.max_active = max(self.state.max_active, self.state.active)
            fail = self.state.fail
            dimensions = self.state.dimension
        try:
            time.sleep(0.04)
            if fail:
                raw = b'{"error":{"message":"injected embedding failure"}}'
                self.send_response(503)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return
            inputs = payload.get("input")
            vector = vector_for(inputs if isinstance(inputs, str) else "", dimensions)
            raw = json.dumps({"data": [{"object": "embedding", "index": 0, "embedding": vector}],
                              "model": payload.get("model"), "usage": {"prompt_tokens": 3, "total_tokens": 3}}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        finally:
            with self.state.lock:
                self.state.active -= 1


def run(cli: str, *args: str, env: dict[str, str], ok: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run([cli, *args], env=env, text=True, capture_output=True, timeout=40)
    if ok and proc.returncode != 0:
        raise AssertionError(f"command failed: {proc.args}\ncode={proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    if not ok and proc.returncode == 0:
        raise AssertionError(f"command unexpectedly succeeded: {proc.args}\nstdout={proc.stdout}")
    return proc


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_embedding.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    state = State()
    Handler.state = state
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}/v1"
    try:
        import tempfile
        with tempfile.TemporaryDirectory(prefix="asgend-memory-embedding-") as tmp:
            root = Path(tmp) / "project"
            root.mkdir()
            files = {
                "cats.md": "# Cats and felidae\nBiology of cats and feline species.\n",
                "dogs.md": "# Dogs and canines\nCare for domestic dogs and canine species.\n",
                "plants.md": "# Botany\nA guide to perennial plant species.\n",
            }
            raw_before: dict[str, bytes] = {}
            for name, text in files.items():
                path = root / name
                path.write_text(text, encoding="utf-8")
                raw_before[name] = path.read_bytes()
            run(cli, "scan", str(root), env=os.environ.copy())
            run(cli, "extract", str(root), env=os.environ.copy())
            database = root / ".asgend_mem" / "index.sqlite3"
            with sqlite3.connect(database) as db:
                assert db.execute("PRAGMA user_version").fetchone()[0] == 9
                rows = db.execute("SELECT file_id,relative_path,sha256 FROM files WHERE deleted_at IS NULL").fetchall()
                titles = {"cats.md": ("Cats field guide", "Felidae and feline species, cat biology."),
                          "dogs.md": ("Dogs field guide", "Domestic canine species, dog health."),
                          "plants.md": ("Plants reference", "Botany of perennial plant species.")}
                for file_id, path, sha in rows:
                    title, description = titles[path]
                    db.execute("INSERT INTO descriptions(file_id,source_sha256,model_id,prompt_version,title,description,status) VALUES(?,?,?,?,?,?,'complete')",
                               (file_id, sha, "test-generator", "test-v1", title, description))
                db.commit()
                assert db.execute("SELECT COUNT(*) FROM memory_search_docs WHERE title!=''").fetchone()[0] == 3
                assert db.execute("SELECT COUNT(*) FROM memory_fts WHERE memory_fts MATCH 'canine'").fetchone()[0] >= 1

            env = os.environ.copy()
            env.update({"ASGEND_MEMORY_EMBEDDING_BASE_URL": base,
                        "ASGEND_MEMORY_EMBEDDING_MODEL": "embedding-test-v1",
                        "ASGEND_MEMORY_EMBEDDING_API_KEY": "embedding-unit-test-secret"})
            first = run(cli, "embed", str(root), "--workers", "3", "--timeout-seconds", "5", env=env)
            assert "status=complete" in first.stdout and "embedded=3" in first.stdout, first.stdout
            with state.lock:
                first_calls = list(state.calls)
                assert len(first_calls) == 3, first_calls
                assert state.max_active >= 2, state.max_active
                assert all(call["path"] == "/v1/embeddings" for call in first_calls), first_calls
                assert all(call["auth"] == "Bearer embedding-unit-test-secret" for call in first_calls)
            with sqlite3.connect(database) as db:
                assert db.execute("SELECT COUNT(*) FROM embeddings").fetchone()[0] == 3
                assert db.execute("SELECT COUNT(*) FROM pipeline_jobs WHERE stage='embedding' AND state='succeeded'").fetchone()[0] == 3
                stored = json.dumps(db.execute("SELECT model_id,vector_json FROM embeddings").fetchall())
                assert "embedding-unit-test-secret" not in stored
                assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

            second = run(cli, "embed", str(root), env=env)
            assert "embedded=0" in second.stdout and "skipped=3" in second.stdout, second.stdout
            with state.lock:
                assert len(state.calls) == 3, state.calls

            hybrid = run(cli, "query", str(root), "feline taxonomy", "--limit", "3", env=env)
            body = json.loads(hybrid.stdout)
            assert body["mode"] == "hybrid", body
            assert body["results"] and body["results"][0]["path"] == "cats.md", body
            assert body["results"][0]["semantic_similarity"] > 0.99, body
            lexical = run(cli, "query", str(root), "canine", "--lexical-only", env=env)
            lexical_body = json.loads(lexical.stdout)
            assert lexical_body["mode"] == "bm25" and lexical_body["results"][0]["path"] == "dogs.md", lexical_body

            # Changing description text invalidates only that description's vector, without modifying source files.
            with sqlite3.connect(database) as db:
                cat_id = db.execute("SELECT file_id FROM files WHERE relative_path='cats.md'").fetchone()[0]
                db.execute("UPDATE descriptions SET title='Feline taxonomy handbook',description='Updated feline species taxonomy.' WHERE file_id=?", (cat_id,))
                db.commit()
                assert db.execute("SELECT COUNT(*) FROM memory_fts WHERE memory_fts MATCH 'handbook'").fetchone()[0] == 1
                assert db.execute("SELECT COUNT(*) FROM memory_fts WHERE memory_fts MATCH 'field'").fetchone()[0] == 1
            changed = run(cli, "embed", str(root), env=env)
            assert "embedded=1" in changed.stdout and "skipped=2" in changed.stdout, changed.stdout
            query_after_update = json.loads(run(cli, "query", str(root), "feline taxonomy", env=env).stdout)
            assert query_after_update["results"][0]["path"] == "cats.md", query_after_update
            assert all((root / name).read_bytes() == raw for name, raw in raw_before.items())

            # Chunk-level embeddings are optional, independently cached, and used for precise semantic retrieval.
            chunk_build = run(cli, "chunk", str(root), "--max-chunk-tokens", "16", env=env)
            first_chunk_embed = run(cli, "embed", str(root), "--chunks", "--chunk-tokens", "16", "--workers", "3", env=env)
            assert "status=complete" in first_chunk_embed.stdout, first_chunk_embed.stdout
            assert "chunks_embedded=" in first_chunk_embed.stdout, first_chunk_embed.stdout
            with sqlite3.connect(database) as db:
                chunk_rows = db.execute("SELECT COUNT(*) FROM chunk_embeddings").fetchone()[0]
                assert chunk_rows > 0, chunk_rows
                assert db.execute("PRAGMA user_version").fetchone()[0] == 9
                assert db.execute("SELECT COUNT(DISTINCT dimensions) FROM chunk_embeddings WHERE model_id='embedding-test-v1'").fetchone()[0] == 1
                stored_chunk_vectors = json.dumps(db.execute("SELECT model_id,vector_json FROM chunk_embeddings").fetchall())
                assert "embedding-unit-test-secret" not in stored_chunk_vectors
            repeated_chunk_embed = run(cli, "embed", str(root), "--chunks", "--chunk-tokens", "16", "--workers", "3", env=env)
            assert "chunks_embedded=0" in repeated_chunk_embed.stdout, repeated_chunk_embed.stdout
            assert "chunks_skipped=" in repeated_chunk_embed.stdout, repeated_chunk_embed.stdout
            with sqlite3.connect(database) as db:
                chunk_vectors_before_query = db.execute("SELECT COUNT(*) FROM chunk_embeddings").fetchone()[0]
            chunk_hybrid = json.loads(run(cli, "query", str(root), "feline taxonomy", "--limit", "3", env=env).stdout)
            assert chunk_hybrid["chunk_semantic_matches"] > 0, chunk_hybrid
            assert chunk_hybrid["mode"].startswith("hybrid-chunk"), chunk_hybrid
            assert any(item.get("semantic_source") == "chunk" for item in chunk_hybrid["results"]), chunk_hybrid
            with sqlite3.connect(database) as db:
                assert db.execute("SELECT COUNT(*) FROM chunk_embeddings").fetchone()[0] == chunk_vectors_before_query, "query must preserve non-default chunk embedding cache"

            # Invalid API-key transport must fail before network transmission when new work is pending.
            with sqlite3.connect(database) as db:
                db.execute("UPDATE descriptions SET title='Feline taxonomy changed once more' WHERE file_id=?", (cat_id,))
                db.commit()
            unsafe_env = env.copy()
            unsafe_env["ASGEND_MEMORY_EMBEDDING_BASE_URL"] = "http://192.0.2.10/v1"
            with state.lock:
                calls_before = len(state.calls)
            unsafe = run(cli, "embed", str(root), env=unsafe_env, ok=False)
            assert "API keys require HTTPS" in unsafe.stderr, unsafe.stderr
            with state.lock:
                assert len(state.calls) == calls_before, state.calls
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
