#!/usr/bin/env python3
"""Opt-in query-plan validation, untrusted-input isolation and safe fallback tests."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


class MockState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.calls: list[dict[str, Any]] = []


class Handler(BaseHTTPRequestHandler):
    state: MockState

    def log_message(self, _format: str, *args: object) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802
        size = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(size))
        messages = payload.get("messages", [])
        user_content = messages[-1].get("content", "") if messages else ""
        try:
            user_data = json.loads(user_content)
        except (TypeError, json.JSONDecodeError):
            user_data = {"raw": user_content}
        query = user_data.get("query", "")
        with self.state.lock:
            self.state.calls.append({
                "path": self.path,
                "model": payload.get("model"),
                "temperature": payload.get("temperature"),
                "system": messages[0].get("content", "") if messages else "",
                "user": user_data,
                "auth": self.headers.get("Authorization"),
            })

        if "unsafe" in query:
            content = json.dumps({
                "intent": "documentation", "reason": "unsafe path test", "rewrites": ["outside needle"],
                "filters": {"type": None, "tag": None, "path_prefix": "../../outside"},
            })
        elif "malformed" in query:
            content = "```json\n{\"intent\":\"general\"}\n```"
        elif "extra" in query:
            content = json.dumps({
                "intent": "documentation", "reason": "extra field", "rewrites": [],
                "filters": {"type": None, "tag": None, "path_prefix": None}, "unexpected": True,
            })
        else:
            content = json.dumps({
                "intent": "documentation",
                "reason": "The query asks for setup documentation.",
                "rewrites": ["startupquartz"],
                "filters": {"type": "guide", "tag": None, "path_prefix": "docs"},
            })
        response = json.dumps({"choices": [{"message": {"role": "assistant", "content": content}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)


def run(cli: str, *args: str, env: dict[str, str] | None = None, ok: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run([cli, *args], env=env, text=True, capture_output=True, timeout=40)
    if ok and proc.returncode != 0:
        raise AssertionError(f"command failed: {proc.args}\ncode={proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    if not ok and proc.returncode == 0:
        raise AssertionError(f"command unexpectedly succeeded: {proc.args}\nstdout={proc.stdout}")
    return proc


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_query_plan.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    state = MockState()
    Handler.state = state
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}/v1"
    try:
        with tempfile.TemporaryDirectory(prefix="asgend-memory-plan-") as temp:
            root = Path(temp) / "project"
            (root / "docs").mkdir(parents=True)
            (root / "src").mkdir()
            (root / "docs" / "setup.md").write_text(
                "---\ntype: guide\ntags: [manual]\n---\n# Setup\nstartupquartz deployment settings. PRIVATE_DOC_SECRET do not transmit.\n",
                encoding="utf-8",
            )
            (root / "src" / "internal.md").write_text(
                "---\ntype: source_code\ntags: [internal]\n---\nstartupquartz source code. PRIVATE_DOC_SECRET do not transmit.\n",
                encoding="utf-8",
            )
            run(cli, "scan", str(root))
            run(cli, "extract", str(root))
            env = os.environ.copy()
            env.update({
                "ASGEND_MEMORY_DECISION_BASE_URL": base,
                "ASGEND_MEMORY_DECISION_MODEL": "query-plan-test",
                "ASGEND_MEMORY_DECISION_API_KEY": "plan-test-secret",
            })

            result = json.loads(run(cli, "query", str(root), "need setup", "--lexical-only", "--plan", env=env).stdout)
            plan = result["query_plan"]
            assert plan["requested"] and plan["used"], plan
            assert plan["intent"] == "documentation" and plan["rewrites"] == ["startupquartz"], plan
            assert result["query_classification"]["method"] == "decision_model_plan", result
            assert result["filters"]["type"] == "guide" and result["filters"]["path_prefix"] == "docs", result
            paths = {item["path"] for item in result["results"]}
            assert "docs/setup.md" in paths and "src/internal.md" not in paths, result

            with state.lock:
                assert len(state.calls) == 1, state.calls
                call = state.calls[0]
                assert call["path"] == "/v1/chat/completions", call
                assert call["model"] == "query-plan-test" and call["temperature"] == 0.0, call
                assert call["auth"] == "Bearer plan-test-secret", call
                assert call["user"] == {"query": "need setup", "allowed_intents": ["general", "dependency", "debugging", "configuration", "code", "documentation"]}, call
                assert "PRIVATE_DOC_SECRET" not in json.dumps(call), call
                assert "untrusted data" in call["system"].lower(), call

            # Explicit CLI filters override all planner-suggested filters.
            explicit = json.loads(run(cli, "query", str(root), "need setup", "--lexical-only", "--plan",
                                      "--type", "source_code", "--path-prefix", "src", env=env).stdout)
            assert explicit["filters"]["type"] == "source_code" and explicit["filters"]["path_prefix"] == "src", explicit
            assert {item["path"] for item in explicit["results"]} == {"src/internal.md"}, explicit

            # Planner errors never break the query and do not apply the planner's unsafe path.
            unsafe = json.loads(run(cli, "query", str(root), "unsafe query", "--lexical-only", "--plan", env=env).stdout)
            assert unsafe["query_plan"]["used"] is False, unsafe
            assert unsafe["query_plan"]["fallback_reason"] == "plan_schema_invalid", unsafe
            assert "path_prefix" not in unsafe["filters"], unsafe

            malformed = json.loads(run(cli, "query", str(root), "malformed query", "--lexical-only", "--plan", env=env).stdout)
            assert malformed["query_plan"]["used"] is False, malformed
            assert malformed["query_plan"]["fallback_reason"] == "plan_schema_invalid", malformed
            extra = json.loads(run(cli, "query", str(root), "extra field query", "--lexical-only", "--plan", env=env).stdout)
            assert extra["query_plan"]["used"] is False and extra["query_plan"]["fallback_reason"] == "plan_schema_invalid", extra

            # Remote cleartext endpoints are rejected before any HTTP request.
            insecure_env = env.copy()
            insecure_env["ASGEND_MEMORY_DECISION_BASE_URL"] = "http://example.com/v1"
            insecure = json.loads(run(cli, "query", str(root), "need setup", "--lexical-only", "--plan", env=insecure_env).stdout)
            assert insecure["query_plan"]["fallback_reason"] == "invalid_endpoint", insecure

            # --plan remains opt-in and works without model configuration as a local-search fallback.
            local_env = os.environ.copy()
            for key in ("ASGEND_MEMORY_DECISION_BASE_URL", "ASGEND_MEMORY_DECISION_MODEL", "ASGEND_MEMORY_DECISION_API_KEY"):
                local_env.pop(key, None)
            fallback = json.loads(run(cli, "query", str(root), "need setup", "--lexical-only", "--plan", env=local_env).stdout)
            assert fallback["query_plan"]["used"] is False, fallback
            assert fallback["query_plan"]["fallback_reason"] == "planner_not_configured", fallback
            ordinary = json.loads(run(cli, "query", str(root), "need setup", "--lexical-only", env=local_env).stdout)
            assert "query_plan" not in ordinary, ordinary
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
