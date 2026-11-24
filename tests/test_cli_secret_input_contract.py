#!/usr/bin/env python3
"""Check hidden token entry and runtime diagnostics do not disclose token values."""
from __future__ import annotations
import json
import pathlib
import socket
import subprocess
import sys
import threading


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_cli_secret_input_contract.py CLI_BINARY PROJECT_ROOT", file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1])
    root = pathlib.Path(sys.argv[2])
    cli = (root / "clients/asgend_cli.c").read_text(encoding="utf-8")
    start = cli.index("static char *read_secret_line(const char *prompt)")
    end = cli.index("static void print_task_list", start)
    secret_reader = cli[start:end]
    assert "hidden.c_lflag &= (tcflag_t)~ECHO;" in secret_reader
    assert "tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden)" in secret_reader
    assert "tcsetattr(STDIN_FILENO, TCSAFLUSH, &original)" in secret_reader
    assert 'read_secret_line("Core token (hidden; ASGEND_TOKEN may be used instead): ")' in cli
    assert "--token may be visible in process listings" in cli
    assert "Do not echo the response body" in cli

    token = "audit-secret-must-not-appear-in-output"
    captured: list[bytes] = []
    server_errors: list[str] = []
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(4.0)
    port = listener.getsockname()[1]

    def serve_once() -> None:
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(3.0)
                chunks = bytearray()
                while b"\n" not in chunks:
                    block = conn.recv(4096)
                    if not block:
                        break
                    chunks.extend(block)
                captured.append(bytes(chunks))
                conn.sendall(b'{"ok":true,"tasks":[]}\n')
        except Exception as exc:  # propagate thread errors to the test
            server_errors.append(repr(exc))
        finally:
            listener.close()

    thread = threading.Thread(target=serve_once, daemon=True)
    thread.start()
    completed = subprocess.run(
        [str(binary), "--token", token, "--host", "127.0.0.1", "--port", str(port), "--no-tui"],
        input="1\nq\n", text=True, capture_output=True, timeout=6,
    )
    thread.join(timeout=5)
    if thread.is_alive():
        raise AssertionError("fake core thread did not finish")
    if server_errors:
        raise AssertionError(f"fake core failed: {server_errors}")
    if completed.returncode != 0:
        raise AssertionError(f"CLI returned {completed.returncode}: {completed.stderr[-2000:]}")
    if not captured:
        raise AssertionError("CLI did not send an RPC request")
    request_line = captured[0].split(b"\n", 1)[0]
    parsed = json.loads(request_line.decode("utf-8"))
    assert parsed.get("auth_token") == token, "fake core did not receive the configured token"
    combined_output = completed.stdout + completed.stderr
    assert token not in combined_output, "token value leaked to stdout/stderr"
    assert "Задач пока нет" in completed.stdout
    print("CLI secret input and runtime diagnostic contract verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
