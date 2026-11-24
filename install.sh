#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}/asgend"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}/asgend"
BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"
LOCAL_PREFIX="$(dirname -- "$BIN_HOME")"
LIB_HOME="${XDG_LIB_HOME:-$LOCAL_PREFIX/lib}"
INCLUDE_HOME="${XDG_INCLUDE_HOME:-$LOCAL_PREFIX/include}"
BUILD_DIR="$ROOT_DIR/build-release"

for tool in cmake ctest python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing required tool: $tool" >&2; exit 1; }
done

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

install -d -m 700 "$CONFIG_HOME" "$CONFIG_HOME/data" "$CONFIG_HOME/orchestrator/agents" "$CONFIG_HOME/instructions"
install -d -m 755 "$CONFIG_HOME/skills" "$CONFIG_HOME/knowledge" "$CONFIG_HOME/prompts" "$CONFIG_HOME/tools" "$CONFIG_HOME/ui" "$DATA_HOME" "$BIN_HOME" "$LIB_HOME" "$INCLUDE_HOME"
install -m 755 "$BUILD_DIR/agent-orchestrator" "$BIN_HOME/asgend-orchestrator"
install -m 755 "$BUILD_DIR/asgend-cli" "$BIN_HOME/asgend-cli"
install -m 755 "$BUILD_DIR/asgend-memory" "$BIN_HOME/asgend-memory"
# Publish the reusable TUI framework for additional C clients as well as statically linking it into the CLI.
install -m 644 "$BUILD_DIR/libasgend_tui.a" "$LIB_HOME/libasgend_tui.a"
install -m 644 "$BUILD_DIR/libasgend_memory.a" "$LIB_HOME/libasgend_memory.a"
install -m 644 "$ROOT_DIR/include/asgend_tui.h" "$INCLUDE_HOME/asgend_tui.h"
install -m 644 "$ROOT_DIR/include/asgend_memory.h" "$INCLUDE_HOME/asgend_memory.h"

# Never overwrite an existing user file. Populate only missing defaults.
if [[ ! -e "$CONFIG_HOME/config.json" ]]; then
  install -m 600 "$ROOT_DIR/config/config.json" "$CONFIG_HOME/config.json"
fi
if [[ ! -e "$CONFIG_HOME/models.json" ]]; then
  install -m 600 "$ROOT_DIR/config/models.json" "$CONFIG_HOME/models.json"
fi
for src in "$ROOT_DIR/config/agents/"*.json; do
  [[ -f "$src" ]] || continue
  dst="$CONFIG_HOME/orchestrator/agents/$(basename "$src")"
  [[ -e "$dst" ]] || install -m 600 "$src" "$dst"
done
for item in skills knowledge tools; do
  if [[ -d "$ROOT_DIR/$item" ]]; then
    cp -an "$ROOT_DIR/$item/." "$CONFIG_HOME/$item/"
  fi
done
cp -an "$ROOT_DIR/system_prompts/." "$CONFIG_HOME/prompts/"
cp -an "$ROOT_DIR/instructions/." "$CONFIG_HOME/instructions/"
# Always refresh the application-owned frontend assets on upgrade. cp -an would
# leave old versions in place forever when the target already exists.
"$ROOT_DIR/scripts/update-ui-assets.sh" "$ROOT_DIR/ui" "$CONFIG_HOME/ui"
cp -an "$ROOT_DIR/clients/asgend_flutter" "$DATA_HOME/"

# Align relative paths in old and fresh configurations with the canonical installed layout.
# This also repairs the prior config/agents vs orchestrator/agents mismatch without deleting data.
python3 - "$CONFIG_HOME" <<'PY'
import json
import secrets
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
config_path = root / "config.json"
config = json.loads(config_path.read_text(encoding="utf-8"))
paths = config.setdefault("paths", {})

def choose_dir(key, candidates, default):
    current = paths.get(key, "")
    if current:
        p = Path(current)
        p = p if p.is_absolute() else root / p
        if p.is_dir():
            paths[key] = str(p.relative_to(root)) if p.is_relative_to(root) else str(p)
            return
    for candidate in candidates:
        p = root / candidate
        if p.is_dir():
            paths[key] = candidate
            return
    paths[key] = default

choose_dir("agents_dir", ["orchestrator/agents", "agents", "config/agents"], "orchestrator/agents")
choose_dir("skills_dir", ["skills"], "skills")
choose_dir("knowledge_dir", ["knowledge"], "knowledge")
choose_dir("system_prompts_dir", ["prompts", "system_prompts"], "prompts")
choose_dir("tools_dir", ["tools"], "tools")

models_path = paths.get("models_config", "")
mp = Path(models_path) if models_path else Path("nonexistent")
if not (mp if mp.is_absolute() else root / mp).is_file():
    paths["models_config"] = "models.json" if (root / "models.json").is_file() else ""
else:
    resolved = mp if mp.is_absolute() else root / mp
    paths["models_config"] = str(resolved.relative_to(root)) if resolved.is_relative_to(root) else str(resolved)
paths["instructions_dir"] = str(root / "instructions")

ui = config.setdefault("ui", {})
ui.setdefault("enabled", False)
ui["directory"] = "ui"
server = config.setdefault("server", {})
server.setdefault("host", "127.0.0.1")
server.setdefault("port", 8765)
if not server.get("auth_token"):
    server["auth_token"] = secrets.token_hex(32)

config_path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
config_path.chmod(0o600)
models_file = root / paths.get("models_config", "models.json")
if models_file.exists():
    models_file.chmod(0o600)
print("Configuration paths aligned:")
for key in ("agents_dir", "models_config", "skills_dir", "knowledge_dir", "system_prompts_dir", "tools_dir", "instructions_dir"):
    print(f"  {key}: {paths[key]}")
print("UI remains disabled by default.")
print("ASGEND_CORE_TOKEN=" + server["auth_token"])
PY

"$BIN_HOME/asgend-orchestrator" --check-config -c "$CONFIG_HOME/config.json"
cat <<EOF2

ASGEND installed successfully.
Core: $BIN_HOME/asgend-orchestrator -c "$CONFIG_HOME/config.json"
CLI:  $BIN_HOME/asgend-cli --help
Memory index: $BIN_HOME/asgend-memory scan /path/to/project
TUI library: $LIB_HOME/libasgend_tui.a
TUI header:  $INCLUDE_HOME/asgend_tui.h
Memory library: $LIB_HOME/libasgend_memory.a
Memory header:  $INCLUDE_HOME/asgend_memory.h
Config directory: $CONFIG_HOME
Flutter source: $DATA_HOME/asgend_flutter

The core TCP listener defaults to 127.0.0.1:8765. Keep it local unless you set up a VPN/SSH tunnel/firewall and use the token printed above. Do not expose raw TCP to the public Internet.
The model profiles are examples and disabled until executable/weights/endpoints are configured. The web UI remains disabled until configured with a separate UI token.
EOF2
