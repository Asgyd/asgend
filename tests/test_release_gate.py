#!/usr/bin/env python3
"""Regression tests for ASGEND's 1.0.0 release gate tooling."""
from __future__ import annotations

import importlib.util
import json
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "check_release_readiness.py"
SPEC = importlib.util.spec_from_file_location("check_release_readiness", MODULE_PATH)
assert SPEC and SPEC.loader
release_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release_gate)


class ReleaseGateTests(unittest.TestCase):
    def test_declared_component_versions_match_all_sources(self):
        self.assertEqual(release_gate.audit_versions(ROOT), [])

    def test_open_required_gate_and_missing_approval_keep_release_blocked(self):
        versions = {"release_approved": False, "release_channel": "release-candidate"}
        gates = {
            "release_ready": True,
            "gates": [
                {"id": "postgres_acceptance", "title": "PostgreSQL", "required": True,
                 "status": "blocked", "evidence": "no hosted run"},
            ],
        }
        blockers = release_gate.release_blockers(gates, [], versions)
        self.assertTrue(any("postgres_acceptance" in item for item in blockers))
        self.assertIn("version manifest release_approved=false", blockers)

    def test_repository_gate_manifest_is_valid(self):
        gates = release_gate.load_gates(ROOT)
        self.assertTrue(any(g["required"] for g in gates["gates"]))
        for gate in gates["gates"]:
            if gate["status"] == "passed":
                self.assertTrue(gate.get("verified_at"), f"{gate['id']} passed without verified_at")

    def test_optional_open_gate_does_not_block_once_required_gates_pass(self):
        versions = {
            "release_approved": True,
            "release_channel": "stable",
        }
        gates = {
            "release_ready": True,
            "gates": [
                {"id": "required", "title": "Required", "required": True,
                 "status": "passed", "evidence": "run #123"},
                {"id": "optional", "title": "Optional", "required": False,
                 "status": "not_run", "evidence": "out of scope"},
            ],
        }
        self.assertEqual(release_gate.release_blockers(gates, [], versions), [])

    def test_gate_manifest_rejects_duplicate_ids(self):
        gates = {
            "gates": [
                {"id": "same", "required": True, "status": "passed", "evidence": "x"},
                {"id": "same", "required": True, "status": "passed", "evidence": "y"},
            ]
        }
        self.assertRaisesRegex(RuntimeError, "duplicate release gate id",
                               release_gate.validate_gates, gates)

    def test_strict_mode_fails_while_required_release_gates_are_open(self):
        output = StringIO()
        with mock.patch.object(release_gate, "release_blockers", return_value=["gate: open"]), \
                redirect_stdout(output):
            result = release_gate.main(["--strict"])
        self.assertEqual(result, 2)
        self.assertIn("BLOCKED", output.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
