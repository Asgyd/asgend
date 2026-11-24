# ASGEND Voice / Cowork / Flutter workspace package

This source package includes the staged work below:

1. `llama-server` backed LFM3-Omni voice report gateway. No PyTorch or Transformers inference runtime is used.
2. Sandboxed MCP artifact tools: workspace list/read/write, real Office/PDF creation, responsive HTML rendering, PNG screenshot return to the active model.
3. Flutter local and remote TUI-inspired task workspaces: task navigation / task details / artifact preview at >=1200 logical px; tablet flow and draggable mobile preview sheet below the breakpoint.
4. CI contract check for layout consistency and release/integration documentation.

## Key sources

- `docs/COWORK_VOICE_MODE.md`
- `docs/ARTIFACT_WORKSPACE.md`
- `docs/FLUTTER_AGENT_WORKSPACE.md`
- `docs/VOICE_COWORK_FLUTTER_RELEASE.md`

## Validation

- `python3 -m unittest discover -s tests/voice_gateway -v`
- `python3 -m unittest discover -s tests/artifact_workspace -v`
- `python3 tests/contracts/test_flutter_workspace_layout.py`
- `python3 tests/contracts/test_contracts.py`
- `cmake --build <build-dir> --target agent-orchestrator -j2`
- `agent-orchestrator -c config/config.json --check-config`
- CTest focused suite: `core_mcp_launcher_smoke`, `cowork_pwa_contract`, `voice_gateway_contract`, `voice_model_role_guard`

Flutter SDK 3.29.3 checks and Android APK build run in `.github/workflows/flutter-android.yml`. This packaging environment did not contain Flutter/Dart SDK; do not treat an APK build as completed until CI passes.

Native Flutter screenshot capture is not claimed. Flutter/Dart source is retained and an HTML responsive companion may be rendered for design inspection unless an emulator/device runner is configured.
