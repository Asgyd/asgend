#!/usr/bin/env python3
"""Guard the source-level responsive layout contract for the Flutter Cowork UI.

Flutter widget/rendering tests remain authoritative in CI. This dependency-free
check catches accidental divergence between local and remote task workspaces.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
LOCAL = ROOT / "clients/asgend_flutter/lib/src/ui/local/local_tasks.dart"
REMOTE = ROOT / "clients/asgend_flutter/lib/src/ui/remote/remote_home.dart"
DOC = ROOT / "docs/FLUTTER_AGENT_WORKSPACE.md"


class FlutterWorkspaceLayoutContract(unittest.TestCase):
    def test_local_workspace_uses_three_panes_at_desktop_breakpoint(self):
        source = LOCAL.read_text(encoding="utf-8")
        self.assertIn("final wide = constraints.maxWidth >= 1200;", source)
        self.assertIn("SizedBox(width: 238, child: _taskSidebar())", source)
        self.assertIn("Expanded(flex: 3, child: chat)", source)
        self.assertIn("Expanded(flex: 2, child: Padding(padding: const EdgeInsets.all(12), child: _workspacePane()))", source)

    def test_remote_workspace_uses_same_three_pane_contract(self):
        source = REMOTE.read_text(encoding="utf-8")
        self.assertIn("final wide = constraints.maxWidth >= 1200;", source)
        self.assertIn("SizedBox(width: 238, child: _taskSidebar())", source)
        self.assertIn("Expanded(flex: 3, child: details)", source)
        self.assertIn("Expanded(flex: 2, child: Padding(padding: const EdgeInsets.all(12), child: _workspacePane()))", source)

    def test_phone_layout_keeps_draggable_preview_sheet(self):
        for path in (LOCAL, REMOTE):
            source = path.read_text(encoding="utf-8")
            self.assertIn("showModalBottomSheet<void>", source, str(path))
            self.assertIn("DraggableScrollableSheet(", source, str(path))

    def test_workspace_can_show_source_and_visual_artifact(self):
        for path in (LOCAL, REMOTE):
            source = path.read_text(encoding="utf-8")
            self.assertIn("SelectableText(", source, str(path))
            self.assertTrue("Image.memory(" in source or "Image.file(" in source, str(path))

    def test_documentation_matches_breakpoint_and_panels(self):
        source = DOC.read_text(encoding="utf-8")
        self.assertIn("1200 logical pixels", source)
        self.assertIn("three-pane", source)
        self.assertIn("draggable bottom sheet", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
