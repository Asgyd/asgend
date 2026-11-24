#!/usr/bin/env python3
"""The Cowork interpreter model must never become a project execution model."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
source = Path(sys.argv[2]).resolve()

def prepare(root: Path):
    for name in ("config", "skills", "knowledge", "system_prompts", "tools", "data", "ui"):
        (root / name).mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "config/config.json", root / "config/config.json")
    shutil.copytree(source / "config/agents", root / "config/agents", dirs_exist_ok=True)
    shutil.copy2(source / "config/models.json", root / "config/models.json")
    for name in ("skills", "knowledge", "system_prompts", "tools"):
        shutil.copytree(source / name, root / name, dirs_exist_ok=True)
    shutil.copytree(source / "ui", root / "ui", dirs_exist_ok=True)
    return root / "config/config.json", root / "config/models.json", root / "config/agents/generalist.json"

def run(config_path):
    return subprocess.run([str(binary), "--check-config", "-c", str(config_path)], text=True, capture_output=True, timeout=15)

with tempfile.TemporaryDirectory(prefix="asgend-voice-role-") as temp:
    root = Path(temp)
    config_path, models_path, agent_path = prepare(root)
    models = json.loads(models_path.read_text())
    models["defaults"]["model_id"] = "lfm3-omni-2b"
    models_path.write_text(json.dumps(models))
    result = run(config_path)
    assert result.returncode != 0, result.stdout + result.stderr
    assert "voice_interpreter" in result.stderr or "voice_interpreter" in result.stdout

with tempfile.TemporaryDirectory(prefix="asgend-voice-agent-") as temp:
    root = Path(temp)
    config_path, models_path, agent_path = prepare(root)
    agent = json.loads(agent_path.read_text())
    agent["model_id"] = "lfm3-omni-2b"
    agent_path.write_text(json.dumps(agent))
    result = run(config_path)
    assert result.returncode != 0, result.stdout + result.stderr
    assert "voice_interpreter" in result.stderr or "voice_interpreter" in result.stdout

print("voice_interpreter role guard passed for default model and agent assignment")
