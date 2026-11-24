# ASGEND Native Android — приоритизированный TODO по результатам аудита

Status: `[ ]` open; `[~]` partial/blocked; `[x]` evidence present in source. «Есть в исходниках» не означает «APK/device test прошёл».

## P1 — блокирует полноценный native parity/release sign-off

- [x] **NAT-01 Remote artifacts (source implemented; device acceptance pending).** Inventory/state/ticket/byte stream follow `contracts/v1/openapi.yaml`; strict same-origin HTTPS ticket URL; no bearer on raw download request; bounded streaming; Content-Length/SHA-256; one refresh after expired ticket; partial cleanup; private cache + verified-only Sharesheet; contract tests.
  - Evidence: Kotlin/JVM contract harness 27/27 with local API-compatible stubs; source verifier PASS.
  - Acceptance still open: run real Gradle/JUnit, exercise FileProvider/ShareSheet and cancellation/expiry on API 37 device/emulator; verify server-side ticket binding/expiry end-to-end.
- [x] **NAT-02 Durable idempotency (source implemented; device acceptance pending).** Persist pending `createTask` request key/signature hash и command keys (answer/cancel/retry) до HTTP вызова; одинаковый payload повторно использует ключ, изменённый payload получает новый ключ; очищать только после подтверждённого исхода; ledger bounded и без raw task text.
  - Приёмка: force-stop/relaunch после server acceptance + потерянного response не создаёт дублирующую задачу/команду.
- [ ] **NAT-03 State restore.** Persist selected task/session ID, selected route, workspace ID, event cursor (local + remote) и bounded event history where privacy-reviewed; не сохранять secrets/raw payloads. Проверить rotation, process recreation/kill and relaunch.
- [x] **NAT-04 Safe config save (source + runtime regression).** Added core `config_update_settings`: only explicit server/runtime/sandbox keys, exact JSON types and source-matched numeric ranges; complete candidate is validated by `config_load`; original config is staged to a mode-0600 rollback file before atomic replacement, then rollback file is removed after directory fsync. Runtime test verifies hidden credentials/paths are preserved, `models.json` and agent profiles are untouched, invalid requests cause no partial writes, existing unresolved rollback files fail closed without being overwritten, and legacy `config_save` preserves fixed-role profiles. Android UI now sends only the allow-listed settings object. Full Android Gradle/JUnit/device acceptance remains pending.
- [~] **NAT-05 Android build evidence (BLOCKED/NOT RUN).** Run Gradle `testDebugUnitTest`, `assembleDebug`, `assembleRelease` on real API 37 SDK. Store CI run URL/log and APK SHA-256; no completion claim from static verifier alone.


- [x] **NAT-17 Atomic workspace switch (source implemented; ViewModel tests pending).** Stage new membership/nodes/task list before commit, or fully clear old workspace state and show accurate partial error; tests for node/list request failing mid-switch.
- [x] **NAT-18 Credential cleanup failure state (source implemented; Keystore fault injection pending).** Do not suppress `clearControlPlaneSettings()` failures on 401; fail closed in memory, report safe cleanup status and expose retry without printing secrets.
- [x] **NAT-19 Validate response body API version (source + contract tests added; JUnit run pending).** Validate `api_version` in every response schema that requires it, in addition to any response header; tests for matching/missing/mismatched header and body versions.
- [x] **NAT-20 Durable idempotency ledger (source + standalone ledger tests added; Android JUnit/process-death acceptance pending).** Save operation key and canonical request digest before mutating HTTP calls; use same key only for same digest after relaunch, bounded TTL and confirmed-outcome cleanup.

## P2 — качество продукта / защитные практики

- [ ] **NAT-06 Backend-change CI triggers.** Add `contracts/v1/**`, `src/server.c`, `docs/PROTOCOL.md`, API contract docs to workflow path filters; ensure schema changes invoke native tests.
- [ ] **NAT-07 Compose UI tests.** Add `src/androidTest`: setup connection/pairing, task create, clarification, cancel/retry, To-do/Goals, viewer vs operator, 401/403/offline/cursor reset, artifact share when NAT-01 lands.
- [ ] **NAT-08 Material 3 Expressive acceptance.** Validate complete design system, not only dependency/theme: typography/shape/color/elevation/motion, reduced motion, no color-only state; compact/medium/expanded/split-screen.
- [ ] **NAT-09 Accessibility.** TalkBack, semantic labels/order, keyboard/switch access, touch targets, large fonts, contrast, IME, rotation; create repeatable manual checklist and UI semantic tests.
- [ ] **NAT-10 Android 17 behavior changes.** Test target API 37 and current behavior-change list on an API37 emulator/device; test previous supported releases too. Record exact device/API and outcomes.
- [ ] **NAT-11 Keystore recovery.** Exercise corrupt ciphertext/key loss; ensure UI clearly says credentials were discarded/re-pair needed; failed local deletion must not be shown as successful logout.
- [ ] **NAT-12 Endpoint/path property tests.** Fuzz Unicode, encoded delimiters, slashes, fragments, query, IDNA, invalid ports, cursor limits and control chars; confirm identifiers remain one path segment and secrets remain header-only.
- [ ] **NAT-13 Runtime tests.** Verify no main-thread I/O, cancellation on navigate/background, no overlapping polls, timeouts/backoff and bounded memory for large artifact and event payloads.


- [ ] **NAT-21 Expanded/multi-pane workspace.** Implement list-detail workspace on expanded windows; current NavigationRail still displays a single destination at a time. Test dynamic resize/split screen.
- [ ] **NAT-22 Design/accessibility acceptance.** Compose UI + manual TalkBack, keyboard/switch, large fonts, contrast, light/dark/dynamic/fallback, reduced motion, IME, rotation and API37 edge-to-edge/back behavior.
- [ ] **NAT-23 String resources/localization.** Move user-visible text from Kotlin literals to Android resources; add pluralization and supported locales.
- [ ] **NAT-24 Strengthen CI evidence.** Keep source verifier as a preflight guardrail, add actual Gradle test/build jobs, Compose/instrumentation and API37 emulator job; preserve logs/run IDs and APK checksums.

## P3 — maintainability

- [ ] **NAT-14 Split large files** (`NativeAppViewModel.kt`, `RuntimeScreen.kt`, `ControlPlaneClient.kt`, `NativeRemoteViewModel.kt`) into feature/repository/state files without changing contract behavior.
- [ ] **NAT-15 Localization.** Move user-visible strings into Android string resources; test at least Russian/English and large font.
- [ ] **NAT-16 Release hygiene.** Add lint, dependency/license report, reproducible unsigned artifact checksum, externally managed signing; keep Flutter target independent.

## Completed in source / evidence level

- [x] Native module separate from Flutter; API 37 Gradle configuration, pinned M3 1.4.0 / Compose BOM.
- [x] Local core workflows, Runtime/config form, local artifact safe export, LSP and plan notification source paths.
- [x] Control Plane HTTPS/pairing and remote workspace/task/event source paths.
- [x] Static native source verifier passes.
- [x] API-compatible Kotlin/JVM harnesses were run in earlier implementation stages (local core repository 14/14, Control Plane client 14/14), but those are not Android Gradle/JUnit results.

## Sign-off rule

Do not mark Android native release ready until NAT-01/02/04 have a closure decision, NAT-05 is PASS, and UI/device/accessibility acceptance has evidence. An unavailable toolchain is recorded as `BLOCKED/NOT RUN`, never `PASS`.


## Stage 7 implementation update (2026-10-25)

- Source implemented: NAT-02/NAT-20 persistent pending mutation ledger, remote subset of NAT-03, NAT-17 atomic workspace switching, NAT-18 fail-closed credential cleanup/retry, NAT-19 response-body API version validation.
- Checks actually run: static source verifier PASS; XML 6/6; standalone Kotlin ledger compile PASS; runtime harness 4/4.
- Not run: Android Gradle/JUnit, Compose compile, APK build, API37 emulator and process-death acceptance. Do not treat this stage as release sign-off.
- Stage report: `ANDROID_NATIVE_IMPLEMENTATION_STAGE_7.md`.


## Stage 8 implementation update (2026-10-25)

- Source implemented: NAT-01 remote Control Plane artifact inventory, short-lived ticket and separate raw HTTPS byte stream; schema validation; same-origin/redirect policy; bounded size/time; header and computed SHA-256 validation; one expired-ticket retry; partial cleanup; verified-only app-private cache and FileProvider Sharesheet. Inventory refresh is triggered by `artifact_available`.
- Checks actually run: source verifier PASS; Android XML 6/6; API-compatible Kotlin/JVM harness 27/27 PASS compiling real changed data/test sources against local stubs. Three local review passes corrected filename preflight, nullable required `expires_at`, event-triggered inventory refresh, partial I/O cleanup and diagnostic redaction.
- Not run: Gradle/JUnit, Compose compilation, APK build, Android API37 emulator/device and ShareSheet acceptance; toolchain is unavailable. NAT-05/NAT-07/NAT-08/NAT-09/NAT-10 remain open or blocked.
- Stage report: `ANDROID_NATIVE_IMPLEMENTATION_STAGE_8.md`.


## Stage 9 implementation update (2026-10-25)

- [x] NAT-04 remediated in source: non-destructive `config_update_settings` TCP RPC, strict allow-list/type/range checks, full-candidate config validation, private staged write and rollback copy during atomic replace, directory fsync, `restart_required` response, and fail-closed handling of a retained recovery copy. The legacy full-bundle `config_save` no longer deletes hidden `coder/planner/reviewer` profiles.
- Android guided Config switched from `config_save` bundle to `config_update_settings`; only allow-listed server/runtime/sandbox fields are sent. Config UI warning now describes partial-update semantics and core restart requirement; explicit acknowledgement remains for sandbox weakening.
- Runtime acceptance: `core_config_safe_update` passed, including invalid types/ranges/unknown keys/no partial change, unchanged model registry, preserved hidden credentials and paths, legacy agent-file preservation and no secret leakage; associated config/string/HTTP/SQLite regressions 5/5 PASS. Full C/C++ CMake Release build PASS. Native source contract verifier PASS.
- Not run: full project CTest/integration for this stage and Android Gradle/JUnit/Compose/APK/API 37 device acceptance. Do not claim release sign-off.
- Detailed report: `ANDROID_NATIVE_IMPLEMENTATION_STAGE_9.md`.
