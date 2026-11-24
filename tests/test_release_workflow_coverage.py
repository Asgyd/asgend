#!/usr/bin/env python3
"""Static guard: release gate must call all acceptance workflows and require success."""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / ".github" / "workflows" / "release-gate.yml"
REUSABLE = (
    ".github/workflows/control-plane-node-e2e.yml",
    ".github/workflows/native-android.yml",
    ".github/workflows/flutter-android.yml",
    ".github/workflows/qt-client.yml",
)


def release_workflow_issues(root: Path = ROOT) -> list[str]:
    issues: list[str] = []
    release_path = root / ".github" / "workflows" / "release-gate.yml"
    if not release_path.is_file():
        return ["release-gate workflow is missing"]
    release = release_path.read_text(encoding="utf-8")
    if "python scripts/check_release_readiness.py --strict" not in release:
        issues.append("release workflow must retain strict metadata validation")
    for rel in REUSABLE:
        path = root / rel
        if not path.is_file():
            issues.append(f"reusable acceptance workflow missing: {rel}")
            continue
        text = path.read_text(encoding="utf-8")
        if not re.search(r"(?m)^  workflow_call:\s*$", text):
            issues.append(f"workflow is not callable: {rel}")
        if f"uses: ./{rel}" not in release:
            issues.append(f"release gate does not call acceptance workflow: {rel}")
    required_summary = (
        "release-verification-summary:",
        "${{ always() }}",
        "      - metadata-gate",
        "      - control-plane-core",
        "      - native-android",
        "      - flutter-android",
        "      - qt-client",
        "needs.metadata-gate.result",
        "needs.control-plane-core.result",
        "needs.native-android.result",
        "needs.flutter-android.result",
        "needs.qt-client.result",
        '"$result" != "success"',
    )
    for marker in required_summary:
        if marker not in release:
            issues.append(f"release summary is missing mandatory result check: {marker}")
    return issues


class ReleaseWorkflowCoverageTests(unittest.TestCase):
    def test_release_workflow_reuses_all_acceptance_suites(self) -> None:
        self.assertEqual(release_workflow_issues(), [])

    def test_guard_rejects_missing_client_workflow_call(self) -> None:
        # Validate against an in-memory copy of the project tree using a minimal patch
        # to ensure the guard is sensitive to a missing reusable workflow declaration.
        with __import__("tempfile").TemporaryDirectory() as tmp:
            import shutil
            copy_root = Path(tmp)
            shutil.copytree(ROOT / ".github", copy_root / ".github")
            target = copy_root / ".github/workflows/qt-client.yml"
            target.write_text(target.read_text(encoding="utf-8").replace("  workflow_call:\n", "", 1), encoding="utf-8")
            issues = release_workflow_issues(copy_root)
            self.assertTrue(any("workflow is not callable" in issue and "qt-client.yml" in issue for issue in issues))

    def test_guard_rejects_summary_that_does_not_wait_for_all_jobs(self) -> None:
        with __import__("tempfile").TemporaryDirectory() as tmp:
            import shutil
            copy_root = Path(tmp)
            shutil.copytree(ROOT / ".github", copy_root / ".github")
            target = copy_root / ".github/workflows/release-gate.yml"
            text = target.read_text(encoding="utf-8").replace("      - qt-client\n", "", 1)
            target.write_text(text, encoding="utf-8")
            issues = release_workflow_issues(copy_root)
            self.assertTrue(any("mandatory result check" in issue and "qt-client" in issue for issue in issues))

    def test_guard_rejects_summary_that_does_not_require_success(self) -> None:
        with __import__("tempfile").TemporaryDirectory() as tmp:
            import shutil
            copy_root = Path(tmp)
            shutil.copytree(ROOT / ".github", copy_root / ".github")
            target = copy_root / ".github/workflows/release-gate.yml"
            text = target.read_text(encoding="utf-8").replace('if: ${{ always() }}', 'if: false', 1)
            target.write_text(text, encoding="utf-8")
            issues = release_workflow_issues(copy_root)
            self.assertTrue(any("mandatory result check" in issue and "${{ always() }}" in issue for issue in issues))


if __name__ == "__main__":
    unittest.main(verbosity=2)
