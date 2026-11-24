#!/usr/bin/env python3
"""Core-side artifact support for the remote Execution Node (contract v1 section 8).

Usage: test_core_artifacts.py <agent-orchestrator binary> <source root>

Covers `artifact_list` (recursive, symlink/special/internal/depth/size filtering,
SHA-256, MIME, truncation) and `workspace_read_bytes` (binary-safe base64 chunks,
bounds, workspace confinement), plus auth and Unix-socket reachability.
"""
import base64
import hashlib
import json
import os
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
TOKEN = "artifact-test-secret"
READ_MAX = 1048576
READ_DEFAULT = 262144


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _exchange(sock, payload, token, timeout):
    body = dict(payload)
    if token is not None:
        body["auth_token"] = token
    data = (json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
    sock.settimeout(timeout)
    sock.sendall(data)
    chunks = bytearray()
    while True:
        part = sock.recv(1 << 20)
        if not part:
            break
        chunks.extend(part)
        if b"\n" in part:
            break
    return json.loads(bytes(chunks).split(b"\n", 1)[0].decode())


def rpc(port, payload, token=TOKEN, timeout=20):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        return _exchange(sock, payload, token, timeout)


def unix_rpc(path, payload, token=TOKEN, timeout=20):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        sock.connect(str(path))
        return _exchange(sock, payload, token, timeout)


def wait_for(predicate, timeout=8, label="condition"):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except (OSError, ConnectionError, TimeoutError, json.JSONDecodeError):
            pass
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {label}; last={last!r}")


def start_server(root: Path, port: int):
    agents_dir = root / "agents"
    shutil.copytree(PROJECT / "config" / "agents", agents_dir)
    models_path = root / "models.json"
    shutil.copyfile(PROJECT / "config" / "models.json", models_path)
    config = {
        "server": {"host": "127.0.0.1", "port": port, "auth_token": TOKEN,
                   "max_request_bytes": 65536, "max_connections": 24, "request_timeout_seconds": 4,
                   "unix_socket": {"enabled": True, "path": str(root / "runtime.sock")}},
        "runtime": {"workers": 1, "queue_max_pending": 100, "max_retries": 2, "priority_aging_seconds": 2,
                    "database_path": str(root / "tasks.sqlite3")},
        "local_llm": {"enabled": False, "base_url": "http://127.0.0.1:11434/v1", "model": "mock-model",
                      "api_key": "", "timeout_seconds": 5, "temperature": 0.0},
        "paths": {"asgend_home": str(root / "asgend-home"),
                  "agents_dir": str(agents_dir),
                  "models_config": str(models_path),
                  "skills_dir": str(PROJECT / "skills"),
                  "knowledge_dir": str(PROJECT / "knowledge"),
                  "system_prompts_dir": str(PROJECT / "system_prompts"),
                  "tools_dir": str(PROJECT / "tools"),
                  "instructions_dir": str(PROJECT / "instructions")},
        "mcp": {"servers": []},
        "lsp": {"servers": []},
        # Hosted CI containers may block Landlock; production defaults stay strict.
        "sandbox": {"enabled": True, "require_landlock": False, "allow_network": False,
                    "cpu_seconds": 120, "memory_mb": 512, "file_size_mb": 64, "open_files": 64},
        "ui": {"enabled": False, "port": 8766, "auth_token": "ui-test-token-with-at-least-32-characters",
               "directory": str(PROJECT / "ui")},
    }
    config_path = root / "config.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    proc = subprocess.Popen([str(BINARY), "-c", str(config_path)], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        wait_for(lambda: rpc(port, {"op": "ping"}) if proc.poll() is None else None,
                 timeout=6, label="server startup")
    except Exception as exc:
        stderr = stop_server(proc, check=False)
        raise AssertionError(f"server failed to start; stderr={stderr!r}") from exc
    return proc


def stop_server(proc, check=True):
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, 15)
        except ProcessLookupError:
            pass
    try:
        _, stderr = proc.communicate(timeout=8)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, 9)
        except ProcessLookupError:
            pass
        _, stderr = proc.communicate(timeout=3)
        if check:
            raise AssertionError("server did not stop promptly")
    if check and proc.returncode not in (0, -15):
        raise AssertionError(f"server exited with {proc.returncode}: {stderr}")
    return stderr or ""


def submit_task(port, db_path, text):
    submitted = rpc(port, {"op": "submit", "task": text})
    assert submitted.get("ok") and submitted.get("task_id"), submitted
    task_id = submitted["task_id"]
    con = sqlite3.connect(db_path)
    try:
        workspace = Path(con.execute("SELECT workspace_path FROM tasks WHERE id=?", (task_id,)).fetchone()[0])
    finally:
        con.close()
    assert workspace.is_dir() and workspace.name == task_id, workspace
    return task_id, workspace


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_all(port, task_id, path, limit):
    offset, out = 0, bytearray()
    for _ in range(100000):
        reply = rpc(port, {"op": "workspace_read_bytes", "task_id": task_id, "path": path,
                           "offset": offset, "limit": limit})
        assert reply.get("ok") is True, reply
        encoded = reply["data_b64"]
        assert len(encoded) % 4 == 0, "base64 must be padded"
        chunk = base64.b64decode(encoded, validate=True)
        assert reply["offset"] == offset, reply
        assert reply["next_offset"] == offset + len(chunk), reply
        assert len(chunk) <= limit, (len(chunk), limit)
        out.extend(chunk)
        offset = reply["next_offset"]
        if reply["eof"]:
            assert offset == reply["size"], reply
            return bytes(out), reply["size"]
        assert len(chunk) == limit, "non-final chunk must be full"
    raise AssertionError("read loop did not terminate")


def expect_error(reply, label):
    assert reply.get("ok") is False and isinstance(reply.get("error"), str) and reply["error"], (label, reply)


def main():
    with tempfile.TemporaryDirectory(prefix="ao-c-artifacts-") as tmp:
        root = Path(tmp)
        port = free_port()
        proc = start_server(root, port)
        try:
            run_checks(root, port)
        finally:
            stop_server(proc)
    print("core artifact checks passed")


def run_checks(root: Path, port: int):
    db_path = root / "tasks.sqlite3"
    task_id, ws = submit_task(port, db_path, "Artifact listing smoke task")

    # --- auth: token is required on both new ops when configured ---
    for op in ({"op": "artifact_list", "task_id": task_id},
               {"op": "workspace_read_bytes", "task_id": task_id, "path": "x"}):
        expect_error(rpc(port, op, token=None), "missing token")
        denied = rpc(port, op, token="wrong-token")
        assert denied.get("ok") is False and denied.get("error") == "unauthorized", denied

    # --- unknown task ---
    expect_error(rpc(port, {"op": "artifact_list", "task_id": "zz-no-such-task"}), "unknown task list")
    expect_error(rpc(port, {"op": "workspace_read_bytes", "task_id": "zz-no-such-task", "path": "a"}), "unknown task read")
    expect_error(rpc(port, {"op": "artifact_list"}), "missing task_id")

    # --- workspace content ---
    text = "# Отчёт\nASGEND artifact\n"
    written = rpc(port, {"op": "workspace_write", "task_id": task_id, "path": "report.md", "content": text})
    assert written.get("ok"), written
    text_bytes = text.encode("utf-8")

    binary = bytes(range(256)) * 4100 + b"\x00tail"          # ~1.05 MB, crosses the 1 MiB read limit
    (ws / "data.bin").write_bytes(binary)
    png = b"\x89PNG\r\n\x1a\n" + b"\x00\x00\x00\rIHDR" + bytes(64)
    (ws / "picture").write_bytes(png)                           # no extension: magic sniff
    (ws / "README").write_bytes(b"plain words without an extension\n")
    (ws / "blob").write_bytes(b"\x00\x01\x02\xfe\xff")         # no extension, not text
    (ws / "empty.txt").write_bytes(b"")
    (ws / "out" / "sub").mkdir(parents=True)
    result_json = b'{"answer": 42}\n'
    (ws / "out" / "sub" / "result.json").write_bytes(result_json)

    # Excluded entries.
    outside_dir = root / "outside"
    outside_dir.mkdir()
    (outside_dir / "secret.txt").write_bytes(b"do not leak")
    (ws / "link.bin").symlink_to(ws / "data.bin")
    (ws / "escape.txt").symlink_to(outside_dir / "secret.txt")
    (ws / "linkdir").symlink_to(outside_dir, target_is_directory=True)
    os.mkfifo(ws / "pipe")
    (ws / ".asgend-write-1-2-3").write_bytes(b"internal temporary")

    # Depth: 16 nested directories are walked, the 17th is not.
    deep = ws
    for level in range(1, 18):
        deep = deep / f"d{level}"
        deep.mkdir()
        if level == 16:
            (deep / "ok.txt").write_bytes(b"depth sixteen\n")
        if level == 17:
            (deep / "too-deep.txt").write_bytes(b"depth seventeen\n")
    deep16 = "/".join(f"d{i}" for i in range(1, 17)) + "/ok.txt"
    deep17 = "/".join(f"d{i}" for i in range(1, 18)) + "/too-deep.txt"

    # Size: > 256 MiB is skipped (sparse file, nothing is hashed).
    with open(ws / "huge.bin", "wb") as fh:
        fh.truncate(256 * 1024 * 1024 + 1)

    listing = rpc(port, {"op": "artifact_list", "task_id": task_id}, timeout=60)
    assert listing.get("ok") is True, listing
    assert listing.get("truncated") is False, listing
    arts = {a["path"]: a for a in listing["artifacts"]}
    for item in listing["artifacts"]:
        assert set(item) >= {"path", "size", "sha256", "mime"}, item
        assert len(item["sha256"]) == 64 and all(c in "0123456789abcdef" for c in item["sha256"]), item
        assert not item["path"].startswith("/") and ".." not in item["path"].split("/"), item
    paths = [a["path"] for a in listing["artifacts"]]
    assert paths == sorted(paths), "artifacts are sorted by path"

    def check(path, data, mime):
        assert path in arts, (path, sorted(arts))
        assert arts[path]["size"] == len(data), arts[path]
        assert arts[path]["sha256"] == sha(data), arts[path]
        assert arts[path]["mime"] == mime, arts[path]

    check("report.md", text_bytes, "text/markdown")
    check("data.bin", binary, "application/octet-stream")
    check("picture", png, "image/png")
    check("README", b"plain words without an extension\n", "text/plain")
    check("blob", b"\x00\x01\x02\xfe\xff", "application/octet-stream")
    check("empty.txt", b"", "text/plain")
    check("out/sub/result.json", result_json, "application/json")
    check(deep16, b"depth sixteen\n", "text/plain")
    for excluded in ("link.bin", "escape.txt", "linkdir", "linkdir/secret.txt", "pipe", ".asgend-write-1-2-3",
                     "huge.bin", deep17, "out", "out/sub"):
        assert excluded not in arts, (excluded, listing)
    assert listing.get("skipped_too_large", 0) >= 1, listing
    assert listing.get("skipped_too_deep", 0) >= 1, listing
    assert listing.get("skipped_special", 0) >= 4, listing  # 3 symlinks + FIFO

    # Same op through the Unix socket and with a unique task ID prefix.
    via_unix = unix_rpc(root / "runtime.sock", {"op": "artifact_list", "task_id": task_id}, timeout=60)
    assert via_unix.get("ok") is True and {a["path"] for a in via_unix["artifacts"]} == set(arts), via_unix
    by_prefix = rpc(port, {"op": "artifact_list", "task_id": task_id[:6]}, timeout=60)
    assert by_prefix.get("ok") is True and "report.md" in {a["path"] for a in by_prefix["artifacts"]}, by_prefix

    # --- workspace_read_bytes: full and chunked reassembly ---
    full, size = read_all(port, task_id, "data.bin", READ_MAX)
    assert full == binary and size == len(binary)
    chunked, _ = read_all(port, task_id, "data.bin", 65537)
    assert chunked == binary
    tiny, _ = read_all(port, task_id, "report.md", 1)
    assert tiny == text_bytes
    nested, _ = read_all(port, task_id, "out/sub/result.json", READ_DEFAULT)
    assert nested == result_json

    default = rpc(port, {"op": "workspace_read_bytes", "task_id": task_id, "path": "data.bin"})
    assert default.get("ok") and default["offset"] == 0 and default["next_offset"] == READ_DEFAULT, \
        {k: v for k, v in default.items() if k != "data_b64"}
    assert base64.b64decode(default["data_b64"]) == binary[:READ_DEFAULT] and default["eof"] is False

    tail_offset = len(binary) - 3
    tail = rpc(port, {"op": "workspace_read_bytes", "task_id": task_id, "path": "data.bin",
                      "offset": tail_offset, "limit": 100})
    assert tail.get("ok") and base64.b64decode(tail["data_b64"]) == binary[-3:] and tail["eof"] is True, tail
    at_end = rpc(port, {"op": "workspace_read_bytes", "task_id": task_id, "path": "data.bin",
                        "offset": len(binary), "limit": 10})
    assert at_end.get("ok") and at_end["data_b64"] == "" and at_end["eof"] is True, at_end
    empty = rpc(port, {"op": "workspace_read_bytes", "task_id": task_id, "path": "empty.txt"})
    assert empty.get("ok") and empty["data_b64"] == "" and empty["size"] == 0 and empty["eof"] is True, empty
    via_unix_read = unix_rpc(root / "runtime.sock", {"op": "workspace_read_bytes", "task_id": task_id,
                                                     "path": "picture"})
    assert via_unix_read.get("ok") and base64.b64decode(via_unix_read["data_b64"]) == png, via_unix_read

    # --- bounds ---
    base = {"op": "workspace_read_bytes", "task_id": task_id, "path": "data.bin"}
    for label, extra in (("limit zero", {"limit": 0}), ("limit too large", {"limit": READ_MAX + 1}),
                         ("limit negative", {"limit": -5}), ("limit string", {"limit": "10"}),
                         ("limit float", {"limit": 1.5}), ("offset negative", {"offset": -1}),
                         ("offset string", {"offset": "0"}), ("offset past end", {"offset": len(binary) + 1})):
        expect_error(rpc(port, {**base, **extra}), label)

    # --- confinement ---
    abs_path = str(ws / "data.bin")
    for bad in ("../outside/secret.txt", "../" + task_id + "/data.bin", "/etc/passwd", abs_path,
                "out/../data.bin", "./data.bin", "data.bin/", "out//sub/result.json", "out\\sub\\result.json",
                "data.bin\u0000.txt", "report.md\u0000", "escape.txt", "link.bin", "linkdir/secret.txt",
                "pipe", "out", "out/sub", "missing.bin", ""):
        expect_error(rpc(port, {**base, "path": bad, "limit": 16}), f"path {bad!r}")
    expect_error(rpc(port, {"op": "workspace_read_bytes", "task_id": task_id}), "missing path")
    expect_error(rpc(port, {"op": "workspace_read_bytes", "task_id": task_id, "path": 7}), "non-string path")
    assert (outside_dir / "secret.txt").read_bytes() == b"do not leak"

    # --- entry limit: more than 1000 regular files -> truncated ---
    many_id, many_ws = submit_task(port, db_path, "Artifact truncation task")
    (many_ws / "bulk").mkdir()
    for i in range(1005):
        (many_ws / "bulk" / f"f{i:04d}.txt").write_bytes(f"{i}\n".encode())
    truncated = rpc(port, {"op": "artifact_list", "task_id": many_id}, timeout=60)
    assert truncated.get("ok") is True and truncated.get("truncated") is True, \
        {k: v for k, v in truncated.items() if k != "artifacts"}
    assert len(truncated["artifacts"]) == 1000, len(truncated["artifacts"])
    assert truncated.get("truncated_reason") == "entry_limit", truncated.get("truncated_reason")
    sample = truncated["artifacts"][0]
    content = (many_ws / sample["path"]).read_bytes()
    assert sample["sha256"] == sha(content) and sample["size"] == len(content), sample


if __name__ == "__main__":
    main()
