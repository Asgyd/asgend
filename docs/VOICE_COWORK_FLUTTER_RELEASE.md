# Voice, Cowork and Flutter workspace release notes

## Inference architecture

- LFM3-Omni-2B is loaded by `llama-server` from GGUF and its multimodal projector.
- The Python voice gateway is a standard-library HTTP client and report/session coordinator. It does not load model weights and has no PyTorch or Transformers dependency.
- Voice reports are revisioned. Any new user input invalidates older report revisions. The main ASGEND task queue is called only after the user confirms the latest revision.
- `voice_interpreter` is reserved for report preparation and cannot be configured as the project default, analysis model, or an execution agent's model.

See [COWORK_VOICE_MODE.md](COWORK_VOICE_MODE.md) for llama-server launch flags, ports, environment, API, and security.

## Agent workspace tools

The `artifact_workspace` MCP server exposes list, read, write, Office creation and visual inspection tools. Office creation generates genuine DOCX, XLSX, PPTX and PDF files. `render_and_inspect` produces a PNG and returns it as MCP image content; Core forwards the image to the active vision-capable model so the agent can review and iterate on its output.

The renderer handles HTML/CSS/SVG at desktop/tablet/mobile viewport sizes; Office/PDF pages and presentation slides can be rendered individually. The renderer runs with JavaScript disabled and without external IP networking. It is a static visual QA surface, not a live application runtime.

See [ARTIFACT_WORKSPACE.md](ARTIFACT_WORKSPACE.md) for tool schemas, document examples, dependencies, and limitations.

## Flutter layout

Local Core and remote Control Plane task screens use a consistent adaptive layout:

- Desktop / large tablet at 1200 logical px and wider: task navigation at left, task conversation/details in the center, agent files and visual preview at right.
- Tablet below 1200 logical px: full-width task conversation/details with files available in the page flow.
- Phone: compact task view; selecting an artifact opens a draggable bottom sheet with zoom/pan and source/visual switching.

The Flutter app shows agent-produced screenshots and source text. It does not execute arbitrary workspace HTML or project code. For native Flutter/Dart UI tasks, the agent should retain Dart as the production source and create a self-contained HTML responsive companion preview for visual design inspection unless an emulator/device capture runner is configured. A companion preview is not a native Flutter runtime screenshot.

## Verification recorded for this source package

- Voice gateway unit/HTTP tests: 15 passed.
- Artifact workspace/Office tests: 10 passed.
- Python API contract tests: 36 passed, 1 skipped.
- Flutter layout source-contract tests: 5 passed.
- C Release target build and Core configuration check: passed.
- Focused Core CTest suite (`core_mcp_launcher_smoke`, `cowork_pwa_contract`, `voice_gateway_contract`, `voice_model_role_guard`): 4 passed.
- Workflow YAML and Dart delimiter/string/comment source checks: passed.

The Flutter SDK was not installed in the packaging environment. `flutter analyze`, Flutter widget tests, and Android APK build are configured in `.github/workflows/flutter-android.yml` using Flutter 3.29.3 and must run in CI before publishing an APK.
