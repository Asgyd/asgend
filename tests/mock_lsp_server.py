#!/usr/bin/env python3
"""Small strict LSP JSON-RPC mock used by tests; uses Content-Length framing."""
import json
import sys


def read_message():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break
        key, sep, value = line.partition(b":")
        if sep:
            headers[key.strip().lower()] = value.strip()
    length = int(headers.get(b"content-length", b"0"))
    if length < 0 or length > 4 * 1024 * 1024:
        raise RuntimeError("invalid Content-Length")
    body = sys.stdin.buffer.read(length)
    if len(body) != length:
        return None
    return json.loads(body.decode("utf-8"))


def send(message):
    body = json.dumps(message, separators=(",", ":")).encode()
    sys.stdout.buffer.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    sys.stdout.buffer.flush()


while True:
    msg = read_message()
    if msg is None:
        break
    method = msg.get("method")
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": msg["id"], "result": {"capabilities": {"textDocumentSync": 1}}})
    elif method == "textDocument/didOpen":
        params = msg.get("params", {})
        doc = params.get("textDocument", {})
        send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
              "params": {"uri": doc.get("uri", ""), "diagnostics": [
                  {"range": {"start": {"line": 0, "character": 0},
                             "end": {"line": 0, "character": 1}},
                   "severity": 1, "code": "MOCK001", "message": "mock error from LSP"}
              ]}})
    elif method == "shutdown":
        send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
        break
