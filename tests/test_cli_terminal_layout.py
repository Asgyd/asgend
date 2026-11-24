#!/usr/bin/env python3
"""Check that the line-oriented CLI menu fits narrow and wide pseudo-terminals."""
from __future__ import annotations

import fcntl
import os
import pty
import re
import select
import struct
import subprocess
import sys
import termios
import time
import unicodedata

ANSI_RE = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")


def display_width(line: str) -> int:
    total = 0
    for char in line:
        if unicodedata.combining(char):
            continue
        total += 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1
    return total


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_cli_terminal_layout.py PATH_TO_ASGEND_CLI", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    for columns, rows in ((20, 10), (31, 10), (40, 15), (44, 15), (80, 20)):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        env = os.environ.copy()
        env["ASGEND_TOKEN"] = "layout-test-token"
        process = subprocess.Popen(
            [binary, "--no-tui"], stdin=slave, stdout=slave, stderr=slave,
            close_fds=True, env=env,
        )
        os.close(slave)
        output = bytearray()
        try:
            # Canonical input mode buffers this line until the CLI reaches its prompt.
            os.write(master, b"q\n")
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], 0.1)
                if not ready:
                    if process.poll() is not None:
                        break
                    continue
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                output.extend(chunk)
            process.wait(timeout=1)
        finally:
            os.close(master)
            if process.poll() is None:
                process.kill()
                process.wait()
        if process.returncode != 0:
            raise AssertionError(
                f"CLI exited {process.returncode} at {columns}x{rows}: "
                f"{ANSI_RE.sub(b'', bytes(output)).decode('utf-8', 'replace')}"
            )
        text = ANSI_RE.sub(b"", bytes(output)).decode("utf-8", "replace")
        if any(char in text for char in "╭╮╰╯│─┌┐└┘"):
            raise AssertionError(f"box-drawing border glyph found at {columns}x{rows}")
        for index, line in enumerate(text.replace("\r", "").splitlines(), 1):
            width = display_width(line)
            if width > columns:
                raise AssertionError(
                    f"CLI line {index} occupies {width} columns in a {columns}-column terminal "
                    f"({columns}x{rows}): {line!r}"
                )
    print("CLI menu fits all tested terminal widths")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
