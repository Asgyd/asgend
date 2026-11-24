#!/usr/bin/env python3
"""Run the LLM SSE timeout/cancellation fixtures independently of full integration."""
import importlib.util
import sys
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
integration_script = PROJECT / "tests" / "integration.py"
saved_argv = sys.argv[:]
try:
    sys.argv = [str(integration_script), str(BINARY), str(PROJECT)]
    spec = importlib.util.spec_from_file_location("asgend_sse_watchdog_integration", integration_script)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load integration test helpers")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
finally:
    sys.argv = saved_argv

module.run_openai_sse_fault_cases(BINARY, PROJECT)
print("core LLM SSE watchdog tests passed", flush=True)
