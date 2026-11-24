#!/usr/bin/env python3
"""Static checks for PWA delivery and API non-caching policy."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ui = ROOT / "ui"
manifest = json.loads((ui / "manifest.webmanifest").read_text(encoding="utf-8"))
sw = (ui / "sw.js").read_text(encoding="utf-8")
index = (ui / "index.html").read_text(encoding="utf-8")
server = (ROOT / "src/server.c").read_text(encoding="utf-8")
assert manifest["display"] == "standalone"
assert manifest["scope"] == "/" and manifest["start_url"] == "/"
for icon in ("icon-192.png", "icon-512.png"):
    path = ui / icon
    assert path.is_file() and path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
    assert f'"/{icon}"' in manifest["icons"][0]["src"] or any(x["src"] == f"/{icon}" for x in manifest["icons"])
assert "request.method !== 'GET'" in sw
assert "url.pathname.startsWith('/api/')" in sw and "url.pathname.startsWith('/v1/')" in sw
assert "CACHEABLE" in sw and "caches.open" in sw
assert "rel=\"manifest\"" in index
for route in ("/manifest.webmanifest", "/sw.js", "/icon-192.png", "/icon-512.png"):
    assert route in server, f"missing static route {route}"
assert "connect-src 'self' http://127.0.0.1:8788 http://localhost:8788" in server
print("Cowork PWA and voice gateway CSP contract passed")
