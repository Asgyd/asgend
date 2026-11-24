#!/usr/bin/env python3
"""Smoke test for benchmark metrics against a tiny deterministic judged corpus."""
from __future__ import annotations
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cli: str, *args: str) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run([cli, *args], text=True, capture_output=True, timeout=35)
    if proc.returncode != 0:
        raise AssertionError(f"command failed: {proc.args}\ncode={proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    return proc


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_retrieval_eval.py ASGEND_MEMORY_CLI PROJECT_SOURCE_ROOT")
    cli, project = sys.argv[1:]
    evaluator = Path(project) / "scripts" / "evaluate_memory_retrieval.py"
    dataset = Path(project) / "tests" / "fixtures" / "retrieval_eval_dataset.json"
    with tempfile.TemporaryDirectory(prefix="asgend-retrieval-eval-") as temp:
        root = Path(temp) / "project"
        (root / "docs" / "security").mkdir(parents=True)
        (root / "docs" / "operations").mkdir(parents=True)
        (root / "src" / "memory").mkdir(parents=True)
        corpus = {
            "docs/security/authentication.md": "# Authentication and session recovery\nRefresh an expired authentication session token to restore login access.\n",
            "docs/security/authorization.md": "# Authorization and access control\nCheck role permissions and access control before opening private resources.\n",
            "docs/operations/database-backup.md": "# SQLite database backup\nCreate database backup snapshots, restore corrupted databases and run an integrity check.\n",
            "src/memory/embedding.c": "/* Embedding vectors are ranked by cosine similarity for semantic retrieval. */\nint embedding_search(void) { return 0; }\n",
            "docs/general.md": "# Project overview\nThis document describes the general project and its main goals.\n",
        }
        raw = {}
        for relative, text in corpus.items():
            path = root / relative
            path.write_text(text, encoding="utf-8")
            raw[relative] = path.read_bytes()
        run(cli, "scan", str(root))
        run(cli, "extract", str(root))
        dbpath = root / ".asgend_mem" / "index.sqlite3"
        titles = {
            "docs/security/authentication.md": ("Authentication session recovery", "Refresh an expired login session token."),
            "docs/security/authorization.md": ("Authorization and role permissions", "Private access control checks for roles and permissions."),
            "docs/operations/database-backup.md": ("SQLite database backup and restore", "Backup snapshots and restore with SQLite integrity checks."),
            "src/memory/embedding.c": ("Embedding vector similarity", "Semantic retrieval uses normalized embedding vectors and cosine similarity."),
            "docs/general.md": ("Project overview", "General project goals and overview."),
        }
        with sqlite3.connect(dbpath) as db:
            for file_id, path, sha in db.execute("SELECT file_id,relative_path,sha256 FROM files WHERE deleted_at IS NULL"):
                title, description = titles[path]
                db.execute("INSERT INTO descriptions(file_id,source_sha256,model_id,prompt_version,title,description,status) VALUES(?,?,?,?,?,?,'complete')",
                           (file_id, sha, "eval-fixture-v1", "test-v1", title, description))
            db.commit()
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
        proc = subprocess.run([sys.executable, str(evaluator), "--cli", cli, "--root", str(root), "--dataset", str(dataset)],
                              text=True, capture_output=True, timeout=90)
        if proc.returncode != 0:
            raise AssertionError(f"evaluator failed\nstdout={proc.stdout}\nstderr={proc.stderr}")
        report = json.loads(proc.stdout)
        assert report["status"] == "complete" and report["query_count"] == 4, report
        assert report["aggregate"]["mrr@3"] >= 0.75, report["aggregate"]
        assert report["aggregate"]["recall@3"] >= 0.75, report["aggregate"]
        assert report["aggregate"]["ndcg@3"] >= 0.75, report["aggregate"]
        assert all(query["metrics"]["mrr@3"] > 0 for query in report["queries"]), report
        with sqlite3.connect(dbpath) as db:
            assert db.execute("PRAGMA user_version").fetchone()[0] == 9
        for relative, before in raw.items():
            assert (root / relative).read_bytes() == before, relative


if __name__ == "__main__":
    main()
