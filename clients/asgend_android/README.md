# ASGEND Native Android (Kotlin + Jetpack Compose)

This is a **separate native Android client**. It does not replace `clients/asgend_flutter` and does not embed the C orchestrator in the APK. The same ASGEND core is contacted through its existing JSON-lines TCP API.

## Build configuration

- Android 17 / API 37: `compileSdk = 37`, `targetSdk = 37`.
- `minSdk = 24`, keeping the existing Android client's declared baseline until device/CI validation says otherwise.
- Jetpack Compose, Material 3 stable `1.4.0` and Compose BOM `2026.09.00` pinned, based on official AndroidX release notes at 2026-10-25.
- AGP `9.4.1`, Gradle `9.6.0`, Compose compiler plugin `2.4.10`, JDK 17.
- Dynamic Material colors on Android 12+; a coherent light/dark fallback palette is used elsewhere.

This source delivery does not include `gradle-wrapper.jar` because neither Gradle nor its wrapper artifacts are installed in the authoring environment. The checked-in `gradle/wrapper/gradle-wrapper.properties` records the expected Gradle version; generate the standard wrapper in a connected Android development environment (`gradle wrapper --gradle-version 9.6.0`) before invoking `./gradlew`. The CI workflow uses `gradle/actions/setup-gradle` and therefore does not depend on an absent wrapper JAR.

## Current implementation stage

Stage 2 adds the Android application module, API 37 project settings, M3 theme, compact bottom navigation / expanded navigation rail, secure core-token storage through Android Keystore AES-GCM and a bounded TCP JSON-line RPC client. Stage 3 adds real core-backed task listing/filtering/creation, task chat/event polling, clarification, follow-up/cancel/retry, global/task-scoped To-do/Goals and safe task workspace file browsing/preview/edit. Runtime/model/config administration is in Stage 4; remaining local dead-letter/artifact/LSP operations are in Stage 5; HTTPS Control Plane pairing/identity is Stage 6A; remote workspace/task operations and cursor event timeline are implemented in Stage 6B; remote artifact download is Stage 6C. See `../../docs/ANDROID_NATIVE_TODO.md` for exact statuses and external build limitations.

## Connection security

The local-core API is raw TCP and is not encrypted. Use a trusted VPN or SSH tunnel; do not expose the core port to the public Internet. The UI requires explicit acknowledgement. `localhost` on a real Android phone points to the phone, not the computer running the core. Prefer the HTTPS Control Plane for remote access. Tokens are encrypted with an AES key held by Android Keystore and are excluded from backup/data extraction. Cleartext HTTP is disabled in the app manifest.

## Build / tests

In an Android environment with API 37 installed:

```sh
cd clients/asgend_android
gradle wrapper --gradle-version 9.6.0
./gradlew testDebugUnitTest
./gradlew assembleDebug assembleRelease
```

APK build and emulator launch are **not verified in the current authoring environment**, which has no Android SDK, SDK platform JAR, Gradle installation/cache, or emulator. Unit tests in source cover connection validation, error redaction, question parsing and repository request contracts, but must be executed on Android CI. The RPC layer has an injectable `CoreRpcTransport` seam so these contracts can be tested without opening sockets.

## Toolchain limitation for this delivery

Do not treat the presence of Gradle files as proof that the project has compiled. This authoring container has Java 21 and Kotlin CLI 1.9, but lacks Gradle, the Android SDK / `android.jar`, and network access to download dependencies. The Kotlin syntax probe found no `expecting` diagnostics, and the pure `CoreSettings.kt` file can be compiled standalone; all Compose and Android API compilation must be completed by the checked-in CI workflow or an Android Studio host.


## Runtime screen

The Runtime destination reads core health, model registry state, agents, resource filenames and MCP connection status. Model enable/disable/start/stop controls call only allow-listed core operations and require confirmation before changing shared state. This screen intentionally does not display API keys, auth tokens, raw config JSON, model endpoints, raw model errors, log paths or absolute resource directories. Run `python3 clients/asgend_android/tools/verify_native_source.py` from the repository root for dependency-free source/security contract checks. Android Gradle/JUnit/device tests still need Android SDK 37 in CI.


## Local CLI/TUI parity — Stage 5

The Workspace destination exposes the core's terminal failed-task list (`dead_letters`) and reuses the existing server-validated retry action. Files destination has a task-scoped artifact inventory (`artifact_list`) showing relative paths, MIME, size and SHA-256, and tells the user when the core scan is incomplete. Artifact export reads bounded binary chunks through the workspace-confined API, checks the recorded size and SHA-256 incrementally, writes only to private cache, and enables Android Sharesheet export only after the verification succeeds. Native export is capped at 64 MiB per file to bound storage use. The Files destination also has a separate confirmed LSP diagnostic action: user-entered normalized relative paths are passed to the configured core LSP process, not run on Android; the C core independently confines resolution to its configured project root and rejects non-regular/oversized files. Generated file writes only clear edit dirty-state after the RPC succeeds. To-do/Goals uses core `plan_wait` revision long-poll with lifecycle cancellation and bounded retry delay so changes from CLI/TUI are visible without tight polling. A local Kotlin/JVM repository contract harness with API-compatible stubs reports 14/14 PASS; this is not an Android Gradle build or Compose/JUnit acceptance.

Run `python3 clients/asgend_android/tools/verify_native_source.py` from the repository root for the static source-policy contract. Contract test source now covers bounded dead-letter requests, task-scoped artifact listing, and rejecting absolute/traversal paths for LSP. These checks are source-only until Android CI executes Gradle/JUnit.


## HTTPS Control Plane — Stage 6A

Settings includes a separate Control Plane pairing section. It requires a normalized HTTPS endpoint, sends the one-time code only to `/v1/pairing/redeem`, stores a schema-validated `ucp_` token in a distinct Android Keystore slot, and shows `/v1/me` membership/role plus workspace nodes. The local core token is unaffected by remote pairing or sign-out. Redirects are refused; bearer credentials are sent only in the Authorization header; JSON response bodies are capped at 2 MiB. Remote task/event and artifact workflows remain Stage 6B/6C.


## HTTPS Control Plane tasks — Stage 6B

Settings → Remote Control Plane now provides workspace switching, remote task filters/pagination, task submission to a selected node, task detail and safe event timeline, answering questions, confirmed cancellation and retry. It refreshes membership before mutation and only offers commands to owner/operator memberships; the Control Plane remains authoritative. It uses the OpenAPI v1 cursor and idempotency fields, never routes remote operations through the local TCP client, and does not enqueue commands while offline. Event summaries are allow-listed, and raw event payloads are not rendered.

A local Kotlin/JVM contract harness with API-compatible stubs reports 14/14 PASS for the HTTPS client. This is not an Android Gradle/JUnit/Compose/APK test. The current environment lacks Android SDK/API 37 and Gradle. Process-death persistence for remote event cursors remains pending; remote artifact ticket/streaming is Stage 6C.
