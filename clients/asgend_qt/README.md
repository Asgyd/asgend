# ASGEND Qt 6 client

A native C++17 / Qt Widgets desktop client for the ASGEND core and remote Control Plane. The UI is deliberately thin: protocol rules, TLS policy, credentials, retries, event mapping, and streamed artifact checksum verification live in `src/core/` adapters.

## Build on Linux

Requires Qt 6.4+ (`Core`, `Network`, `Widgets`, `Svg`, `LinguistTools`, `Test`), CMake 3.16+, a C++17 compiler, and Ninja or Make.

```bash
cmake -S clients/asgend_qt -B build-qt -DCMAKE_BUILD_TYPE=Release \
  -DASGEND_QT_BUILD_TESTS=ON -DASGEND_QT_USE_QTKEYCHAIN=AUTO
cmake --build build-qt --parallel
ctest --test-dir build-qt --output-on-failure
cpack --config build-qt/CPackConfig.cmake -G TGZ
```

Set `ASGEND_QT_USE_QTKEYCHAIN=ON` to require QtKeychain; `AUTO` uses it when installed. When it is unavailable, the application uses a per-user credential file with mode `0600` on Unix and displays a warning because that fallback is not encrypted. CI builds and tests the client, packages TGZ and DEB, and installs/launches/removes the DEB in a clean Ubuntu 24.04 container. For 1.0.0 the RPM was also built and install-tested on Fedora 42.

## Connection setup

- **Local Unix socket** is the preferred local profile.
- **Local TCP** is accepted only for loopback addresses; the core TCP protocol is not TLS-protected.
- **Remote Control Plane** requires `https://`, verified server certificates, and pairing code redemption. Certificate validation errors are not ignored.

Do not expose the raw core TCP listener to the Internet. Use the HTTPS Control Plane or a protected VPN/SSH tunnel.

## Current UI scope

The main window includes task list/state/text filtering, task creation, polling of status and events, question answers (including a single-choice question), cancel/retry, dead-letter review, node/agent/model views, model lifecycle actions where the backend allows them, and artifact downloads through the adapters' bounded SHA-256 verification path. The remote Control Plane is capability-gated and can be read-only for viewer-role tokens.

The protocol/core and utility tests cover task-state mapping, idempotency keys, endpoint policy, export-secret filtering, log redaction, and SHA-256 helpers. Qt is not bundled in this source package; build/test status must be confirmed in an environment with the Qt 6 development packages installed. Linux AppImage, PKGBUILD and Flatpak deliverables, broader UI tests and end-to-end tests remain open tasks.
