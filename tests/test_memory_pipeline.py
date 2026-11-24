#!/usr/bin/env python3
"""Integration tests for the resumable ASGEND model-enrichment pipeline."""
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


class MockState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.calls: list[dict[str, Any]] = []
        self.active = 0
        self.max_active = 0
        self.fail_model: str | None = None


class Handler(BaseHTTPRequestHandler):
    state: MockState

    def log_message(self, _format: str, *args: object) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        size = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(size))
        model = payload["model"]
        messages = payload["messages"]
        user = json.loads(messages[-1]["content"])
        task = user["task"]
        with self.state.lock:
            self.state.calls.append({"model": model, "task": task, "auth": self.headers.get("Authorization"),
                                    "path": self.path, "has_existing_okf": bool(user.get("existing_okf_metadata"))})
            self.state.active += 1
            self.state.max_active = max(self.state.max_active, self.state.active)
            should_fail = self.state.fail_model == model
        time.sleep(0.035)
        try:
            if should_fail:
                raw = b'{"error":{"message":"injected test failure"}}'
                self.send_response(503)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return
            if task == "classify_document":
                result = {
                    "type": "source_code" if user["extension"] == "c" else "documentation",
                    "tags": ["asgend", "test"],
                    "keywords": ["memory-index", user["relative_path"]],
                    "language": "en",
                    "content_kind": "text",
                    "entities": ["ASGEND"],
                    "references": [],
                    "confidence": 0.93,
                    "untrusted_extra": "must not be persisted",
                }
            else:
                result = {
                    "title": f"Generated: {user['relative_path']}",
                    "description": f"A factual test description for {user['relative_path']}.",
                    "ignore_me": "ignored",
                }
            content = json.dumps(result, ensure_ascii=False)
            response = json.dumps({"choices": [{"message": {"role": "assistant", "content": content}}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)
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
        raise SystemExit("usage: test_memory_pipeline.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    state = MockState()
    Handler.state = state
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}/v1"
    try:
        import tempfile
        with tempfile.TemporaryDirectory(prefix="asgend-memory-pipeline-") as tmp:
            root = Path(tmp) / "project"
            root.mkdir()
            (root / "README.md").write_text("---\ntype: project_overview\ntags:\n  - orchestration\n  - memory\n---\n# ASGEND\nA local agent orchestrator and project memory.\n", encoding="utf-8")
            (root / "src").mkdir()
            (root / "src" / "memory.c").write_text("/* project memory scanner */\nint scan(void) { return 1; }\n", encoding="utf-8")
            (root / "docs.txt").write_text("Decision model and embedding pipeline specification.\n", encoding="utf-8")
            run(cli, "scan", str(root), env=os.environ.copy())
            run(cli, "extract", str(root), env=os.environ.copy())
            run(cli, "chunk", str(root), "--max-chunk-tokens", "96", env=os.environ.copy())

            env = os.environ.copy()
            env.update({
                "ASGEND_MEMORY_DECISION_BASE_URL": base,
                "ASGEND_MEMORY_DECISION_MODEL": "decision-test-v1",
                "ASGEND_MEMORY_DECISION_API_KEY": "unit-test-secret",
                "ASGEND_MEMORY_GENERATION_BASE_URL": base,
                "ASGEND_MEMORY_GENERATION_MODEL": "generation-test-v1",
                "ASGEND_MEMORY_GENERATION_API_KEY": "unit-test-secret-generation",
            })
            first = run(cli, "enrich", str(root), "--workers", "3", "--timeout-seconds", "5", env=env)
            assert "status=complete" in first.stdout, first.stdout
            assert "files_seen=3" in first.stdout, first.stdout
            assert "metadata_generated=3" in first.stdout, first.stdout
            assert "descriptions_generated=3" in first.stdout, first.stdout
            with state.lock:
                first_calls = list(state.calls)
                assert len(first_calls) == 6, first_calls
                assert state.max_active >= 2, state.max_active
                assert all(call["path"] == "/v1/chat/completions" for call in first_calls), first_calls
                assert {call["task"] for call in first_calls} == {"classify_document", "generate_title_and_description"}
                assert any(call["task"] == "classify_document" and call["has_existing_okf"] for call in first_calls), first_calls
                assert all(call["auth"] == "Bearer unit-test-secret" or call["auth"] == "Bearer unit-test-secret-generation" for call in first_calls)

            database = root / ".asgend_mem" / "index.sqlite3"
            with sqlite3.connect(database) as db:
                assert db.execute("PRAGMA user_version").fetchone()[0] == 9
                assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
                metadata = db.execute("SELECT COUNT(*) FROM model_metadata").fetchone()[0]
                descriptions = db.execute("SELECT COUNT(*) FROM descriptions WHERE status='complete'").fetchone()[0]
                assert (metadata, descriptions) == (3, 3)
                row = db.execute("SELECT metadata_json FROM model_metadata LIMIT 1").fetchone()
                parsed = json.loads(row[0])
                assert parsed["type"] in {"documentation", "source_code"}
                assert "untrusted_extra" not in parsed
                stored = " ".join(str(x) for x in db.execute("SELECT metadata_json FROM model_metadata").fetchall())
                stored += " ".join(str(x) for x in db.execute("SELECT title,description FROM descriptions").fetchall())
                assert "unit-test-secret" not in stored
                assert db.execute("SELECT COUNT(*) FROM pipeline_jobs WHERE state='succeeded'").fetchone()[0] == 6
            refreshed = json.loads(run(cli, "query", str(root), "Generated", "--lexical-only", "--limit", "10", env=env).stdout)
            refreshed_paths = {item["path"] for item in refreshed["results"]}
            assert any(path.startswith("src/") for path in refreshed_paths), refreshed

            source_files = [root / "README.md", root / "src" / "memory.c", root / "docs.txt"]
            source_bytes = [path.read_bytes() for path in source_files]
            second = run(cli, "enrich", str(root), "--workers", "2", env=env)
            assert "metadata_generated=0" in second.stdout and "descriptions_generated=0" in second.stdout, second.stdout
            with state.lock:
                assert len(state.calls) == 6, state.calls

            # Switching only the generation model regenerates descriptions, not metadata.
            env["ASGEND_MEMORY_GENERATION_MODEL"] = "generation-test-v2"
            third = run(cli, "enrich", str(root), env=env)
            assert "metadata_generated=0" in third.stdout and "descriptions_generated=3" in third.stdout, third.stdout
            with state.lock:
                assert len(state.calls) == 9, state.calls
                assert all(path.read_bytes() == before for path, before in zip(source_files, source_bytes))

            # Changing the decision profile invalidates both decision and dependent descriptions.
            env["ASGEND_MEMORY_DECISION_MODEL"] = "decision-test-v2"
            fourth = run(cli, "enrich", str(root), env=env)
            assert "metadata_generated=3" in fourth.stdout and "descriptions_generated=3" in fourth.stdout, fourth.stdout
            with state.lock:
                assert len(state.calls) == 15, state.calls

            # Failure is persisted with backoff; immediate retry is deferred, --retry bypasses it.
            (root / "new.txt").write_text("new file to check per-file failure handling\n", encoding="utf-8")
            run(cli, "scan", str(root), env=env)
            run(cli, "extract", str(root), env=env)
            state.fail_model = "decision-failing-model"
            env["ASGEND_MEMORY_DECISION_MODEL"] = "decision-failing-model"
            failed = run(cli, "enrich", str(root), env=env, ok=False)
            assert "failed=4" in failed.stdout, failed.stdout
            with sqlite3.connect(database) as db:
                assert db.execute("SELECT COUNT(*) FROM pipeline_jobs WHERE stage='decision' AND state='failed'").fetchone()[0] == 4
            retry = run(cli, "enrich", str(root), env=env, ok=False)
            assert "deferred=4" in retry.stdout, retry.stdout
            retried = run(cli, "enrich", str(root), "--retry", env=env, ok=False)
            assert "failed=4" in retried.stdout, retried.stdout

            # Do not send a configured secret over non-loopback plain HTTP.
            env["ASGEND_MEMORY_DECISION_MODEL"] = "unsafe-transport-model"
            env["ASGEND_MEMORY_DECISION_BASE_URL"] = "http://192.0.2.10/v1"
            with state.lock:
                before_unsafe = len(state.calls)
            unsafe = run(cli, "enrich", str(root), env=env, ok=False)
            assert "API keys require HTTPS" in unsafe.stderr, unsafe.stderr
            with state.lock:
                assert len(state.calls) == before_unsafe, state.calls
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
