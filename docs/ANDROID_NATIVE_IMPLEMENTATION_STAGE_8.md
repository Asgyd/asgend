# Native Android implementation — Stage 8: remote Control Plane artifact delivery

**Date:** 2026-10-25
**Audit basis:** `docs/ANDROID_NATIVE_AUDIT_FINAL.md`, `docs/ANDROID_NATIVE_REVIEW_DECOMPOSITION.md`, `docs/ANDROID_NATIVE_AUDIT_TODO.md`, and `contracts/v1/openapi.yaml`.

## Implementation

Implemented NAT-01 in the native source tree:

- `GET /v1/tasks/{task}/artifacts` with bounded inventory count, strict task binding, required schema fields, ID/name/MIME/state/size/SHA-256/timestamp validation, and nullable-but-required `expires_at` handling.
- `POST /v1/artifacts/{art}/download-ticket` uses the authenticated JSON client. Ticket response must have a valid ticket identifier, exact `/v1/downloads/{ticket}` path, timestamp, and matching optional artifact binding.
- Ticket byte transfer is a separate HTTPS-only transport which intentionally has no bearer-token parameter and never sets `Authorization`. Redirects are disabled. A ticket is reconstructed against the configured Control Plane origin and exact origin-relative path; external origin, query, fragment, malformed path and ticket mismatch fail before network use.
- Download is bounded to 256 MiB, 64 KiB chunks and a 30-minute absolute deadline. It requires HTTP 200, `X-ASGEND-API-Version: 1`, matching `Content-Length`, a valid `X-Checksum-SHA256` consistent with inventory and a matching computed SHA-256. A 410 ticket expiration is retried once with a fresh ticket.
- An unverified download remains a `.part` file in the app-private cache, is never published to UI/share, is deleted on hash/length/read/cancellation failure, and is atomically renamed only after successful verification and `fsync`.
- UI lists artifacts for the selected task, has loading/empty/error/refresh/progress/cancel states, refreshes inventory on `artifact_available`, and enables Android Sharesheet only for a verified private-cache file through non-exported `FileProvider`. Selected workspace/task changes remove the verified export; startup cleans stale files in this dedicated cache folder.
- Sensitive `toString()` output redacts ticket, filename and digest data.

## Contract and test changes

Added `RemoteArtifactTransfer.kt`, `RemoteArtifactTransferContractTest.kt` and strengthened `verify_native_source.py`. Tests exercise valid inventory/task scoping, required/null expiry, missing/bad expiry, same-origin ticket path, bearer-token boundary, hash/length mismatch, zero-byte artifacts, expired ticket refresh once, redirects, oversize metadata/stream overrun, invalid filename before ticket request, non-available states and partial-file cleanup following stream I/O failure.

## Verification actually run

- `python3 clients/asgend_android/tools/verify_native_source.py`: **PASS**.
- Android XML parsing: **6/6 PASS**.
- Kotlin/JVM contract harness compiling the real `ControlPlaneClient.kt`, `RemoteArtifactTransfer.kt` and relevant test sources against local API-compatible JSON/coroutine/JUnit stubs: **27/27 PASS**.
- The artifact filename guard was moved ahead of ticket creation after the first negative-test review found a direct-call bypass possibility.
- Three local review passes were performed: OpenAPI/schema correctness; credential/URL/hash/size boundary; lifecycle/event-refresh/partial-cleanup. Corrections included strict required nullable expiry handling, refresh on `artifact_available`, no redundant response close, stream failure cleanup, and redaction of filenames from diagnostic `toString()` methods. These passes are not an independent external subagent review.
- Final check before packaging: source verifier PASS; Android XML 6/6 PASS; Kotlin/JVM contract harness 27/27 PASS; archive/diff clean-extract and application verification completed before release of the stage package.

## Not run / remaining acceptance

Gradle, wrapper JAR and Android SDK/platform 37 are unavailable in the current authoring environment. Therefore Android Gradle/JUnit, Compose compiler, APK build, FileProvider/ShareSheet instrumented flow, Android 17 emulator/device and accessibility acceptance are **NOT RUN**, not PASS. NAT-01 is source-implemented; NAT-05/NAT-07/NAT-08/NAT-09/NAT-10 and actual device acceptance remain blocked/open. The Control Plane server must continue to enforce ticket expiry, user/artifact binding and authorization; client checks are defense in depth only.
