#!/usr/bin/env python3
"""Verify interactive RPCs bound both connect and foreground socket waits."""
from __future__ import annotations
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_cli_rpc_timeout_contract.py PROJECT_ROOT", file=sys.stderr)
        return 2
    cli = (pathlib.Path(sys.argv[1]) / "clients/asgend_cli.c").read_text(encoding="utf-8")
    assert "static int connect_with_timeout(" in cli
    connect_start = cli.index("static int connect_with_timeout(")
    connect_end = cli.index("static int connect_core", connect_start)
    connect = cli[connect_start:connect_end]
    assert "fcntl(fd, F_SETFL, original_flags | O_NONBLOCK)" in connect
    assert "poll(&pfd, 1, timeout_ms)" in connect
    assert "ETIMEDOUT" in connect and "SO_ERROR" in connect
    assert "connect_with_timeout(fd, it->ai_addr, it->ai_addrlen, cli_rpc_timeout_seconds)" in cli

    loop_start = cli.index("static int cli_tui_loop(const Client *client)")
    loop_end = cli.index("static void usage(", loop_start)
    loop = cli[loop_start:loop_end]
    assert "previous_rpc_timeout = cli_rpc_timeout_seconds" in loop
    assert "cli_rpc_timeout_seconds = 3;" in loop
    assert "cli_rpc_timeout_seconds = previous_rpc_timeout;" in loop
    print("CLI interactive RPC timeout contract verified")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
