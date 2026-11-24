# Stage 5 — Native Flutter voice and workspace capture

## Delivered

- Flutter local-core Cowork can record mono 16 kHz WAV with Android runtime microphone permission, a three-minute clip limit, and a 20 MiB upload limit.
- Voice clips are submitted only to the separate voice gateway. The app displays the gateway's revisioned report and only calls the confirmation endpoint after the user confirms the current revision. There is no direct Flutter call to Core's `submit` operation.
- Failed uploads remain available for Retry or Discard. Recorder/player resources and app-private temporary audio are cleaned up when the Cowork route is disposed.
- Optional `speech_wav_base64` replies are stored with a size bound and played with `audioplayers`; TTS failures do not invalidate a successfully received text report.
- The Task Workspace camera action captures the actual rendered preview subtree (Office/PDF/site/design/code snapshot) through `RenderRepaintBoundary.toImage`, checks pixel/dimension/encoded-size bounds, and opens the platform share sheet. Temporary PNG files are removed after the native share flow returns.
- Cowork has a separate bounded report-panel snapshot attachment sent to the voice gateway.
- Gateway URL is configured independently of the Core endpoint. The UI token is kept in platform secure storage and excluded from settings export. Android requires HTTPS; desktop may use HTTP only for loopback.

## Validation evidence

- `python3 tests/contracts/test_flutter_voice_contract.py -v` — source contract tests.
- `python3 tests/contracts/test_flutter_workspace_layout.py -v` — responsive local/remote workspace layout contracts.
- `python3 -m unittest discover -s tests/voice_gateway -v` — voice gateway API and llama-server mock integration.
- `python3 -m unittest discover -s tests/artifact_workspace -v` — MCP workspace / Office artifact creation and preview.
- `python3 -m unittest discover -s tests/contracts -v` — aggregate API and client contracts.
- Release CMake build and the three Core RPC CTest targets.
- YAML parsing for `pubspec.yaml` and Flutter workflow; Android manifest XML parsing.

The Flutter SDK is not installed in the implementation environment. `flutter analyze`, Dart widget tests, Android APK build, and actual microphone/share-sheet device smoke tests are not claimed as locally executed. `.github/workflows/flutter-android.yml` runs Flutter 3.29.3 dependency resolution, analyze, tests, and unsigned APK build. The checked-in `pubspec.lock` predates the new `record` and `audioplayers` dependencies and must be refreshed by `flutter pub get` in the pinned Flutter environment.

Cowork voice integration is currently exposed in the local-core Flutter app. The remote Control Plane mode continues to use its existing remote workspace APIs and does not yet expose this gateway session UI. The repository does not include non-Android desktop platform scaffolds, so native desktop microphone capture must be built and verified after generating the platform runner and installing the documented platform dependencies.
