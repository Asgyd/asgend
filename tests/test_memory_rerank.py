#!/usr/bin/env python3
"""Exercises explicit, candidate-constrained decision-model reranking."""
from __future__ import annotations
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class State:
    calls = 0
    auth = []
    candidates = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        payload = json.loads(raw)
        State.calls += 1
        State.auth.append(self.headers.get("Authorization", ""))
        messages = payload.get("messages", [])
        user = json.loads(messages[1]["content"])
        candidates = user["candidates"]
        State.candidates = candidates
        target = next((item["id"] for item in candidates if item["path"] == "docs/authentication.md"), None)
        # Unknown IDs are hostile model output and must be ignored rather than mapped to a file.
        ranking = [{"id": 999999, "relevance": 4, "reason": "unknown id"}]
        if target is not None:
            ranking.append({"id": target, "relevance": 4, "reason": "Directly explains login recovery."})
        for item in candidates:
            if item["id"] != target:
                ranking.append({"id": item["id"], "relevance": 2, "reason": "Secondary reference."})
        content = json.dumps({"ranking": ranking})
        response = json.dumps({"choices": [{"message": {"content": content}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)


def run(cli: str, *args: str, env: dict[str, str] | None = None, ok: bool = True):
    proc = subprocess.run([cli, *args], text=True, capture_output=True, env=env, timeout=30)
    if ok and proc.returncode != 0:
        raise AssertionError(f"command failed: {proc.args}\ncode={proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    if not ok and proc.returncode == 0:
        raise AssertionError(f"command unexpectedly succeeded: {proc.args}\nstdout={proc.stdout}")
    return proc


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_rerank.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}/v1"
    try:
        with tempfile.TemporaryDirectory(prefix="asgend-memory-rerank-") as temp:
            root = Path(temp) / "project"
            (root / "docs").mkdir(parents=True)
            (root / "docs" / "authentication.md").write_text(
                "# Authentication recovery\nLogin recovery refreshes an expired session token and restores authentication.\n", encoding="utf-8")
            (root / "docs" / "incident.md").write_text(
                "# Login incident runbook\nWhen login fails, inspect authentication events and recover the session.\n", encoding="utf-8")
            (root / "docs" / "database.md").write_text(
                "# Database backup\nSQLite database snapshots and backup recovery procedures.\n", encoding="utf-8")
            run(cli, "scan", str(root))
            run(cli, "extract", str(root))
            database = root / ".asgend_mem" / "index.sqlite3"
            with sqlite3.connect(database) as db:
                rows = db.execute("SELECT file_id,sha256,relative_path FROM files WHERE deleted_at IS NULL").fetchall()
                descriptions = {
                    "docs/authentication.md": ("Authentication recovery", "How to recover login by refreshing the session token."),
                    "docs/incident.md": ("Login incident runbook", "Debug authentication failures and session recovery."),
                    "docs/database.md": ("SQLite database backup", "Database snapshot, backup and restore."),
                }
                for file_id, sha, path in rows:
                    title, description = descriptions[path]
                    db.execute("INSERT INTO descriptions(file_id,source_sha256,model_id,prompt_version,title,description,status) VALUES(?,?,?,?,?,?,'complete')",
                               (file_id, sha, "test-generator", "test-v1", title, description))
                db.commit()
                assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

            env = os.environ.copy()
            env.update({"ASGEND_MEMORY_RERANK_BASE_URL": base,
                        "ASGEND_MEMORY_RERANK_MODEL": "decision-rerank-test-v1",
                        "ASGEND_MEMORY_RERANK_API_KEY": "rerank-test-secret"})
            # Configuring credentials alone must not activate the model request.
            no_rerank = json.loads(run(cli, "query", str(root), "login recovery session token", "--lexical-only", env=env).stdout)
            assert State.calls == 0, State.calls
            reranked = json.loads(run(cli, "query", str(root), "login recovery session token", "--lexical-only", "--rerank", "--rerank-candidates", "3", env=env).stdout)
            assert State.calls == 1, State.calls
            assert State.auth == ["Bearer rerank-test-secret"], State.auth
            assert len(State.candidates) == 3, State.candidates
            assert reranked["reranking"]["method"] == "decision_model", reranked
            assert reranked["reranking"]["ranked_count"] == 3, reranked
            assert reranked["results"][0]["path"] == "docs/authentication.md", reranked
            assert reranked["results"][0]["rerank_rank"] == 1, reranked
            assert reranked["results"][0]["rerank_relevance"] == 4, reranked
            assert "Directly explains" in reranked["results"][0]["rerank_reason"], reranked
            assert all(item.get("path") != "" for item in reranked["results"]), reranked
            with sqlite3.connect(database) as db:
                stored = json.dumps(db.execute("SELECT model_id,title,description FROM descriptions").fetchall())
                assert "rerank-test-secret" not in stored
                assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

            # Remote cleartext endpoints are rejected before any network transmission.
            unsafe_env = env.copy()
            unsafe_env["ASGEND_MEMORY_RERANK_BASE_URL"] = "http://192.0.2.10/v1"
            before = State.calls
            unsafe = run(cli, "query", str(root), "login recovery session token", "--lexical-only", "--rerank", env=unsafe_env, ok=False)
            assert "requires HTTPS" in unsafe.stderr, unsafe.stderr
            assert State.calls == before, State.calls
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
