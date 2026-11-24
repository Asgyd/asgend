# ASGEND implementation status — iteration 8

Date: 2026-10-21  
Work package: WP4 / Q1 — native Qt 6 Linux client  
Status: source implementation advanced; native build/runtime acceptance remains blocked by the missing Qt SDK in this environment.

## Implemented

- Repaired `clients/asgend_qt/CMakeLists.txt`: removed stale source references to UI classes and `TaskEventStream` that were not in the delivered source tree; targets now enumerate the shipped adapter/core files and the new main window.
- Added `src/main.cpp` and `src/ui/mainwindow.{h,cpp}`. The Qt Widgets main window covers profile selection/editing, pairing a remote HTTPS Control Plane, local Unix socket and loopback-only local TCP profiles, task list with state/text filtering, task submission, periodic status/event refresh, answer/free-text or single-choice response, cancel, retry, dead letters, node/agent/model overview, capability-gated model actions and node revoke, and artifact download through the adapter's size/checksum verification.
- Reused the existing `CoreAdapter`, `ControlPlaneAdapter`, `ConnectionManager`, `CredentialStore`, `IdempotencyKey` and model mapping rather than duplicating protocol logic in the UI.
- Added `clients/asgend_qt/README.md`, Qt Test utility tests, Russian state-name translation source, desktop file, SVG icon, AppStream metainfo, CPack configuration for TGZ/DEB/RPM, and `.github/workflows/qt-client.yml` for Ubuntu build/test/TGZ packaging.
- Updated `TODO.md`, `docs/CLIENTS.md`, `docs/ROADMAP.md`, `docs/IMPLEMENTATION_PLAN.md` and the top-level README to record current scope and verification limits.

## Verification completed

- Core CMake build already available in the workspace; quick CTest matrix excluding the long `integration` test: **16/16 passed**.
- Contract tests: **10/10 passed** (`tests/contracts/test_contracts.py`).
- Control Plane tests: **37/37 passed** (`server/tests`).
- Node-agent tests: **30 passed, 1 skipped** (`node_agent/tests`).
- Static project checks: all **28** `src/...` paths referenced by the standalone Qt CMake manifest exist; MainWindow declarations have definitions; XML metainfo and TS files parse; CI workflow YAML parses; source files comply with `QT_NO_KEYWORDS`.

## Not verified / limitations

- `cmake -S clients/asgend_qt -B build-qt ...` cannot proceed because Qt6 development files are absent (`Qt6Config.cmake` not found). An attempt to refresh the package index timed out, so I did not install dependencies in this run.
- Therefore no Qt C++ compile, Qt Test execution, GUI runtime, pairing interaction, TLS handshake, or native package installation is claimed as successful. The new Ubuntu CI workflow is intended to perform those checks in a Qt-enabled environment.
- Qt UI settings import/export actions, system tray/notifications, AppImage, Arch PKGBUILD, Flatpak, and cross-distro package verification remain open.
- The long core `integration` CTest remains outside the quick matrix and is not counted as passed. Docker/PostgreSQL live deployment and restore testing also remain open.

## Next recommended work

1. Run the Qt workflow or build on a Qt 6.4+ system and fix compiler/test findings, then add mock-server widget/integration coverage.
2. Add settings import/export UI and complete tray/accessibility/localization work; finish AppImage/PKGBUILD/Flatpak or explicitly narrow the supported package matrix.
3. Execute full CP ↔ node-agent ↔ core end-to-end acceptance and Docker/PostgreSQL restore drill before declaring ASGEND 1.0 release-ready.
