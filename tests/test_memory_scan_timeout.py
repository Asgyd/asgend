#!/usr/bin/env python3
"""Regression guard: timed-out scanner subprocesses must not leak processes or pipes."""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from memory_scan_runner import run_command  # noqa: E402


def proc_state(pid: int) -> str | None:
    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        # The command field is parenthesized and may contain spaces; state is after ') '.
        return text[text.rfind(") ") + 2:].split()[0]
    except (FileNotFoundError, ProcessLookupError, PermissionError, IndexError):
        return None


def main() -> int:
    # The grandchild deliberately inherits stdout/stderr, which would keep pipes open
    # if timeout cleanup only killed/reaped the direct child.
    child = (
        "import subprocess,sys,time; "
        "p=subprocess.Popen(['sleep','30']); "
        "print('grandchild_pid='+str(p.pid),flush=True); "
        "time.sleep(30)"
    )
    result = run_command([sys.executable, "-c", child], timeout_seconds=1.0, poll_seconds=0.002)
    assert result["timed_out"] is True, result
    assert result["returncode"] is not None and result["returncode"] != 0, result
    assert result["pipes_closed"] is True, result
    assert result["wall_seconds"] < 3.0, result
    marker = "grandchild_pid="
    assert marker in result["stdout"], result
    grandchild_pid = int(result["stdout"].split(marker, 1)[1].splitlines()[0])
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        state = proc_state(grandchild_pid)
        if state is None or state == "Z":
            break
        time.sleep(0.02)
    state = proc_state(grandchild_pid)
    assert state is None or state == "Z", f"timed-out grandchild still running: pid={grandchild_pid}, state={state}"
    print("timeout cleanup passed: process group terminated, grandchild not running, stdout/stderr pipes closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
