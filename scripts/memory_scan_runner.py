#!/usr/bin/env python3
"""Bounded process runner for the ASGEND memory scan benchmark.

Captures stdout/stderr without leaving pipes or descendants behind on timeout.
Peak RSS is sampled from Linux /proc/<pid>/status VmHWM while the child lives.
"""
from __future__ import annotations

import os
import selectors
import signal
import subprocess
import time
from typing import Any, Sequence


def _read_vmhwm_kib(pid: int) -> int | None:
    try:
        with open(f"/proc/{pid}/status", "rt", encoding="ascii") as stream:
            for line in stream:
                if line.startswith("VmHWM:"):
                    fields = line.split()
                    if len(fields) >= 2:
                        return int(fields[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
        return None
    return None


def _signal_group(pgid: int, sig: int) -> None:
    try:
        os.killpg(pgid, sig)
    except ProcessLookupError:
        pass
    except PermissionError:
        # A child that already exited can race with process-group cleanup.
        pass


def run_command(argv: Sequence[str], timeout_seconds: float, poll_seconds: float = 0.002) -> dict[str, Any]:
    """Run one command with captured output, elapsed time, peak RSS, and timeout cleanup.

    A new session gives the child its own process group. On timeout the whole group
    is terminated, then killed if necessary. Pipes are always drained/closed and the
    direct child is always waited for before returning.
    """
    if not argv or timeout_seconds <= 0 or poll_seconds <= 0:
        raise ValueError("argv must be non-empty and timeout/poll intervals must be positive")

    started = time.monotonic()
    deadline = started + timeout_seconds
    proc = subprocess.Popen(
        list(argv), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        stdin=subprocess.DEVNULL, bufsize=0, close_fds=True, start_new_session=True,
    )
    assert proc.stdout is not None and proc.stderr is not None
    selector = selectors.DefaultSelector()
    captured = {"stdout": bytearray(), "stderr": bytearray()}
    for name, pipe in (("stdout", proc.stdout), ("stderr", proc.stderr)):
        os.set_blocking(pipe.fileno(), False)
        selector.register(pipe, selectors.EVENT_READ, name)

    peak_rss_kib = 0
    timed_out = False
    term_sent_at: float | None = None
    kill_sent = False
    hard_cleanup_deadline: float | None = None
    try:
        # Read VmHWM immediately; the child process exists even before exec finishes.
        while True:
            sample = _read_vmhwm_kib(proc.pid)
            if sample is not None:
                peak_rss_kib = max(peak_rss_kib, sample)

            now = time.monotonic()
            if not timed_out and now >= deadline:
                timed_out = True
                term_sent_at = now
                hard_cleanup_deadline = now + 1.0
                _signal_group(proc.pid, signal.SIGTERM)
            elif timed_out and not kill_sent and term_sent_at is not None and now >= term_sent_at + 0.20:
                _signal_group(proc.pid, signal.SIGKILL)
                kill_sent = True

            wait_interval = poll_seconds
            if not timed_out:
                wait_interval = min(wait_interval, max(0.0, deadline - now))
            events = selector.select(wait_interval) if selector.get_map() else []
            for key, _mask in events:
                pipe = key.fileobj
                name = key.data
                try:
                    chunk = os.read(pipe.fileno(), 65536)
                except BlockingIOError:
                    continue
                except OSError:
                    chunk = b""
                if chunk:
                    captured[name].extend(chunk)
                else:
                    try:
                        selector.unregister(pipe)
                    except Exception:
                        pass
                    pipe.close()

            child_done = proc.poll() is not None
            pipes_done = not selector.get_map()
            if child_done and pipes_done:
                break
            if timed_out and hard_cleanup_deadline is not None and time.monotonic() >= hard_cleanup_deadline:
                # Defensive last resort: close every remaining pipe rather than hanging
                # indefinitely if a descendant escaped the process group.
                _signal_group(proc.pid, signal.SIGKILL)
                for key in list(selector.get_map().values()):
                    pipe = key.fileobj
                    try:
                        selector.unregister(pipe)
                    except Exception:
                        pass
                    try:
                        pipe.close()
                    except OSError:
                        pass
                break

        try:
            returncode = proc.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            _signal_group(proc.pid, signal.SIGKILL)
            returncode = proc.wait(timeout=1.0)
    finally:
        for key in list(selector.get_map().values()):
            pipe = key.fileobj
            try:
                selector.unregister(pipe)
            except Exception:
                pass
            try:
                pipe.close()
            except OSError:
                pass
        selector.close()
        for pipe in (proc.stdout, proc.stderr):
            if pipe and not pipe.closed:
                pipe.close()

    # If VmHWM could not be sampled (e.g. an unusually short-lived executable), keep
    # the value null rather than substituting an unrelated cumulative RSS measurement.
    return {
        "pid": proc.pid,
        "returncode": returncode,
        "timed_out": timed_out,
        "wall_seconds": time.monotonic() - started,
        "peak_rss_kib": peak_rss_kib or None,
        "rss_source": "linux_proc_vm_hwm" if peak_rss_kib else None,
        "stdout": captured["stdout"].decode("utf-8", errors="replace"),
        "stderr": captured["stderr"].decode("utf-8", errors="replace"),
        "pipes_closed": proc.stdout.closed and proc.stderr.closed and not selector.get_map(),
    }
