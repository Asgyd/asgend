#!/usr/bin/env python3
"""Small OpenAI-compatible HTTP process used to test managed model startup/routing."""
import argparse
import json
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--port", type=int, required=True)
parser.add_argument("--tag", required=True)
parser.add_argument("--requests", required=True)
args = parser.parse_args()

STEPS = [
    {"title": "Inspect inputs", "instruction": "Inspect the task and its constraints.", "acceptance_criteria": "Inputs and constraints are listed."},
    {"title": "Implement change", "instruction": "Implement the requested change and report what was done.", "acceptance_criteria": "Implementation is described concretely."},
    {"title": "Verify output", "instruction": "Run relevant verification and report limitations.", "acceptance_criteria": "Verification outcome is recorded."},
]

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/health", "/v1/models"):
            body = b'{"ok":true}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(size).decode("utf-8"))
        messages = request.get("messages", [])
        system = next((m.get("content", "") for m in messages if m.get("role") == "system"), "")
        user = next((m.get("content", "") for m in reversed(messages) if m.get("role") == "user"), "")
        log_entry = {"tag": args.tag, "model": request.get("model"), "purpose": "agent"}
        log_entry["memory_probe_in_context"] = "ASGEND_MEMORY_CONTEXT_INTEGRATION_PROBE" in system
        log_entry["memory_document_in_context"] = "project-memory-proof.md" in system
        log_entry["auto_resource_v1_in_context"] = "ASGEND_AUTO_RESOURCE_DISCOVERY_V1" in system
        log_entry["auto_resource_refresh_v2_in_context"] = "ASGEND_AUTO_RESOURCE_REFRESH_V2" in system
        log_entry["auto_project_refresh_v2_in_context"] = "ASGEND_AUTO_PROJECT_REFRESH_V2" in system
        if "You are one of four independent resource classifiers" in system:
            match = re.search(r"Resource type:\s*([a-z_]+)", user)
            agent_match = re.search(r"Agent:\s*([A-Za-z0-9_-]+)", user)
            category = match.group(1) if match else "unknown"
            agent_id = agent_match.group(1) if agent_match else "unknown"
            files = re.findall(r"^### FILE: (.+)$", user, flags=re.MULTILINE)
            directory = {"agent-alpha": "planning", "agent-beta": "coding", "agent-gamma": "review"}.get(agent_id, "")
            desired = None
            if category == "skills":
                desired = f"{directory}/SKILL.md" if directory else None
            elif category == "system_prompts":
                desired = "security_review.md" if agent_id == "agent-gamma" else ("coding_style.md" if agent_id == "agent-beta" else None)
            elif category == "tools":
                desired = "echo.md" if agent_id == "agent-beta" else "knowledge_search.md"
            elif category == "knowledge":
                desired = "project-principles.md"
            selected = [desired] if desired and desired in files else []
            content = json.dumps({"selected": selected, "reason": "Deterministic integration-test resource classifier"})
            log_entry.update({"purpose": "resource_classifier", "resource_type": category, "agent": agent_id,
                              "catalog_files": files, "selected": selected})
        elif "decision model routing" in system:
            if "Inspect" in user or "Review" in user:
                routed_id = "agent-alpha"
            elif "Implement" in user or "change" in user.lower():
                routed_id = "agent-beta"
            elif "Verify" in user or "verification" in user.lower():
                routed_id = "agent-gamma"
            else:
                routed_id = "generalist"
            content = json.dumps({"agent_id": routed_id, "rationale": "The configured flexible profile best matches this subtask."})
            log_entry.update({"purpose": "router"})
        elif "clarification stage of a task decomposition loop" in system:
            needs_question = "NEEDS_CLARIFICATION" in user and "User clarifications:" not in user
            questions = ([{"question": "Which environment should be targeted?", "options": [
                {"id": "local", "label": "Local device"}, {"id": "lan", "label": "Local network"}]}] if needs_question else [])
            content = json.dumps({"title": "Review ASGEND task editor and safely preserve existing project history",
                                  "goal": "Implement requested change", "known_information": ["Task provided"],
                                  "questions": questions, "assumptions": [], "constraints": [],
                                  "clarification_needed": "NEEDS_CLARIFICATION" in user})
            log_entry.update({"purpose": "clarification"})
        elif "stage 2 of a task decomposition loop" in system or "independent quality reviewer" in system:
            obj = {"steps": STEPS}
            if "independent quality reviewer" in system:
                obj["review"] = {"score": 9, "summary": "All requirements have verifiable criteria.", "issues": [], "changes": []}
            content = json.dumps(obj)
            log_entry.update({"purpose": "decomposition" if "stage 2 of a task decomposition loop" in system else "quality_review"})
        elif "AGENT_INPUT_NEEDED" in user and "User clarifications:" not in user and "Implement" in user:
            content = json.dumps({"type": "question", "question": {
                "question": "Which implementation strategy should I use?",
                "options": [{"id": "minimal", "label": "Smallest safe change"},
                            {"id": "modular", "label": "More modular implementation"}]}})
            log_entry.update({"purpose": "agent_question"})
        else:
            content = f"managed model reply from {args.tag}"
            log_entry.update({"purpose": "agent"})
        with Path(args.requests).open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(log_entry, ensure_ascii=False) + "\n")
        body = json.dumps({"choices": [{"index": 0, "message": {"role": "assistant", "content": content}, "finish_reason": "stop"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *values):
        return

ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
