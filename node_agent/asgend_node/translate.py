"""Translation of core status/events into contract v1 TaskEvent payloads."""

from __future__ import annotations

import hashlib
import json
import os
import re
from typing import Any

CORE_STATES = ("queued", "running", "waiting_input", "completed", "completed_without_model",
               "failed", "cancelled")

_SENSITIVE_KEY = re.compile(
    r"(?i)^(?:.*[_-])?(tokens?|secrets?|password|passwd|api[_-]?key|apikey|authorization|auth|cookies?|"
    r"credentials?|private[_-]?key|workspace[_-]?path|checkpoint|env|environment|command[_-]?line|argv)"
    r"(?:[_-].*)?$")
_TOKEN_VALUE = re.compile(r"\b(ncp|ucp|dlt|enr)_[A-Za-z0-9._\-]{4,}|\bsk-[A-Za-z0-9_\-]{12,}|\bBearer\s+\S+")
_ABS_PATH = re.compile(r"(?<![\w.])(/(?:home|root|var|srv|opt|tmp|etc|mnt|media|run|data|usr)/[^\s\"']*|"
                       r"[A-Za-z]:\\[^\s\"']*)")

MAX_STRING = 4000
MAX_LIST = 50
MAX_KEYS = 100
MAX_DEPTH = 6
MAX_PAYLOAD_BYTES = 32 * 1024


def _sanitize_string(value: str, limit: int = MAX_STRING) -> str:
    value = _TOKEN_VALUE.sub("[redacted]", value)
    value = _ABS_PATH.sub("[path]", value)
    if len(value) > limit:
        value = value[:limit] + "…[truncated]"
    return value


def sanitize(value: Any, depth: int = 0) -> Any:
    """Bound and scrub a core payload before it leaves the host."""
    if depth > MAX_DEPTH:
        return "[depth-limit]"
    if isinstance(value, dict):
        out: dict[str, Any] = {}
        for i, (k, v) in enumerate(value.items()):
            if i >= MAX_KEYS:
                out["_truncated_keys"] = len(value) - MAX_KEYS
                break
            key = str(k)[:100]
            if _SENSITIVE_KEY.search(key):
                out[key] = "[redacted]"
            else:
                out[key] = sanitize(v, depth + 1)
        return out
    if isinstance(value, list):
        items = [sanitize(v, depth + 1) for v in value[:MAX_LIST]]
        if len(value) > MAX_LIST:
            items.append(f"[{len(value) - MAX_LIST} more items]")
        return items
    if isinstance(value, str):
        return _sanitize_string(value)
    if isinstance(value, (int, float, bool)) or value is None:
        return value
    return _sanitize_string(str(value))


def bounded_payload(payload: dict) -> dict:
    encoded = json.dumps(payload, ensure_ascii=False)
    if len(encoded.encode("utf-8")) <= MAX_PAYLOAD_BYTES:
        return payload
    slim = {k: payload[k] for k in ("core_event_id", "core_type", "actor") if k in payload}
    slim["message"] = _sanitize_string(str(payload.get("message", "")), 2000)
    slim["payload_truncated"] = True
    return slim


def core_event_payload(event: dict, forward_payloads: bool = True) -> dict:
    payload: dict[str, Any] = {
        "core_event_id": int(event.get("event_id") or 0),
        "core_type": str(event.get("type") or "event")[:100],
        "actor": str(event.get("actor") or "system")[:100],
        "message": _sanitize_string(str(event.get("message") or "")),
    }
    if event.get("created_at"):
        payload["core_created_at"] = str(event["created_at"])[:64]
    if forward_payloads and isinstance(event.get("payload"), (dict, list)):
        payload["payload"] = sanitize(event["payload"])
    return bounded_payload(payload)


def extract_question(status: dict) -> dict | None:
    """Map core ``result.question`` ({question, options:[{id,label}]}) to the contract
    shape ``{"text", "options":[label...]}``."""
    result = status.get("result") if isinstance(status.get("result"), dict) else {}
    q = result.get("question")
    text = None
    options: list[str] = []
    option_ids: list[str] = []
    if isinstance(q, str):
        text = q
    elif isinstance(q, dict):
        text = q.get("question") or q.get("text")
        for opt in q.get("options") or []:
            if isinstance(opt, str):
                options.append(opt[:300])
            elif isinstance(opt, dict) and (opt.get("label") or opt.get("text")):
                options.append(str(opt.get("label") or opt.get("text"))[:300])
                option_ids.append(str(opt.get("id") or ""))
    if not text:
        return None
    out: dict[str, Any] = {"text": _sanitize_string(str(text), 1000), "options": options[:8]}
    if option_ids and len(option_ids) == len(options):
        out["option_ids"] = option_ids[:8]
    return out


def question_key(question: dict | None) -> str | None:
    if not question:
        return None
    return hashlib.sha256(json.dumps(question, sort_keys=True).encode("utf-8")).hexdigest()


def result_summary(status: dict) -> str | None:
    result = status.get("result")
    if isinstance(result, str):
        return _sanitize_string(result, 2000)
    if not isinstance(result, dict):
        return None
    for key in ("summary", "final_answer", "answer", "note"):
        if isinstance(result.get(key), str) and result[key].strip():
            return _sanitize_string(result[key], 2000)
    agent_results = result.get("agent_results")
    if isinstance(agent_results, list):
        for entry in reversed(agent_results):
            if isinstance(entry, dict) and isinstance(entry.get("output"), str) and entry["output"].strip():
                return _sanitize_string(entry["output"], 2000)
    return None


def status_error(status: dict) -> str | None:
    err = status.get("error")
    return _sanitize_string(str(err), 2000) if err else None


def state_changed_payload(status: dict, previous: str | None, core_task_id: str) -> dict:
    state = str(status.get("status") or "unknown")
    payload: dict[str, Any] = {
        "state": state,
        "previous_state": previous,
        "core_task_id": core_task_id,
        "attempts": int(status.get("attempts") or 0),
    }
    if status.get("max_attempts") is not None:
        payload["max_attempts"] = int(status.get("max_attempts") or 0)
    if status.get("next_attempt_at"):
        payload["next_attempt_at"] = str(status["next_attempt_at"])[:64]
    summary = result_summary(status)
    if summary and state in ("completed", "completed_without_model"):
        payload["result_summary"] = summary
    error = status_error(status)
    if error and state in ("failed", "cancelled"):
        payload["error"] = error
    return payload


def snapshot_body(status: dict, core_task_id: str, last_seq: int) -> dict:
    state = str(status.get("status") or "unknown")
    body: dict[str, Any] = {
        "state": state,
        "core_task_id": core_task_id,
        "last_seq": int(last_seq),
        "attempts": int(status.get("attempts") or 0),
    }
    question = extract_question(status) if state == "waiting_input" else None
    if question:
        body["question"] = question
    summary = result_summary(status)
    if summary and state in ("completed", "completed_without_model"):
        body["result_summary"] = summary
    error = status_error(status)
    if error and state in ("failed", "cancelled"):
        body["error"] = error
    return body


_BAD_NAME = re.compile(r"[\x00-\x1f\x7f/\\]")


def artifact_name(path: str) -> str | None:
    """Basename suitable for the contract (no '/', '\\', NUL, <=255 bytes)."""
    if not path or "\x00" in path:
        return None
    parts = re.split(r"[/\\]", path)
    if any(p == ".." for p in parts) or os.path.isabs(path) or path.startswith(("/", "\\")):
        return None
    name = parts[-1]
    if not name or name in (".", ".."):
        return None
    name = _BAD_NAME.sub("_", name)
    while len(name.encode("utf-8")) > 255:
        name = name[:-1]
    return name or None
