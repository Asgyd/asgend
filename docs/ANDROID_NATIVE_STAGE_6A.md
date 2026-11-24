# ASGEND Native Android — Stage 6A: Control Plane HTTPS and pairing

**Date:** 2026-10-25  
**Base:** Stage 5 local CLI/TUI parity archive.

## Delivered in source

- Independent `ControlPlaneClient` and transport; no raw-core RPC payloads are sent to HTTPS APIs. Network requests run on `Dispatchers.IO`, default platform TLS verifies certificates, redirects are disabled and JSON responses are capped at 2 MiB.
- Endpoint validation accepts HTTPS only, rejects URL user-info/query/fragment, rejects dot-segment traversal, lowercases the host, removes default port and strips a trailing `/v1` base suffix. API path/query fields are encoded; IDs are sent as path segments.
- Request IDs and `User-Agent` are attached; `X-ASGEND-API-Version` is checked when supplied. Errors map to bounded, generic messages and never echo response bodies or credentials.
- Pairing uses unauthenticated `POST /v1/pairing/redeem`. One-time code is JSON-body-only and not saved. Response token must match the OpenAPI schema `^ucp_[A-Za-z0-9_-]{32,256}$`; workspace ID, user ID and a recognized role are required.
- Local core token and remote user token are kept in separate encrypted preference slots and separate Android Keystore aliases. Pairing preserves local credentials. Sign-out attempts server revoke but always clears local remote metadata/token if the server is offline. A 401 clears expired remote credentials.
- Settings exposes remote pairing/session identity, memberships/roles and connected workspace node inventory without increasing the five-item primary navigation.

## Contract/source checks

- Control Plane contract harness: **7/7 PASS** using local API-compatible Kotlin/JVM stubs. Covers endpoint normalization/refusals, pairing method/payload, bearer header, no-token no-network guard, API version and redirect policy, node segment encoding, and malformed token response.
- Native source-contract verifier: PASS; checks HTTPS-only/redirect policy, Bearer placement, response bounds/version, separate Keystore slot and Settings reachability.
- Stage archive will be checked after clean extraction, with source verifier and XML parse rerun.

## Explicitly not verified

No Gradle executable/wrapper JAR, Android SDK/platform `android-37`, emulator or AndroidX cache is available here. Therefore native Android Gradle/Compose/JUnit compilation, APK build and device acceptance are not claimed. The contract harness exercises HTTP request policy through a fake transport; it does not prove a real TLS handshake or UI compilation.

## Remains for Stage 6B/6C

Remote workspace switching, task creation/list pagination, event long-poll/cursor, role-gated answer/cancel/retry, and artifact download tickets/streaming/hash/export have not been completed in Stage 6A. They remain explicit TODO items.
