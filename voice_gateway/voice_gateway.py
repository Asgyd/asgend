#!/usr/bin/env python3
"""Loopback-only LFM3-Omni report gateway for ASGEND Cowork.

The gateway is deliberately a review intermediary: /turn creates/updates a
report, while only /confirm can submit the latest report to ASGEND's existing
/api/tasks queue. No model output is executed as a tool call.
"""
from __future__ import annotations

import base64
import binascii
import datetime as dt
import io
import json
import hashlib
import mimetypes
import os
import re
import secrets
import shutil
import tempfile
import threading
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable
from urllib.parse import urlparse

MAX_REQUEST_BYTES = 48 * 1024 * 1024
MAX_FILE_BYTES = 24 * 1024 * 1024
MAX_SESSION_BYTES = 64 * 1024 * 1024
MAX_ATTACHMENTS = 12
MAX_TURNS = 40
SESSION_TTL_SECONDS = 2 * 60 * 60
ALLOWED_MIME_PREFIXES = ("audio/", "image/", "video/")
ALLOWED_AUDIO = {"audio/wav", "audio/x-wav", "audio/mpeg", "audio/mp4", "audio/webm", "audio/ogg", "audio/flac"}
ALLOWED_IMAGE = {"image/png", "image/jpeg", "image/webp", "image/gif"}
ALLOWED_VIDEO = {"video/mp4", "video/webm", "video/quicktime", "video/x-matroska"}

SYSTEM_PROMPT = """You are the voice-and-multimodal interpreter for ASGEND Cowork. You are a separate model between the user and the project orchestrator.
You MUST NOT execute tools, modify files, submit tasks, or claim that changes were made. Your only job is to maintain a precise change report for human approval.
Use the project state, previous turns, user speech/text, and every selected object as evidence. Distinguish explicit user requests from inference. Do not invent missing details. Preserve filenames, code identifiers, numbers, and constraints accurately.
Return a concise but complete Markdown report with these sections:
1. Goal
2. Requested changes (numbered, actionable)
3. Selected context and affected objects (include file/selection labels when known)
4. Constraints and non-goals
5. Acceptance checks
6. Open questions / assumptions
End with: "Учтены ли все требования? Добавьте уточнения или подтвердите эту версию отчёта."
When new information arrives, revise the entire report, do not merely append a note. Never treat this report as user approval."""


@dataclass
class Attachment:
    path: Path
    name: str
    mime_type: str
    size: int


@dataclass
class Session:
    session_id: str
    created_at: float
    updated_at: float
    temp_dir: Path
    project_state: str = ""
    turns: list[dict[str, Any]] = field(default_factory=list)
    selections: list[dict[str, str]] = field(default_factory=list)
    attachments: list[Attachment] = field(default_factory=list)
    report: str = ""
    revision: int = 0
    confirmed_task_id: str = ""
    confirmed_revision: int = 0
    lock: threading.RLock = field(default_factory=threading.RLock, repr=False)


class GatewayError(Exception):
    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.status = status


class LlamaServerRunner:
    """HTTP adapter for LFM3-Omni served by llama.cpp's OpenAI-compatible API.

    No model weights or ML frameworks are loaded in this process. The local
    llama-server owns GGUF/projector loading and inference. Audio output is
    requested from the same server's /tts endpoint after the report is made.
    """

    def __init__(
        self,
        model_id: str | None = None,
        speech_output: bool = False,
        base_url: str | None = None,
        api_key: str | None = None,
        timeout: float | None = None,
    ):
        self.model_id = model_id or os.environ.get(
            "ASGEND_LFM3_MODEL", "LiquidAI/LFM3-Omni-2B-GGUF:Q4_K_M"
        )
        self.base_url = (base_url or os.environ.get(
            "ASGEND_LFM3_SERVER_URL", "http://127.0.0.1:8787"
        )).rstrip("/")
        parsed = urlparse(self.base_url)
        if parsed.scheme not in {"http", "https"} or not parsed.hostname:
            raise ValueError("ASGEND_LFM3_SERVER_URL must be an absolute HTTP(S) URL")
        if parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError("ASGEND_LFM3_SERVER_URL must not contain credentials, query, or fragment")
        self.api_key = api_key if api_key is not None else os.environ.get("ASGEND_LFM3_API_KEY", "")
        self.timeout = float(timeout if timeout is not None else os.environ.get("ASGEND_LFM3_TIMEOUT", "300"))
        self.speech_output = speech_output
        self.max_response_bytes = 16 * 1024 * 1024

    def _request(self, path: str, body: dict[str, Any] | None = None, timeout: float | None = None) -> tuple[bytes, str]:
        url = self.base_url + (path if path.startswith("/") else "/" + path)
        headers = {"Accept": "application/json" if body is not None else "*/*"}
        data = None
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if self.api_key:
            headers["Authorization"] = "Bearer " + self.api_key
        request = urllib.request.Request(url, data=data, headers=headers, method="POST" if body is not None else "GET")
        try:
            with urllib.request.urlopen(request, timeout=timeout or self.timeout) as response:
                content = response.read(self.max_response_bytes + 1)
                if len(content) > self.max_response_bytes:
                    raise GatewayError("llama-server response exceeds the configured 16 MiB limit", 502)
                return content, response.headers.get("Content-Type", "")
        except urllib.error.HTTPError as exc:
            detail = exc.read(4096).decode("utf-8", "replace")
            raise GatewayError(f"llama-server returned HTTP {exc.code}: {detail}", 502) from exc
        except GatewayError:
            raise
        except Exception as exc:
            raise GatewayError("Cannot reach llama-server at " + self.base_url + ": " + str(exc), 503) from exc

    @staticmethod
    def _data_uri(attachment: Attachment) -> str:
        encoded = base64.b64encode(attachment.path.read_bytes()).decode("ascii")
        return f"data:{attachment.mime_type};base64,{encoded}"

    def _message_content(self, session: Session) -> list[dict[str, Any]]:
        context_lines = [
            "Current project state:",
            session.project_state or "(not supplied)",
        ]
        if session.turns:
            context_lines.append("Conversation notes and selected objects:")
            for index, turn in enumerate(session.turns[-MAX_TURNS:], 1):
                if turn.get("text"):
                    context_lines.append(f"Turn {index}: {turn['text']}")
                for selection in turn.get("selections", []):
                    context_lines.append(
                        f"Selected {selection.get('kind', 'object')} "
                        f"[{selection.get('label', 'selection')}]:\n{selection.get('text', '')}"
                    )
        parts: list[dict[str, Any]] = [{"type": "text", "text": "\n\n".join(context_lines)}]
        for attachment in session.attachments:
            if not attachment.path.is_file():
                continue
            kind = attachment.mime_type.split("/", 1)[0]
            data_uri = self._data_uri(attachment)
            if kind == "image":
                parts.append({"type": "image_url", "image_url": {"url": data_uri}})
            elif kind == "audio":
                # llama.cpp detects the encoded file format; `format` is accepted
                # by OpenAI clients but may be ignored by llama-server.
                parts.append({"type": "input_audio", "input_audio": {"data": data_uri}})
            elif kind == "video":
                parts.append({"type": "input_video", "input_video": {"data": data_uri}})
        parts.append({
            "type": "text",
            "text": "Create or update the complete structured change report. "
                    "Return only the report, do not execute tools or claim changes were applied.",
        })
        return parts

    @staticmethod
    def _extract_text(response: dict[str, Any]) -> str:
        try:
            content = response["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as exc:
            raise GatewayError("llama-server response does not contain choices[0].message.content", 502) from exc
        if isinstance(content, str):
            return content.strip()
        if isinstance(content, list):
            return "\n".join(
                str(part.get("text", "")) for part in content
                if isinstance(part, dict) and part.get("type") in {"text", "output_text"}
            ).strip()
        raise GatewayError("llama-server returned an unsupported message content format", 502)

    def _synthesize(self, text: str) -> str | None:
        if not self.speech_output or not text:
            return None
        body, content_type = self._request(
            "/tts",
            {
                "input": text[:12_000],
                "lang": os.environ.get("ASGEND_LFM3_TTS_LANG", "ru"),
                "voice": os.environ.get("ASGEND_LFM3_TTS_VOICE", "female"),
                "response_format": "wav",
                "stream": False,
            },
        )
        if not content_type.lower().startswith(("audio/wav", "audio/x-wav", "application/octet-stream")):
            raise GatewayError("llama-server /tts returned an unexpected content type: " + content_type, 502)
        if not body:
            raise GatewayError("llama-server /tts returned empty audio", 502)
        return base64.b64encode(body).decode("ascii")

    def generate(self, session: Session) -> tuple[str, str | None]:
        payload = {
            "model": self.model_id,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT + " Respond in text; speech is generated via llama-server /tts."},
                {"role": "user", "content": self._message_content(session)},
            ],
            "temperature": 0.0,
            "max_tokens": 1600,
            "stream": False,
        }
        raw, _ = self._request("/v1/chat/completions", payload)
        try:
            response = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise GatewayError("llama-server returned invalid JSON from /v1/chat/completions", 502) from exc
        if not isinstance(response, dict):
            raise GatewayError("llama-server returned a non-object response", 502)
        text = self._extract_text(response)
        if not text:
            raise GatewayError("LFM3-Omni returned an empty report. Please clarify the request.", 502)
        return text, self._synthesize(text)

    def health(self) -> bool:
        try:
            self._request("/health", timeout=2.5)
            return True
        except GatewayError:
            return False


# Backward-compatible name for deployments importing this class directly.
LFM3OmniRunner = LlamaServerRunner

class CoworkManager:
    def __init__(self, runner: Any, enqueue: Callable[[str, str], str]):
        self.runner = runner
        self.enqueue = enqueue
        self.sessions: dict[str, Session] = {}
        self.lock = threading.RLock()

    def _prune(self) -> None:
        now = time.time()
        for sid, session in list(self.sessions.items()):
            if now - session.updated_at > SESSION_TTL_SECONDS:
                shutil.rmtree(session.temp_dir, ignore_errors=True)
                del self.sessions[sid]

    def create(self, project_state: str) -> Session:
        with self.lock:
            self._prune()
            if len(self.sessions) >= 64:
                raise GatewayError("Too many active Cowork sessions", 429)
            sid = str(uuid.uuid4())
            directory = Path(tempfile.mkdtemp(prefix="asgend-cowork-"))
            now = time.time()
            session = Session(sid, now, now, directory, project_state[:100_000])
            self.sessions[sid] = session
            return session

    def get(self, sid: str) -> Session:
        with self.lock:
            self._prune()
            session = self.sessions.get(sid)
            if not session:
                raise GatewayError("Cowork session not found or expired", 404)
            return session

    @staticmethod
    def _save_attachment(session: Session, item: dict[str, Any], pending_count: int = 0, pending_bytes: int = 0) -> Attachment:
        name = Path(str(item.get("name", "attachment"))).name[:180] or "attachment"
        mime = str(item.get("mime_type") or mimetypes.guess_type(name)[0] or "application/octet-stream").split(";", 1)[0].strip().lower()
        if not any(mime.startswith(prefix) for prefix in ALLOWED_MIME_PREFIXES):
            raise GatewayError(f"Unsupported multimodal file type: {mime}. Upload audio, image or video; office files stay in the project workspace.")
        if mime.startswith("audio/") and mime not in ALLOWED_AUDIO:
            raise GatewayError("Unsupported audio format: " + mime)
        if mime.startswith("image/") and mime not in ALLOWED_IMAGE:
            raise GatewayError("Unsupported image format: " + mime)
        if mime.startswith("video/") and mime not in ALLOWED_VIDEO:
            raise GatewayError("Unsupported video format: " + mime)
        encoded = item.get("data_base64")
        if not isinstance(encoded, str) or not encoded:
            raise GatewayError("Attachment data_base64 is required")
        try:
            raw = base64.b64decode(encoded, validate=True)
        except (binascii.Error, ValueError) as exc:
            raise GatewayError("Invalid attachment base64") from exc
        if not raw or len(raw) > MAX_FILE_BYTES:
            raise GatewayError("Attachment must be between 1 byte and 24 MiB")
        total = sum(a.size for a in session.attachments) + pending_bytes
        if total + len(raw) > MAX_SESSION_BYTES or len(session.attachments) + pending_count >= MAX_ATTACHMENTS:
            raise GatewayError("Cowork session attachment limit exceeded")
        ext = Path(name).suffix[:16]
        path = session.temp_dir / (uuid.uuid4().hex + ext)
        path.write_bytes(raw)
        return Attachment(path, name, mime, len(raw))

    def ingest(self, sid: str, payload: dict[str, Any]) -> dict[str, Any]:
        session = self.get(sid)
        with session.lock:
            return self._ingest_locked(session, payload)

    def _ingest_locked(self, session: Session, payload: dict[str, Any]) -> dict[str, Any]:
        sid = session.session_id
        project_state = payload.get("project_state")
        if project_state is not None:
            if not isinstance(project_state, str) or len(project_state) > 100_000:
                raise GatewayError("project_state must be text up to 100,000 characters")
            session.project_state = project_state
        if session.confirmed_task_id:
            raise GatewayError("This Cowork session has already been confirmed", 409)
        if len(session.turns) >= MAX_TURNS:
            raise GatewayError("Cowork session turn limit reached", 429)
        text = payload.get("text", "")
        if not isinstance(text, str) or len(text) > 50_000:
            raise GatewayError("text must be a string no longer than 50,000 characters")
        selections = payload.get("selections", [])
        if not isinstance(selections, list) or len(selections) > 32:
            raise GatewayError("selections must be an array of at most 32 items")
        normalized: list[dict[str, str]] = []
        for item in selections:
            if not isinstance(item, dict):
                raise GatewayError("each selection must be an object")
            value = item.get("text", "")
            if not isinstance(value, str) or len(value) > 30_000:
                raise GatewayError("selected text must be no longer than 30,000 characters")
            normalized.append({"kind": str(item.get("kind", "text"))[:40], "label": str(item.get("label", "selection"))[:180], "text": value})
        files = payload.get("attachments", [])
        if not isinstance(files, list) or len(files) > MAX_ATTACHMENTS:
            raise GatewayError("attachments must be an array of at most 12 items")
        stored: list[Attachment] = []
        try:
            for file in files:
                if not isinstance(file, dict):
                    raise GatewayError("each attachment must be an object")
                pending_bytes = sum(item.size for item in stored)
                stored.append(self._save_attachment(session, file, len(stored), pending_bytes))
        except Exception:
            # Treat one turn's media batch atomically: invalid later files must
            # not leave orphaned bytes outside the session accounting limits.
            for attachment in stored:
                try:
                    attachment.path.unlink(missing_ok=True)
                except OSError:
                    pass
            raise
        turn = {"text": text, "selections": normalized, "at": dt.datetime.now(dt.timezone.utc).isoformat()}
        session.turns.append(turn)
        session.selections.extend(normalized)
        session.attachments.extend(stored)
        session.updated_at = time.time()
        try:
            report, speech = self.runner.generate(session)
        except Exception:
            # Keep the submitted turn so user can retry by sending an explicit correction.
            raise
        session.report = report
        session.revision += 1
        return {
            "ok": True, "session_id": sid, "revision": session.revision,
            "report": report, "speech_wav_base64": speech,
            "attachment_count": len(session.attachments), "selection_count": len(session.selections),
            "requires_confirmation": True,
        }

    def confirm(self, sid: str, revision: int, ui_token: str) -> dict[str, Any]:
        session = self.get(sid)
        with session.lock:
            if session.confirmed_task_id:
                return {"ok": True, "task_id": session.confirmed_task_id, "revision": session.confirmed_revision, "idempotent_replay": True}
            if revision != session.revision or revision < 1 or not session.report:
                raise GatewayError("Report revision is stale. Review and confirm the latest report version.", 409)
            task = "Выполнить изменения по подтверждённому отчёту Cowork (версия %d):\n\n%s" % (revision, session.report)
            # Stable key protects against a lost HTTP response followed by a retry.
            idempotency_key = str(uuid.uuid5(uuid.NAMESPACE_URL, f"asgend-cowork:{session.session_id}:{revision}"))
            # enqueue callback can only be called here, after revision validation.
            task_id = self.enqueue(task, ui_token, idempotency_key)
        if not task_id:
            raise GatewayError("ASGEND did not return a task ID", 502)
        session.confirmed_task_id = task_id
        session.confirmed_revision = revision
        session.updated_at = time.time()
        return {"ok": True, "task_id": task_id, "revision": revision, "idempotent_replay": False}


class CoreQueueClient:
    def __init__(self):
        self.base_url = os.environ.get("ASGEND_CORE_URL", "http://127.0.0.1:8766").rstrip("/")

    def submit(self, task: str, ui_token: str, idempotency_key: str) -> str:
        if not ui_token or len(ui_token) > 512:
            raise GatewayError("Missing ASGEND UI token", 401)
        body = json.dumps({"task": task, "priority": 5, "idempotency_key": idempotency_key}).encode("utf-8")
        req = urllib.request.Request(
            self.base_url + "/api/tasks", data=body, method="POST",
            headers={"Content-Type": "application/json", "X-AO-Token": ui_token},
        )
        try:
            with urllib.request.urlopen(req, timeout=20) as response:
                data = json.loads(response.read(1024 * 1024).decode("utf-8"))
        except urllib.error.HTTPError as exc:
            detail = exc.read(8192).decode("utf-8", "replace")
            raise GatewayError(f"ASGEND queue rejected the report: HTTP {exc.code}: {detail}", 502) from exc
        except Exception as exc:
            raise GatewayError("Cannot reach ASGEND API: " + str(exc), 502) from exc
        task_id = data.get("task_id") if isinstance(data, dict) else None
        if not task_id:
            raise GatewayError("ASGEND response did not contain task_id", 502)
        return str(task_id)


def json_response(handler: BaseHTTPRequestHandler, status: int, obj: dict[str, Any]) -> None:
    raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    handler.send_response(status)
    origin = handler.headers.get("Origin", "")
    allowed_origins = getattr(handler.server, "allowed_origins", set())
    if origin and origin in allowed_origins:
        handler.send_header("Access-Control-Allow-Origin", origin)
        handler.send_header("Vary", "Origin")
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(raw)))
    handler.send_header("Cache-Control", "no-store")
    handler.send_header("X-Content-Type-Options", "nosniff")
    handler.end_headers()
    handler.wfile.write(raw)


def make_handler(manager: CoworkManager, expected_token: str, allowed_origins: set[str]):
    class Handler(BaseHTTPRequestHandler):
        server_version = "ASGEND-LFM3-Gateway/1.0"

        def log_message(self, fmt: str, *args: Any) -> None:
            # Never log request bodies, API tokens, model prompts, or media metadata.
            print("[voice-gateway] " + (fmt % args))

        def _cors(self) -> bool:
            origin = self.headers.get("Origin", "")
            if origin and origin not in allowed_origins:
                json_response(self, 403, {"ok": False, "error": "Origin not allowed"})
                return False
            if origin:
                self.send_header("Access-Control-Allow-Origin", origin)
                self.send_header("Vary", "Origin")
            return True

        def do_OPTIONS(self) -> None:
            origin = self.headers.get("Origin", "")
            if origin not in allowed_origins:
                json_response(self, 403, {"ok": False, "error": "Origin not allowed"})
                return
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
            self.send_header("Access-Control-Allow-Headers", "Content-Type, X-AO-Token")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Max-Age", "600")
            self.end_headers()

        def _authorized(self) -> bool:
            token = self.headers.get("X-AO-Token", "")
            if not expected_token or not secrets.compare_digest(token, expected_token):
                json_response(self, 401, {"ok": False, "error": "Invalid voice gateway token"})
                return False
            return True

        def _body(self) -> dict[str, Any]:
            length = int(self.headers.get("Content-Length", "0"))
            if length < 1 or length > MAX_REQUEST_BYTES:
                raise GatewayError("Request body must be between 1 byte and 48 MiB", 413)
            raw = self.rfile.read(length)
            try:
                value = json.loads(raw)
            except Exception as exc:
                raise GatewayError("Request body must be valid JSON") from exc
            if not isinstance(value, dict):
                raise GatewayError("Request JSON must be an object")
            return value

        def do_GET(self) -> None:
            if not self._cors(): return
            path = urlparse(self.path).path
            if path == "/health":
                json_response(self, 200, {"ok": True, "service": "asgend-lfm3-gateway", "model": os.environ.get("ASGEND_LFM3_MODEL", "LiquidAI/LFM3-Omni-2B-GGUF:Q4_K_M"), "runtime": "llama-server-openai-compatible", "llama_server": os.environ.get("ASGEND_LFM3_SERVER_URL", "http://127.0.0.1:8787")})
            else:
                json_response(self, 404, {"ok": False, "error": "Not found"})

        def do_POST(self) -> None:
            origin = self.headers.get("Origin", "")
            if origin and origin not in allowed_origins:
                json_response(self, 403, {"ok": False, "error": "Origin not allowed"}); return
            if not self._authorized(): return
            try:
                body = self._body()
                path = urlparse(self.path).path
                if path == "/v1/sessions":
                    project_state = body.get("project_state", "")
                    if not isinstance(project_state, str) or len(project_state) > 100_000:
                        raise GatewayError("project_state must be text up to 100,000 characters")
                    session = manager.create(project_state)
                    result = {"ok": True, "session_id": session.session_id, "revision": 0}
                else:
                    match = re.fullmatch(r"/v1/sessions/([0-9a-fA-F-]{36})/(turn|confirm)", path)
                    if not match:
                        raise GatewayError("Not found", 404)
                    sid, action = match.groups()
                    if action == "turn":
                        result = manager.ingest(sid, body)
                    else:
                        revision = body.get("revision")
                        if not isinstance(revision, int) or isinstance(revision, bool):
                            raise GatewayError("revision must be an integer")
                        result = manager.confirm(sid, revision, self.headers.get("X-AO-Token", ""))
                # CORS headers must be sent before json_response sends the response.
                # Build the response inline to keep all headers in one place.
                raw = json.dumps(result, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                if origin:
                    self.send_header("Access-Control-Allow-Origin", origin)
                    self.send_header("Vary", "Origin")
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.end_headers()
                self.wfile.write(raw)
            except GatewayError as exc:
                json_response(self, exc.status, {"ok": False, "error": str(exc)})
            except Exception as exc:
                json_response(self, 500, {"ok": False, "error": "Internal gateway error: " + str(exc)})

    return Handler


def main() -> int:
    bind = os.environ.get("ASGEND_VOICE_BIND", "127.0.0.1")
    port = int(os.environ.get("ASGEND_VOICE_PORT", "8788"))
    if bind not in {"127.0.0.1", "::1", "localhost"}:
        raise SystemExit("Refusing non-loopback bind; use a secured reverse proxy and explicit review before changing this guard")
    expected_token = os.environ.get("ASGEND_UI_TOKEN", "")
    if not expected_token:
        raise SystemExit("ASGEND_UI_TOKEN must be set to the same value as ui.auth_token")
    origin = os.environ.get("ASGEND_UI_ORIGIN", "http://127.0.0.1:8766")
    origins = {origin}
    # localhost and 127.0.0.1 are distinct browser origins; enable both only for the configured UI port.
    parsed = urlparse(origin)
    if parsed.hostname == "127.0.0.1":
        origins.add(f"{parsed.scheme}://localhost:{parsed.port or 80}")
    elif parsed.hostname == "localhost":
        origins.add(f"{parsed.scheme}://127.0.0.1:{parsed.port or 80}")
    runner = LlamaServerRunner(speech_output=os.environ.get("ASGEND_LFM3_SPEECH", "0") == "1")
    manager = CoworkManager(runner, CoreQueueClient().submit)
    server = ThreadingHTTPServer((bind, port), make_handler(manager, expected_token, origins))
    server.allowed_origins = origins
    server.daemon_threads = True
    print(f"ASGEND LFM3 gateway listening on http://{bind}:{port}; UI origin: {origin}")
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown(); server.server_close()
        for session in manager.sessions.values():
            shutil.rmtree(session.temp_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
