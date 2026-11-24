#!/usr/bin/env python3
"""Exercise the TUI renderer inside real pseudo-terminals of varied sizes."""
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
ROW_START_RE = re.compile(rb"\x1b\[(\d+);1H\x1b\[2K")


def display_width(line: str) -> int:
    width = 0
    for char in line:
        if unicodedata.combining(char):
            continue
        width += 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1
    return width


def render_at_size(binary: str, columns: int, rows: int, mode: str = "--render") -> list[str]:
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    process = subprocess.Popen(
        [binary, mode], stdin=slave, stdout=slave, stderr=slave, close_fds=True
    )
    os.close(slave)
    output = bytearray()
    deadline = time.monotonic() + 5
    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.1)
            if not ready:
                if process.poll() is not None:
                    break
                continue
            try:
                chunk = os.read(master, 65536)
            except OSError:  # Linux PTY master returns EIO after the slave closes.
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
            f"TUI renderer exited {process.returncode} at {columns}x{rows}: "
            f"{ANSI_RE.sub(b'', bytes(output)).decode('utf-8', 'replace')}"
        )
    # The renderer updates only dirty rows (cursor-position + erase-line), so
    # rebuild the final physical screen from the latest contents written to each row.
    raw = bytes(output)
    row_markers = list(ROW_START_RE.finditer(raw))
    final_rows: dict[int, str] = {}
    for index, marker in enumerate(row_markers):
        start = marker.end()
        end = row_markers[index + 1].start() if index + 1 < len(row_markers) else len(raw)
        row_number = int(marker.group(1))
        cleaned = ANSI_RE.sub(b"", raw[start:end]).decode("utf-8", "replace")
        cleaned = cleaned.replace("\r", "").replace("\n", "")
        final_rows[row_number] = cleaned
    if not final_rows:
        raise AssertionError(f"TUI renderer produced no positioned rows for terminal {columns}x{rows}")
    if max(final_rows) > rows:
        raise AssertionError(
            f"TUI wrote row {max(final_rows)} in a {rows}-row terminal ({columns}x{rows})"
        )
    for row_number, line in final_rows.items():
        width = display_width(line)
        if width > columns:
            raise AssertionError(
                f"TUI line {row_number} occupies {width} columns in a {columns}-column terminal "
                f"({columns}x{rows}): {line!r}"
            )
    return [final_rows[row] for row in sorted(final_rows)]


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_tui_terminal_layout.py PATH_TO_TEST_BINARY", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    border_chars = set("╭╮╰╯│─┌┐└┘")
    for columns, rows in ((80, 10), (40, 12), (120, 30), (30, 6), (80, 7), (36, 8)):
        lines = render_at_size(binary, columns, rows)
        text = "\n".join(lines)
        if any(char in text for char in border_chars):
            raise AssertionError(f"box-drawing border glyph found at {columns}x{rows}")
        if rows >= 8 and columns >= 100 and ("Goals · MCP" not in text or "Проект" not in text):
            raise AssertionError("wide terminals must show both project and Goals/MCP panels")
        if rows >= 8 and 68 <= columns < 100 and ("Проект" not in text or "Goals · MCP" in text):
            raise AssertionError("medium terminals must hide the right pane before the left pane")
        if 36 <= columns < 68 and ("Проект ·" in text or "Goals · MCP" in text or "Чат" not in text):
            raise AssertionError("narrow terminals must keep only the chat pane")
        if columns < 36 and "окно" not in text:
            raise AssertionError("terminals below the minimum size must show a compact warning")
        if len(lines) > rows:
            raise AssertionError(
                f"TUI rendered {len(lines)} physical lines in a {rows}-row terminal ({columns}x{rows})"
            )
        for index, line in enumerate(lines, 1):
            width = display_width(line)
            if width > columns:
                raise AssertionError(
                    f"TUI line {index} occupies {width} columns in a {columns}-column terminal "
                    f"({columns}x{rows}): {line!r}"
                )
    wide = "\n".join(render_at_size(binary, 120, 30))
    for title in ("TUI_TWO_CLIENT_SYNC_TODO", "TUI_TWO_CLIENT_SYNC_GOAL"):
        if title not in wide:
            raise AssertionError(
                f"wide right panel must render complete plan titles without clipping: {title!r}\n{wide}"
            )
    welcome = "\n".join(render_at_size(binary, 120, 30, "--render-welcome"))
    if "Активные задачи" not in welcome or "Task 00" not in welcome:
        raise AssertionError("welcome view must render active task tiles")
    picker = "\n".join(render_at_size(binary, 120, 30, "--render-session"))
    if "Выбор задачи · /session" not in picker or "Task 02" not in picker or "2026-10-21" not in picker:
        raise AssertionError("/session modal must show search results, task title, and date")
    print("TUI layout, welcome task tiles, and session picker fit tested terminal sizes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
