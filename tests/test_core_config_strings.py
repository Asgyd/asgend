#!/usr/bin/env python3
"""Configuration parser must reject present invalid/overlong string fields."""
from __future__ import annotations
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()


def make_config(root: Path) -> dict:
    config = json.loads((PROJECT / "config/config.json").read_text(encoding="utf-8"))
    # Resolve source-tree references so the temporary config can run --check-config.
    paths = config.setdefault("paths", {})
    paths.update({
        "agents_dir": str(PROJECT / "config/agents"),
        "models_config": str(PROJECT / "config/models.json"),
        "asgend_home": str(root / "asgend-home"),
        "skills_dir": str(PROJECT / "skills"),
        "knowledge_dir": str(PROJECT / "knowledge"),
        "system_prompts_dir": str(PROJECT / "system_prompts"),
        "tools_dir": str(PROJECT / "tools"),
        "instructions_dir": str(root / "instructions"),
        "project_root": str(PROJECT),
    })
    runtime = config.setdefault("runtime", {})
    runtime["database_path"] = str(root / "tasks.sqlite3")
    server = config.setdefault("server", {})
    server["port"] = 18765
    server["unix_socket"] = {"enabled": False, "path": str(root / "core.sock")}
    return config


def run_check(config: dict, root: Path) -> subprocess.CompletedProcess[str]:
    path = root / "test-config.json"
    path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
    return subprocess.run([str(BINARY), "--check-config", "-c", str(path)],
                          capture_output=True, text=True, timeout=8, check=False)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="ao-core-config-strings-") as td:
        root = Path(td)
        valid = make_config(root)
        valid["server"].pop("auth_token", None)  # absent optional value still defaults safely
        result = run_check(valid, root)
        assert result.returncode == 0, result.stderr

        bad_cases = [
            ("server.auth_token", lambda cfg: cfg["server"].update({"auth_token": "SECRET_MARKER_" + "x" * 300})),
            ("server.auth_token", lambda cfg: cfg["server"].update({"auth_token": 37})),
            ("ui.auth_token", lambda cfg: cfg["ui"].update({"auth_token": ["bad-type"]})),
            ("local_llm.base_url", lambda cfg: cfg["local_llm"].update({"base_url": "https://example.invalid/" + "a" * 600})),
            ("paths.agents_dir", lambda cfg: cfg["paths"].update({"agents_dir": "a" * 1400})),
        ]
        for label, mutate in bad_cases:
            candidate = copy.deepcopy(make_config(root))
            mutate(candidate)
            outcome = run_check(candidate, root)
            assert outcome.returncode != 0, f"configuration unexpectedly accepted invalid {label}"
            assert label in outcome.stderr, (label, outcome.stderr)
            assert "SECRET_MARKER" not in outcome.stderr, "secret value leaked in config diagnostic"
    print("core config string validation tests passed")


if __name__ == "__main__":
    main()
