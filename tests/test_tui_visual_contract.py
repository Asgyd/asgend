#!/usr/bin/env python3
"""Guard the borderless Catppuccin Mocha visual contract."""
from __future__ import annotations
import pathlib
import sys
import re

def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_tui_visual_contract.py PROJECT_ROOT", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])
    tui = (root / "src/asgend_tui.c").read_text(encoding="utf-8")
    cli = (root / "clients/asgend_cli.c").read_text(encoding="utf-8")
    for color in ((203,166,247), (243,139,168), (166,227,161), (205,214,244), (30,30,46), (49,50,68), (69,71,90), (88,91,112)):
        pattern = r"\{\s*" + r"\s*,\s*".join(map(str, color)) + r"\s*\}"
        assert re.search(pattern, tui), f"missing Catppuccin Mocha RGB color {color}"
    assert "tui_file_icon" in tui, "file extension icon mapping is missing"
    assert "usable_width >= 100" in tui, "right pane responsive threshold is missing"
    assert "usable_width >= 68" in tui, "left pane responsive threshold is missing"
    outlines = "╭╮╰╯│─┌┐└┘"
    assert not any(char in tui for char in outlines), "TUI source contains box-drawing outlines"
    assert not any(char in cli for char in outlines), "CLI source contains box-drawing outlines"
    assert "Thinking %s\\n" in cli and "байт потока" not in cli
    print("Catppuccin palette, file icons, responsive panels, and borderless contract verified")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
