#!/usr/bin/env python3
"""Upgrade replaces all app-owned web/PWA files and preserves user files."""
from pathlib import Path
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
SYNC = PROJECT / "scripts" / "update-ui-assets.sh"
ASSETS = ("index.html", "app.js", "style.css", "manifest.webmanifest", "sw.js", "icon-192.png", "icon-512.png")

with tempfile.TemporaryDirectory(prefix="asgend-ui-upgrade-") as tmp:
    root = Path(tmp)
    src = root / "source-ui"
    dst = root / "installed-ui"
    src.mkdir()
    dst.mkdir()
    for name in ASSETS:
        (src / name).write_bytes(f"new-{name}\n".encode())
        (dst / name).write_bytes(f"old-{name}\n".encode())
    custom = dst / "custom-not-owned-by-asgend.txt"
    custom.write_text("keep me\n", encoding="utf-8")
    subprocess.run([str(SYNC), str(src), str(dst)], check=True, capture_output=True, text=True)
    for name in ASSETS:
        assert (dst / name).read_bytes() == f"new-{name}\n".encode(), name
        assert (dst / name).stat().st_mode & 0o777 == 0o644
    assert custom.read_text(encoding="utf-8") == "keep me\n"
    assert sorted(p.name for p in dst.iterdir()) == sorted((*ASSETS, custom.name))
print("UI asset upgrade regression passed: web/PWA assets updated; custom asset preserved")
