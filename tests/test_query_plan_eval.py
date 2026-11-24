#!/usr/bin/env python3
"""Paired retrieval-evaluation smoke test for baseline versus query planning."""
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
from typing import Any


PLANS: dict[str, str] = {
    "how can I regain use of my profile": "session credential renewal login",
    "who controls entitlements for employees": "role permissions access control private resources",
    "protect information before maintenance": "SQLite database backup snapshots restore integrity",
    "rank similar representations efficiently": "embedding vectors cosine similarity semantic retrieval",
}


class MockPlanner(BaseHTTPRequestHandler):
    calls = 0

    def log_message(self, _format: str, *args: object) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802
        type(self).calls += 1
        size = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(size))
        messages = payload.get("messages", [])
        user = json.loads(messages[-1].get("content", "{}")) if messages else {}
        query = str(user.get("query", ""))
        rewrites = [PLANS[query]] if query in PLANS else []
        body = {
            "choices": [{"message": {"role": "assistant", "content": json.dumps({
                "intent": "general", "reason": "Use a concise domain-specific phrase.",
                "rewrites": rewrites,
                "filters": {"type": None, "tag": None, "path_prefix": None},
            })}}]
        }
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def run(*cmd: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(cmd, text=True, capture_output=True, timeout=45, env=env)
    if proc.returncode != 0:
        raise AssertionError(f"command failed: {cmd!r}\ncode={proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    return proc


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_query_plan_eval.py ASGEND_MEMORY_CLI PROJECT_SOURCE_ROOT")
    cli, project = sys.argv[1:]
    evaluator = Path(project) / "scripts" / "evaluate_memory_retrieval.py"
    dataset = Path(project) / "tests" / "fixtures" / "query_plan_eval_dataset.json"
    previous_env = os.environ.copy()
    MockPlanner.calls = 0
    server = ThreadingHTTPServer(("127.0.0.1", 0), MockPlanner)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        env = os.environ.copy()
        env.update({
            "ASGEND_MEMORY_DECISION_BASE_URL": f"http://127.0.0.1:{server.server_port}/v1",
            "ASGEND_MEMORY_DECISION_MODEL": "query-plan-eval-test",
        })
        with tempfile.TemporaryDirectory(prefix="asgend-query-plan-eval-") as temporary:
            root = Path(temporary) / "project"
            corpus = {
                "docs/security/authentication.md": "# Authentication and session recovery\nRefresh an expired authentication session token to restore login access.\n",
                "docs/security/authorization.md": "# Authorization and access control\nCheck role permissions and access control before opening private resources.\n",
                "docs/operations/database-backup.md": "# SQLite database backup\nCreate database backup snapshots, restore corrupted databases and run an integrity check.\n",
                "src/memory/embedding.c": "/* Embedding vectors are ranked by cosine similarity for semantic retrieval. */\nint embedding_search(void) { return 0; }\n",
                "docs/general.md": "# Project overview\nThis document describes the general project and its main goals.\n",
            }
            titles = {
                "docs/security/authentication.md": ("Authentication session recovery", "Refresh an expired login session token."),
                "docs/security/authorization.md": ("Authorization and role permissions", "Private access control checks for roles and permissions."),
                "docs/operations/database-backup.md": ("SQLite database backup and restore", "Backup snapshots and restore with SQLite integrity checks."),
                "src/memory/embedding.c": ("Embedding vector similarity", "Semantic retrieval uses normalized embedding vectors and cosine similarity."),
                "docs/general.md": ("Project overview", "General project goals and overview."),
            }
            for relative, text in corpus.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            run(cli, "scan", str(root), env=env)
            run(cli, "extract", str(root), env=env)
            dbpath = root / ".asgend_mem" / "index.sqlite3"
            with sqlite3.connect(dbpath) as db:
                for file_id, relative, sha in db.execute("SELECT file_id,relative_path,sha256 FROM files WHERE deleted_at IS NULL"):
                    title, description = titles[relative]
                    db.execute("INSERT INTO descriptions(file_id,source_sha256,model_id,prompt_version,title,description,status) VALUES(?,?,?,?,?,?,'complete')",
                               (file_id, sha, "query-plan-eval-fixture-v1", "test-v1", title, description))
                db.commit()
                assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

            proc = run(sys.executable, str(evaluator), "--cli", cli, "--root", str(root),
                       "--dataset", str(dataset), "--compare-plan", env=env)
            report = json.loads(proc.stdout)
            assert report["status"] == "complete" and report["evaluation_mode"] == "query_plan_comparison", report
            assert report["query_count"] == len(PLANS), report
            assert report["planned"]["aggregate"]["mrr@3"] > report["baseline"]["aggregate"]["mrr@3"], report
            assert report["planned"]["aggregate"]["recall@3"] > report["baseline"]["aggregate"]["recall@3"], report
            assert report["delta_planned_minus_baseline"]["mrr@3"] > 0.0, report
            assert all(item["query_plan"]["requested"] and item["query_plan"]["used"] for item in report["planned"]["queries"]), report
            assert MockPlanner.calls == len(PLANS), MockPlanner.calls
            for item in report["baseline"]["queries"]:
                assert "query_plan" not in item, item
            with sqlite3.connect(dbpath) as db:
                assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        # Do not leak test-only planner configuration into subsequent tests.
        os.environ.clear()
        os.environ.update(previous_env)


if __name__ == "__main__":
    main()
