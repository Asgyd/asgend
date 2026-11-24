# ASGEND Native Android — Stage 5: remaining local CLI/TUI parity

**Date:** 2026-10-25  
**Base:** Stage 4B configuration-editor archive.

## Delivered in source

- Dead-letter inspection through core `dead_letters`, with bounded result count; retry still uses the server-side `retry_task` eligibility rules.
- Task-scoped generated-artifact inventory via `artifact_list`, including relative path, MIME, byte size, SHA-256, skipped-entry/truncated-scan warnings.
- Binary reads through `workspace_read_bytes` in bounded chunks; local export is capped at 64 MiB, each response is checked for path/offset/size/next-offset/decoded-length consistency, artifact size must remain stable, and the final SHA-256 must match the inventory. Data goes to app-private `cache/artifacts` and is exported only after verification via a non-exported AndroidX `FileProvider` exposing only that subdirectory.
- Confirmed LSP diagnostics for normalized relative paths inside the core-configured project root. Client rejects absolute paths, backslashes, control/NUL bytes and `.`/`..` components before network access; the C core applies its own path/root/file-size constraints.
- To-do/Goals refresh via shared core `plan_wait` revision long-poll; a timeout and lifecycle cancellation avoid a fast polling loop.
- Text file editor dirty-state is cleared only after the core confirms the write succeeded.
- Fixed a pre-existing source-level navigation inconsistency: the Runtime screen referenced `RuntimeSection.Configuration`, but that enum member was missing. Added it and extended the static source contract so this mismatch is caught.

## Verification run

- Dependency-free native source-contract checker: **PASS**.
- Android resource XML parse: **6/6 PASS**.
- Kotlin/JVM repository contract harness compiled with local API-compatible Android/JSON/coroutine/JUnit stubs: **14/14 PASS**. Covered bounded dead-letter query, artifact scoping, binary chunk request/response validation, bad relative-path rejection, LSP diagnostics bounds, `plan_wait` revision/deadline and invalid inputs.
- Archive verification: this stage must be checked after packaging and clean extraction; see checksum sidecar.

## Explicitly not verified

The container has no Gradle executable, Gradle wrapper JAR, Android SDK/platform `android-37`, emulator or AndroidX dependency cache. Therefore no Android Gradle compile, Compose compiler, official JUnit task, APK build, emulator launch, or device acceptance is claimed. The API-compatible contract harness validates repository request logic only; it does not prove Compose UI compilation.

## Recommended follow-ups

1. Run `gradle wrapper --gradle-version 9.6.0` in an Android SDK-equipped environment, then `./gradlew testDebugUnitTest assembleDebug assembleRelease`.
2. Add Compose semantics tests for artifact truncated/empty/verified/error states and plan long-poll lifecycle cancellation.
3. Add device tests for large artifact export, process death, scoped-storage Sharesheet behavior, VPN/LAN connection and Android 17 edge-to-edge/IME/back behavior.
4. Preserve the recommendation for a narrower/non-destructive core config update operation; Stage 5 does not resolve that earlier configuration side effect.
