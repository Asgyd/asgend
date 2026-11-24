# ASGEND Native Android — декомпозиция итогового ревью

**Дата:** 2026-10-25 · **Версия:** 1.0 · **База:** три последовательных локальных аудиторских прохода.

## A. Структура/паритет

### A1. Native build foundation
- Source modules: MainActivity, Compose shell/theme, CoreRpcClient, ControlPlaneClient, settings/Keystore, CI.
- Приёмка: actual Gradle/JDK/SDK API37 compile + unit test; unsigned Debug/Release APK; clean install; no regression Flutter/C clients.
- Статус: реализовано в исходниках, build/device evidence отсутствует.

### A2. Local CLI/TUI parity
- Tasks/chat/events/clarification/follow-up/cancel/retry; Plan; Files; local artifacts; LSP; Runtime/models/resources/MCP/agents/config.
- Приёмка: source contract + JUnit repository contracts + Compose UI/device flows; core-side authorization/path confinement остаётся authoritative.
- Статус: основные операции присутствуют в исходниках; local UI/device acceptance не пройдена; process-death restoration открыта.

### A3. Control Plane parity
- Pairing/identity/workspace nodes/tasks/events/role-gated commands есть в исходниках.
- **Stage 8 source implementation:** Remote artifact list/ticket/stream/checksum/export now follows OpenAPI; same-origin HTTPS, no bearer on ticket-byte GET, redirects disabled, bounded size/time, SHA-256/length checks, one expired-ticket retry, partial cleanup and verified-only private-cache Sharesheet. Contract harness 27/27 with local API-compatible stubs.
- Приёмка остаётся открытой: actual Gradle/JUnit run, FileProvider/ShareSheet instrumentation, cancellation and expiry on API 37 device/emulator, plus server-side ticket binding/expiry E2E.

## B. Надёжность/безопасность

### B1. Durable request ledger (P1)
- Persistence для pending create signature hash/key и command idempotency keys; no raw task text in durable ledger; bind records to workspace/task/operation/payload hash; TTL; clear only after confirmed outcome.
- Persist selected local task/session ID and remote event cursor/history bounded by size/time; no raw event payload or secrets.
- Тесты process kill/relaunch after mutation accepted but response lost; identical payload reuse key; changed payload gets new key.

### B2. Local secret/transport boundary (P1/P2)
- Raw TCP stays acknowledged as unencrypted and must be via VPN/SSH; no public exposure.
- **Source update Stage 7:** 401/logout detaches remote credentials in memory before persisted deletion; failed deletion is surfaced with retry UI. Keystore fault-injection/device verification is still required.
- No token in logs, URLs, saved UI state or artifact download URLs.

### B3. Safe configuration update (P1/P2)
- Replace destructive full-bundle legacy `config_save` with server-side schema-validated non-destructive update/merge; atomic backup/rollback; preserve legacy fixed-role agent files.
- Until available, keep warning/acknowledgment and test the destructive behavior explicitly.

### B4. Remote artifact delivery (P1 feature gap)
- Implement list/status/ticket; same-origin HTTPS allow-list; no Authorization header to signed-download origin; redirect policy explicitly validated; bounded streaming; length/hash; expiry; partial file cleanup; 64 MiB (or schema-approved) cap; FileProvider only for verified private cache.

### B5. API contracts / CI triggers (P2)
- Trigger native workflow on contracts/v1/**, source server RPC definition and protocol docs.
- Make schema-based contract tests part of CI; add lint/static analysis and UI/device jobs.

## C. UX / Material 3 Expressive / accessibility

### C1. Adaptive workspace
- Compact: up to 5 primary bottom destinations, detail route/back behavior; avoid putting remote artifact/settings overflow in inaccessible submenus.
- Medium/expanded: list-detail and optional multi-pane workspace; validate 640dp threshold and window resizing, not only orientation.
- Restore selected route/task across configuration/process death.

### C2. Expressive design quality
- Theme roles, shapes, typography, tonal elevation, component hierarchy, motion; no color-only status coding; reduced motion respect.
- Use current stable M3 release and keep all Compose versions pinned/consistent.
- Verify Light/Dark, Dynamic/Fallback colors and contrast.

### C3. Accessibility
- TalkBack order/labels, touch targets, large font scaling, keyboard/switch navigation, IME/rotation, progress/status announcement, semantic tests.

## D. Release / evidence

1. `verify_native_source.py` — static contract only.
2. Kotlin/JVM harness with stubs — partial compile/contract evidence only.
3. `testDebugUnitTest` — actual Gradle/JUnit evidence.
4. `assembleDebug assembleRelease` — APK compile evidence.
5. API37 emulator/device acceptance — OS/behavior evidence.
6. Signed release — outside this audit; signing material must remain external.

**Current sign-off:** HOLD until D3–D5 pass and P1 items have owners/acceptance. No independent external subagent opinion is represented in this report; the three passes are local review passes.


## B5. Дополнительные подтверждённые edge cases (Iteration 2)

- **ANDR-007:** workspace selection must be atomic/accurately recoverable if nodes/tasks refresh fails after selection changes.
- **ANDR-008:** 401 credential cleanup failure must not be silently represented as fully cleared; preserve fail-closed UI and expose safe retry status.
- **ANDR-009:** validate `api_version` in JSON envelopes where OpenAPI schema requires it, not only optional response header.
- **ANDR-010:** durable idempotency ledger must survive process death to prevent duplicate durable commands after ambiguous response.
- **ANDR-011:** restore selected task/workspace and event cursor/history boundedly after process death; revalidate authoritative server state.
- **ANDR-012:** add targeted unit/ViewModel/instrumentation tests for all above; source grep is not test execution.


## C4. Iteration 3 UX/platform findings

- **ANDR-013:** wide-window mode switches to NavigationRail but still renders one destination at a time; implement list-detail/multi-pane composition for larger widths.
- **ANDR-014:** M3 1.4.0/theme is present, but expressive motion, contrast, semantics and accessibility need actual Compose/UI-device acceptance.
- **ANDR-015:** API37 target is configured but Android 17 runtime behavior remains NOT RUN without SDK/emulator.
- **ANDR-016:** move hard-coded user-facing text into string resources and establish localization/large-font acceptance.
- **ANDR-017:** keep `verify_native_source.py` as a guardrail, never substitute it for actual Gradle/JUnit/Compose/APK/device verification.
- **ANDR-018:** split oversized ViewModel/Client/Screen files incrementally after contract tests exist.


## Stage 7 status addendum (2026-10-25)

- NAT-17 atomic workspace switch: implemented in source; nodes/tasks fetch staged before local selection commit.
- NAT-18 credentials: fail-closed in-memory invalidation + explicit cleanup retry UI implemented; Keystore failure test not run.
- NAT-19 API version: JSON body `api_version == 1` required on success, alongside optional header check; source test added.
- NAT-02/NAT-20 idempotency: persistent digest/key ledger implemented; 7-day ambiguous record expires fail-closed and requires explicit user review to discard.
- NAT-03: remote selected task/filter/event cursor + capped safe history persisted; local core state and process-death device acceptance still open.
- Verification evidence: source verifier PASS; XML 6/6; standalone ledger compile PASS; custom runtime harness 4/4. No Android Gradle/JUnit/APK/device test was run.


## B6. Remote artifact delivery — implementation status Stage 8
- Inventory schema and selected-task binding validated before rendering. Required `expires_at` accepts JSON null or a matching timestamp; missing/wrong type is rejected.
- Ticket creation uses authenticated Control Plane JSON call; byte stream uses separate HTTPS transport without bearer token and rejects redirects/external URL.
- Stream requires API version, exact content length, SHA-256 header and computed digest; max 256 MiB, bounded 64 KiB chunks/deadline, one ticket refresh after HTTP 410, partial deletion on failures/cancellation.
- Verified-only file gets shown in Sharesheet from non-exported FileProvider; share is not possible from a `.part` file.
- Static source and local Kotlin/JVM contract harness pass; Android build/device acceptance remains blocked/not run.
