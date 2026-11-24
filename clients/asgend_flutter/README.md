# ASGEND Flutter client (Linux + Android)

This Flutter source package talks directly to the ASGEND core's line-delimited JSON TCP API. It does not host or embed the C orchestration runtime. The same core can serve the web UI, this application, and the C CLI concurrently.

## Features

- Task list, create task and open chat immediately
- Live task status/event timeline and final result
- Agent/orchestrator questions with choice list and free-text response
- Cancel task and continue a session
- Nodes and model registry, enabled/running state, enable/disable/start/stop
- Add/edit flexible agent profiles
- Configurable core host/port/token
- HTTPS Control Plane mode with one-time pairing code or pasted `asgend-pair://` QR payload
- Workspace switching, node/task list, role-gated remote commands, live event cursor and logout/revoke
- Artifact download ticket flow with bounded streaming, SHA-256 verification, and explicit save/share export
- Local Cowork voice input through the separate LFM3 voice gateway (mono 16 kHz WAV), revisioned report and explicit confirmation before task enqueue
- Optional playback of gateway-generated speech replies
- Native PNG capture of the actual Flutter task-workspace preview pane with pixel/byte bounds and OS share sheet

## Build and test

Requires Flutter 3.29.3 / Dart 3.5 or compatible. Dependencies for the API, secure storage, artifact export, microphone recording (`record`) and audio reply playback (`audioplayers`) are declared in `pubspec.yaml`. `flutter_file_dialog` is pinned to 3.0.2 to stay compatible with the committed Gradle 8.10.2 / AGP 8.7.3 Android toolchain; update both together after Android CI verification.

```bash
cd clients/asgend_flutter
flutter doctor -v
flutter pub get
flutter analyze --no-pub
flutter test --no-pub

# The checked-in Android config pins AGP/Kotlin/Gradle; wrapper binaries are
# generated from the selected Flutter SDK template and are not checked in.
./scripts/bootstrap_android_wrapper.sh
flutter build apk --release --no-pub
```

The release APK built by CI is unsigned by design. Production signing must be configured locally or in a protected release workflow; keystores and signing properties must never be committed. The included `pubspec.lock` predates the new `record` and `audioplayers` dependencies. Run `flutter pub get` and commit the refreshed lockfile in the pinned Flutter CI/developer environment before treating package resolution as reproducible. `android/app/src/main/AndroidManifest.xml` declares INTERNET, disables cleartext HTTP and app backups, and the extraction rules exclude credential/state files. Control Plane endpoints must use HTTPS. The local core protocol remains raw TCP and must only be used on loopback or through a protected VPN/SSH tunnel.

The Control Plane screen is opened from the local-core app bar using the cloud icon. Pairing is currently manual or via a copied pairing-link/QR payload; an in-app camera QR scanner is not included. Remote mutations are never queued silently while offline; the UI reports the failed action and the session reuses idempotency keys only for an explicit retry.

Widget tests use an injected `AppEnvironment` (`SettingsRepository`, HTTP client factory and `ArtifactExporter`), so pairing/offline/role/logout flows can be tested with memory stores and `MockClient` without calling platform channels or a live server. Actual `flutter analyze` / `flutter test` / APK verification still must run in the Flutter CI workflow.

The Linux platform runner is not included in this package yet. For desktop development, generate only its platform files into a temporary project and copy `linux/` into this package without replacing `lib/`, then install Flutter Linux dependencies (including `libsecret-1-dev` for secure storage and `parecord`, `pactl`, `ffmpeg` for the `record` Linux backend) before `flutter build linux --release`. Windows/macOS platform scaffolds and their microphone entitlements/usage descriptions must likewise be generated and verified before claiming native support there.

## Networking and security

The core TCP protocol is JSON-line over raw TCP. Set the core IP, TCP port and `server.auth_token` in connection settings. Default `127.0.0.1` refers to the device running the client; on Android use the core host's reachable VPN/LAN address. The protocol currently has no built-in TLS, so do not expose the core port to the public Internet; use WireGuard, an SSH tunnel or another protected channel, and firewall the core to trusted peers. Host and port are stored in preferences; the core token is stored using the platform secure-storage plugin. On first launch after upgrading from an older build, the token is moved from preferences to secure storage; the old value is deleted only after a successful secure write.

Verified on 2026-10-22 with the pinned Flutter 3.29.3 (Dart 3.7.2), JDK 17 and Android SDK 35 / NDK 27: `flutter analyze` reports no issues, `flutter test` passes 20/20, `./scripts/bootstrap_android_wrapper.sh && flutter build apk --release` produces an unsigned `app-release.apk` (versionName 1.0.0, versionCode 2, minSdk 24), and the APK, re-signed with a throwaway key, launches on an Android 14 emulator. Sign release builds with your own keystore; no signing material is committed.

## Native Flutter Cowork voice and screenshots

The local-core Flutter client exposes a `Cowork` tab. Configure a separate voice-gateway origin and `ASGEND_UI_TOKEN` in Connection settings; the token is stored in the secure secret store and is not part of settings exports. The core TCP token and gateway UI token are independent credentials. Android requires an HTTPS gateway origin; desktop permits plain HTTP only on loopback (`localhost`, `127.0.0.1`, or `::1`). The gateway normally binds to `127.0.0.1:8788`, so an Android phone reaches it through a TLS reverse proxy/VPN endpoint, not the phone's loopback address.

Flutter records voice as mono 16 kHz WAV for up to three minutes per clip. A failed upload can be retried or explicitly discarded; pending clips live only in the app-private temporary directory and are deleted when the Cowork route is closed. The optional `audioplayers` output path plays a `speech_wav_base64` reply returned by the gateway. Android declares `RECORD_AUDIO`, and the `record` plugin requests microphone permission at runtime.

The native screenshot action is available in the Task Workspace preview toolbar. It captures the actual Flutter-rendered file-preview pane (agent-produced document/page/code preview), checks the pixel dimensions and PNG size before sharing, and opens the platform share sheet. Cowork also has a separate action to send its report-panel image to the voice interpreter. These actions use Flutter `RenderRepaintBoundary.toImage`; they capture only an ASGEND widget subtree, never the whole Android screen, notification shade, or other applications. For the agent's own artifact verification, keep using MCP `render_and_inspect`: its screenshot is returned to the main vision model as an image tool result.

Android APK build/analyze/widget tests run in `.github/workflows/flutter-android.yml`. The current implementation environment does not include the Flutter SDK, so local Dart analysis, Flutter widget tests, and APK build are not reported as executed; the workflow is configured to run them on Flutter 3.29.3.
