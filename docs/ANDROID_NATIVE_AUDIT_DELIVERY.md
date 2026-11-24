# ASGEND Native Android audit — delivery manifest

Audit date: 2026-10-25. This package contains the source tree, the three audit pass reports, final audit/decomposition/TODO and implementation-stage reports. Stage 7 reliability and Stage 8 remote artifact source implementations are included; Android build/device evidence remains blocked.

## Core documents

- `ANDROID_NATIVE_AUDIT_FINAL.md` — consolidated findings, evidence, severity, release decision.
- `ANDROID_NATIVE_REVIEW_DECOMPOSITION.md` — review decomposed by architecture, security, state, UI and release evidence.
- `ANDROID_NATIVE_AUDIT_TODO.md` — prioritised remediation list with acceptance criteria.
- `ANDROID_NATIVE_AUDIT_ITERATION_1.md` — structure and functional parity.
- `ANDROID_NATIVE_AUDIT_ITERATION_2.md` — security, protocol, idempotency and lifecycle.
- `ANDROID_NATIVE_AUDIT_ITERATION_3.md` — Material 3 Expressive, responsive UI, Android 17 and release evidence.
- `ANDROID_NATIVE_STAGE_LOG.md` — chronological implementation/audit notes.
- `ANDROID_NATIVE_IMPLEMENTATION_STAGE_7.md` — atomic workspace switching, credential cleanup, body API versioning, durable remote idempotency and remote recovery state.
- `ANDROID_NATIVE_IMPLEMENTATION_STAGE_8.md` — remote Control Plane artifact inventory/ticket/verified download/export and 27/27 API-compatible contract harness result.

## Verification snapshot

- `python3 clients/asgend_android/tools/verify_native_source.py`: PASS.
- Android XML parse: 6/6 PASS.
- Standalone `CoreSettings.kt` compile: PASS.
- Gradle / Android SDK API 37 / emulator: unavailable in the authoring environment.
- Stage 8 Kotlin/JVM contract harness: 27/27 PASS using local API-compatible stubs; not Android JUnit.
- Native Compose/JUnit/APK/device acceptance: NOT RUN, not PASS.

## Scope boundary

The package preserves existing project files and adds audit documentation. Stage 7 remediates NAT-02/NAT-20, remote subset of NAT-03, NAT-17, NAT-18 and NAT-19 in source; NAT-01 is source-implemented in Stage 8 with Android device acceptance open; NAT-04 safe config endpoint remains OPEN. The pre-existing APK under `dist/asgend-1.0.0` is the legacy Flutter Android distribution artifact described by release notes; it is not a Kotlin-native build result.

Three local audit passes are documented. The runtime did not expose an external subagent launcher, so no independent external agent score is claimed.
