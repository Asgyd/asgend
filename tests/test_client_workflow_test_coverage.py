#!/usr/bin/env python3
"""Negative tests for the client workflow command-coverage guard."""
from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUARD_PATH = ROOT / "tests" / "test_client_workflow_contract_triggers.py"
_spec = importlib.util.spec_from_file_location("client_workflow_contract_guard", GUARD_PATH)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"cannot load workflow guard at {GUARD_PATH}")
guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guard)


class WorkflowCommandCoverageTests(unittest.TestCase):
    def test_accepts_build_and_all_rpc_tests(self) -> None:
        workflow = "\n".join((
            "cmake -S . -B build-core-ci -DCMAKE_BUILD_TYPE=Release",
            "cmake --build build-core-ci --target agent-orchestrator --parallel 2",
            "run: ctest --test-dir build-core-ci --output-on-failure --timeout 75 -R '^(core_rpc_contract_inventory|core_rpc_runtime_contract|core_rpc_state_transitions)$'",
        ))
        self.assertEqual(guard.workflow_rpc_execution_issues(workflow), [])

    def test_rejects_path_trigger_without_test_command(self) -> None:
        workflow = "\n".join((
            "tests/test_core_rpc_runtime_contract.py",
            "tests/test_core_rpc_state_transitions.py",
            "cmake -S . -B build-core-ci -DCMAKE_BUILD_TYPE=Release",
            "cmake --build build-core-ci --target agent-orchestrator --parallel 2",
        ))
        issues = guard.workflow_rpc_execution_issues(workflow)
        self.assertEqual(len(issues), 1)
        self.assertIn("missing CTest command", issues[0])

    def test_rejects_ctest_command_that_omits_one_rpc_suite(self) -> None:
        workflow = "\n".join((
            "cmake -S . -B build-core-ci -DCMAKE_BUILD_TYPE=Release",
            "cmake --build build-core-ci --target agent-orchestrator --parallel 2",
            "ctest --test-dir build-core-ci -R '^(core_rpc_contract_inventory|core_rpc_runtime_contract)$'",
        ))
        issues = guard.workflow_rpc_execution_issues(workflow)
        self.assertEqual(len(issues), 1)
        self.assertIn("core_rpc_state_transitions", issues[0])


    def test_rejects_commands_only_in_comments(self) -> None:
        workflow = "\n".join((
            "# cmake -S . -B build-core-ci -DCMAKE_BUILD_TYPE=Release",
            "# cmake --build build-core-ci --target agent-orchestrator --parallel 2",
            "# run: ctest --test-dir build-core-ci --output-on-failure --timeout 75 -R '^(core_rpc_contract_inventory|core_rpc_runtime_contract|core_rpc_state_transitions)$'",
        ))
        issues = guard.workflow_rpc_execution_issues(workflow)
        self.assertEqual(len(issues), 3)

    def test_rejects_build_without_live_core_target(self) -> None:
        workflow = "\n".join((
            "cmake -S . -B build-core-ci -DCMAKE_BUILD_TYPE=Release",
            "cmake --build build-core-ci --target asgend-cli --parallel 2",
            "ctest --test-dir build-core-ci -R '^(core_rpc_contract_inventory|core_rpc_runtime_contract|core_rpc_state_transitions)$'",
        ))
        issues = guard.workflow_rpc_execution_issues(workflow)
        self.assertEqual(len(issues), 1)
        self.assertIn("agent-orchestrator", issues[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
