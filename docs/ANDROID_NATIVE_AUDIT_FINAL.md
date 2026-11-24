# ASGEND Native Android — итоговый аудит по декомпозиции

**Дата аудита:** 2026-10-25  
**Проверяемый модуль:** `clients/asgend_android` (Kotlin + Jetpack Compose)  
**Базовая спецификация:** `ANDROID_NATIVE_AUDIT.md`, `ANDROID_NATIVE_DECOMPOSITION.md` и `ANDROID_NATIVE_TODO.md`.  
**Метод:** три последовательных локальных прохода: структура/паритет; security/protocol/reliability; дизайн/Android 17/build/test evidence.

## 1. Executive summary

Нативный модуль существует отдельно от Flutter и использует существующие C core JSON-lines TCP API и HTTPS Control Plane API. Source audit подтверждает наличие основных local workflows: задачи/чат/события, уточнения, отмена/retry, To-do/Goals, файловый workspace, local artifact inventory/export, LSP diagnostics, runtime/models/resources/MCP/agents и guided configuration. Control Plane покрывает pairing/identity/workspace nodes, remote tasks, event cursor, role-gated mutations.

**Статус: CONTINUE DEVELOPMENT; RELEASE SIGN-OFF: HOLD.** Основные причины:

1. NAT-01 remote Control Plane artifact inventory/ticket/verified stream/export реализован в исходниках на Stage 8; реальный Android/JUnit/ShareSheet device acceptance ещё не выполнен.
2. Stage 7 добавил durable remote idempotency keys и remote selected-task/event-cursor/safe-history persistence. Остаются local-core state restore и process-kill/relaunch acceptance; общий Android runtime evidence отсутствует.
3. Android Gradle/Compose/JUnit/APK/API37 device acceptance в доступной среде не запускались. Наличие статического checker и CI workflow не является доказательством сборки.
4. Expanded width показывает NavigationRail, но не реализует запланированный list-detail/multi-pane workspace; accessibility/M3 Expressive acceptance не выполнена.
5. NAT-04 закрыт на уровне backend source: Android использует `config_update_settings`, который валидирует allow-list и атомарно обновляет только главный config, не затрагивая model registry и agent profiles. Legacy `config_save` также исправлен так, чтобы не удалять скрытые fixed-role profiles. Android/Gradle/device acceptance остаётся открытым.
6. Stage 7 реализовал атомарный workspace switch, fail-closed 401 credential cleanup, body `api_version` validation и durable remote idempotency/state; ViewModel/JUnit/Keystore fault-injection и process-death tests остаются обязательными.

## 2. Проверки и evidence level

| Проверка | Результат | Что реально доказывает |
|---|---|---|
| `python3 clients/asgend_android/tools/verify_native_source.py` | PASS (повторен в каждой итерации, включая Stage 8 artifact contract) | Статические текстовые assertions по source contract; не Kotlin/Compose compile |
| XML resources | 6/6 PASS | XML well-formedness manifest/resources |
| Standalone `CoreSettings.kt` via `kotlinc` | PASS | Только чистая Kotlin-модель без AndroidX/Compose |
| Earlier API-compatible repository harness | 14/14 PASS (reported in existing stage log) | Локальный контрактный harness со stubs, не Android Gradle/JUnit |
| Earlier API-compatible Control Plane harness | 14/14 PASS (reported in existing stage log) | Локальный API client harness со stubs, не Android runtime |
| Gradle / wrapper JAR | NOT AVAILABLE | Сборка в текущей среде заблокирована |
| Android SDK/platform 37 / emulator | NOT AVAILABLE | Android API37/device acceptance не выполнялась |
| `src/androidTest` | ABSENT | Instrumented Compose/device tests отсутствуют |
| Compose/JUnit/APK | NOT RUN | Нельзя заявлять PASS или release-ready |

В корне проекта присутствует старый distribution artifact `dist/asgend-1.0.0/asgend-android-1.0.0-unsigned.apk`. Release notes 1.0.0 описывают тот Android component как Flutter-клиент; этот ранее существовавший APK не является сборкой `clients/asgend_android` и не считается доказательством native build.

Official platform/version check:
- Android 17 is API level 37; review [all-app behavior changes](https://developer.android.com/about/versions/17/behavior-changes-all) and [changes for apps targeting API 37](https://developer.android.com/about/versions/17/behavior-changes-17).
- Compose Material 3 `1.4.0` appears as stable in the [official release notes](https://developer.android.com/jetpack/androidx/releases/compose-material3) at 2026-10-25 review time.
- [AGP compatibility page](https://developer.android.com/build/releases/about-agp) states AGP 9.4 requires Gradle 9.6.0; the repository's AGP 9.4.1 / Gradle 9.6.0 pairing meets that minimum.
- [M3 Compose guidance](https://developer.android.com/develop/ui/compose/designsystems/material3) confirms dynamic color on Android 12+ and recommends fallback palettes on older devices.

## 3. Consolidated findings

| ID | Priority | Finding | Status |
|---|---|---|---|
| ANDR-001 | P1 | Remote artifact transfer was absent at initial audit; Stage 8 adds inventory/ticket/verified download/export source and contract harness | SOURCE IMPLEMENTED; DEVICE ACCEPTANCE OPEN |
| ANDR-002 / 010 | P1 | Mutation idempotency keys only in memory; unknown-outcome retry after process death can use new key | OPEN |
| ANDR-003 / 015 | P1 | No actual Gradle/JUnit/Compose/APK/API37 device evidence | BLOCKED/NOT RUN |
| ANDR-004 / 014 | P2 | M3 Expressive, accessibility and window behavior not accepted through UI/device tests | OPEN |
| ANDR-005 | P2 | Native CI path filters omit backend protocol/schema sources | OPEN |
| ANDR-006 | P1/P2 | Legacy `config_save` could delete hidden legacy fixed-role agent files | FIXED IN SOURCE; runtime regression added |
| ANDR-007 | P2 | Workspace switch can partially commit selection and show inaccurate error if later fetch fails | OPEN |
| ANDR-008 | P2 | 401 cleanup suppresses persistence/deletion errors; state may imply cleanup completed | OPEN |
| ANDR-009 | P2 | JSON body `api_version` not validated where schemas require it | OPEN |
| ANDR-011 | P2 | Task/workspace/event cursor restoration after process death is absent | OPEN |
| ANDR-012 / 017 | P2 | Text-only source guardrails are not semantic tests; no evidence for newly identified error cases | OPEN |
| ANDR-013 | P2 | Expanded UI changes navigation chrome only; planned list-detail/multi-pane workspace absent | OPEN |
| ANDR-016 | P3 | User-visible text is hard-coded; localization/consistency work remains | OPEN |
| ANDR-018 | P3 | Large UI/state/client files increase review/maintenance cost | OPEN |

### ANDR-001 — Remote Control Plane artifacts

No remote artifacts APIs appear in `ControlPlaneClient`, nor artifact list/download UI in `RemoteControlScreen`. `artifact_available` is only an event summary. Implement schema-correct list/state/ticket routes; verify ticket path is relative to the approved HTTPS origin, never send bearer to a signed download endpoint, bound stream length, verify checksum, clean partial files, and expose only verified files to Sharesheet.

### ANDR-002/010/011 — Durability and idempotency

`NativeRemoteViewModel` keeps pending create signatures/keys and command keys in process fields; cursor and selected task are also in ordinary ViewModel state. Add bounded persistence for operation + workspace/task IDs + canonical payload digest + idempotency key, cursor/selected task, and safe event summaries. Do not persist raw payloads/secrets; reuse a key only for the exact same payload; remove ledger records after confirmed result/TTL. Test forced process death after request acceptance but before response delivery.

### ANDR-003/015/017 — Build and release evidence

The native workflow exists and target API 37 is configured. No actual build was run in this environment because Gradle/wrapper JAR and Android SDK/API37 are absent. Add/fix tests in the Android CI environment and include run result, not only source checker output. Current Android CI runs JVM unit tests and assemble tasks, but has no Compose instrumented tests or API37 emulator acceptance. Target API assignment alone is not evidence that Android 17 behavior works.

### ANDR-004/013/014/016 — UI/design

Material 3 dependency and theme exist, with dynamic color/fallback palettes and a custom shape scale. Compact uses bottom navigation and expanded uses navigation rail. However the expanded layout is still a single destination screen; no list-detail/pane composition or semantics/device acceptance is present. Add responsive acceptance for compact/medium/expanded, IME/rotation/window resizing, TalkBack, keyboard/switch access, large fonts, contrast, status announcement and reduced motion. Move visible text to string resources.

### ANDR-005/006/007/008/009 — CI, configuration and API consistency

- Include `contracts/v1/**`, `src/server.c`, `docs/PROTOCOL.md` and API contract docs in native CI path filters.
- Replace legacy destructive config full-bundle save with a schema-validated non-destructive update endpoint/merge and backup/rollback.
- Make workspace switching atomic or fully clear old state before partial loads; error text must describe real persisted selection.
- On `401`, do not silently suppress credential cleanup failure; remain fail-closed, report safe recovery state and allow retry without printing secret.
- Validate JSON-body `api_version` for response schemas that require it, alongside any response header.

## 4. Декомпозиция завершённого ревью

1. **Architecture/parity** — local APIs and main local workflows are present; remote artifact workflow missing; expanded multi-pane missing.
2. **Security/protocol** — HTTPS, no redirects, header-only bearer, response caps, separate Keystore slots and role recheck are positive controls; raw TCP stays explicitly unencrypted; local config update risk persists.
3. **State/reliability** — idempotency ledger, workspace switch transaction, cursor and selected task process restore need work.
4. **UI/accessibility** — theme exists, but M3 Expressive visual acceptance, locale, TalkBack, dynamic/fallback contrast, large text, keyboard and window size tests are missing.
5. **Build/test/release** — source checker passes, but actual Android compiler/JUnit/Compose/APK/device evidence remains missing.
6. **Maintainability** — split large ViewModel/client/screen files after critical behavior is contract-tested.

Подробная рабочая декомпозиция: `ANDROID_NATIVE_REVIEW_DECOMPOSITION.md`. Полный action list: `ANDROID_NATIVE_AUDIT_TODO.md`.

## 5. Три прохода аудита и качество процесса

- Iteration 1: структура и функциональный паритет — `ANDROID_NATIVE_AUDIT_ITERATION_1.md`.
- Iteration 2: security/protocol/lifecycle — `ANDROID_NATIVE_AUDIT_ITERATION_2.md`.
- Iteration 3: Material 3 Expressive, Android 17, accessibility, CI/release — `ANDROID_NATIVE_AUDIT_ITERATION_3.md`.

Каждый проход повторно проверял исходники и дополнял выводы предыдущего; TODO/decomposition корректировались по найденным противоречиям. Доступная среда не предоставляет отдельного sabagent launcher, поэтому отчёт не заявляет independent external score от отдельного агента. Три прохода — локальные аудиторские циклы, не внешний независимый review.

## 6. Release decision

**HOLD / NOT RELEASE-READY.** Не из-за доказанной ошибки Kotlin компиляции, а потому, что обязательное build/device evidence отсутствует и P1 gaps остаются. Минимальные условия перехода к release candidate:

1. Закрыть или formally defer remote artifact delivery и дать обоснование продуктового паритета.
2. Durable idempotency keys + process restore, тест с force-stop/ambiguous response.
3. Safe config update endpoint или explicit no-write restriction пока legacy destructive side effect существует.
4. `testDebugUnitTest`, `assembleDebug`, `assembleRelease` на Android SDK 37; проверить APK artifact/checksum.
5. Compose UI/instrumentation + Android 17 emulator/device smoke; accessibility/window/IME/role/error-state acceptance.
6. CI paths trigger на server contract/schema changes; финальный audit/TODO и archive regenerated from exact tested tree.


## Implementation update — Stage 7 (2026-10-25)

Реализованы в исходниках находки NAT-02/NAT-20 (durable idempotency ledger с 7-day fail-closed review, без raw task/answer text), remote subset NAT-03 (selected task, filter, event cursor и до 100 safe summaries), NAT-17 atomic workspace switching, NAT-18 fail-closed credential cleanup с retry UI и NAT-19 обязательная проверка body `api_version`. После проверки устранены недочёты обработки expired ledger, ошибки очистки после уже принятой сервером мутации и восстановления selected task при 401/403.

**Evidence:** source verifier PASS; XML 6/6 PASS; pure Kotlin ledger compile PASS; custom runtime harness 4/4 PASS. Android Gradle/JUnit/Compose/APK/API37 device/process-death tests остаются NOT RUN/BLOCKED. Подробности: `ANDROID_NATIVE_IMPLEMENTATION_STAGE_7.md`.


## Implementation update — Stage 8 (2026-10-25)

NAT-01 is now implemented in source. Added inventory, ticket and byte-stream endpoints against `contracts/v1/openapi.yaml`, with strict metadata validation (including required nullable `expires_at`), same-origin HTTPS ticket handling, bearer-free ticket transport, redirects disabled, 256 MiB/30-minute bounds, `Content-Length` and computed SHA-256 verification, one 410 refresh attempt, partial-file cleanup and verified-only private-cache ShareSheet via non-exported FileProvider. `artifact_available` events refresh the selected task inventory. `verify_native_source.py` passes; XML is 6/6; Kotlin/JVM source/contract harness is 27/27 using local API-compatible stubs. Three local review passes corrected filename validation ordering, null expiry schema handling, event refresh, stream failure cleanup and diagnostic redaction. This is not independent external-agent review. Gradle/JUnit/Compose/APK/API37 device/ShareSheet acceptance remains NOT RUN because the toolchain is unavailable.
