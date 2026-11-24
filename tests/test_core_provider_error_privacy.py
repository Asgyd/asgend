#!/usr/bin/env python3
"""Runtime regression: upstream HTTP error body must not leak to persisted errors/logs."""
from __future__ import annotations
import importlib.util
import sys
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
script = PROJECT / "tests" / "integration.py"
saved_argv = sys.argv[:]
try:
    sys.argv = [str(script), str(BINARY), str(PROJECT)]
    spec = importlib.util.spec_from_file_location("asgend_core_privacy_integration", script)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load integration helpers")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
finally:
    sys.argv = saved_argv

module.run_openai_sse_error_privacy_case(BINARY, PROJECT)
print("core provider error privacy test passed")
