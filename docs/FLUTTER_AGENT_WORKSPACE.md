# Flutter task workspace layout

Both local Core and remote Control Plane task detail screens expose a responsive `Agent workspace` alongside the task conversation/details:

- At widths of 1200 logical pixels or more, the view uses a three-pane TUI-inspired layout: task navigation on the left, task conversation/details in the center, and the agent workspace with file list + visual preview on the right.
- On tablet widths below 1200 logical pixels, task details remain full-width and the workspace file list is available in the flow. On phone widths, the task/details view stays full-width. Selecting a project artifact opens a draggable bottom sheet with the preview, zoom/pan, and source toggle where source text is available.
- Artifact metadata is refreshed periodically for local tasks and through the remote task follower for remote tasks. Binary previews are transferred in bounded chunks locally or downloaded through the existing verified remote ArtifactDownloader.
- Office/PDF/HTML/UI artifacts show an agent-generated PNG companion produced by the `render_and_inspect` MCP tool. Code files can also be opened as source text. Image artifacts are displayed directly and support zooming.
- The Flutter client does not execute HTML, scripts, or project code. It displays sandbox-generated screenshots, office-document previews, images and source text. It shows the saved render created inside the isolated task workspace. Export/download remains an explicit user action and potentially executable artifacts receive a warning.

The UI intentionally distinguishes a screenshot from a live editor: the application displays the generated artifact and visual snapshot. Editing is performed by the ASGEND agent through the task-workspace MCP tools, and a new snapshot appears after the next render. A source file's existence alone is not represented as a successful visual preview.

## Backend requirements

The Core must expose `artifact_list` and bounded `workspace_read_bytes` operations to local clients. The Control Plane must expose artifacts through its authenticated metadata/download flow. Keep task/workspace authorization and the existing artifact SHA-256 verification in place.

## Flutter verification

The project CI pins Flutter 3.29.3 and runs `flutter pub get`, `flutter analyze --no-pub`, `flutter test --no-pub`, and a release Android APK build. A Flutter SDK was not available in the implementation environment; run those CI checks before publishing an APK.

## Native application UI previews

The workspace can show generated screenshots and source files. For Flutter/Dart application work, the agent must keep the native Dart source as the implementation of record and create a self-contained responsive HTML preview companion for visual design review when no emulator/device capture runner is configured. The companion is rendered at desktop, tablet, and mobile viewports and returned as an image to the model. Do not describe this companion screenshot as a native Flutter runtime capture. A native screenshot requires a configured Flutter SDK plus an Android emulator or physical-device capture runner on the worker.
