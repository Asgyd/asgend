#!/usr/bin/env python3
"""Check that Stage 31's CTest/scanner/timeout test processes are no longer alive."""
from __future__ import annotations

import json
import os
from datetime import datetime, timezone
from pathlib import Path


def process_info(pid: int) -> dict | None:
    base = Path('/proc') / str(pid)
    try:
        comm = (base / 'comm').read_text().strip()
        raw = (base / 'cmdline').read_bytes()
        argv = [part.decode('utf-8', 'replace') for part in raw.split(b'\0') if part]
        stat = (base / 'stat').read_text().split()
        state = stat[2]
        ppid = int(stat[3])
    except (OSError, ValueError, IndexError):
        return None
    return {'pid': pid, 'ppid': ppid, 'state': state, 'comm': comm, 'argv': argv}


def main() -> int:
    findings = []
    own_pid = os.getpid()
    own_ppid = os.getppid()
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        info = process_info(pid)
        if not info or pid in {own_pid, own_ppid} or info['state'] == 'Z':
            continue
        argv = info['argv']
        cmd = ' '.join(argv)
        comm = info['comm']
        target = False
        if comm == 'ctest' and '/mnt/data/asgend-stage31-build' in cmd:
            target = True
        if comm in {'python', 'python3'} and any(
            arg.endswith(('tests/integration.py', 'tests/test_memory_scan_timeout.py',
                          'scripts/benchmark_memory_scan_index.py')) for arg in argv
        ):
            target = True
        if comm == 'asgend-memory' and 'scan' in argv:
            target = True
        if comm == 'agent-orchestrator' and any('/mnt/data/asgend-stage31-build/' in arg for arg in argv):
            target = True
        if comm == 'sleep' and argv[1:] == ['30']:
            target = True
        if target:
            findings.append({k: info[k] for k in ('pid', 'ppid', 'state', 'comm', 'argv')})

    result = {
        'generated_utc': datetime.now(timezone.utc).isoformat(),
        'status': 'PASS' if not findings else 'FAIL',
        'checked_process_families': ['CTest', 'integration.py', 'test_memory_scan_timeout.py', 'benchmark_memory_scan_index.py', 'asgend-memory scan', 'stage31 agent-orchestrator child', 'sleep 30'],
        'remaining_matching_processes': findings,
    }
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if not findings else 1


if __name__ == '__main__':
    raise SystemExit(main())
