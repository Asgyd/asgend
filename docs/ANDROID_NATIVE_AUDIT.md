# ASGEND Native Android — аудит текущей структуры и дизайна

**Дата анализа:** 2026-10-25  
**Исходная база:** `asgend-core-audit-final-cumulative-v2.tar.gz`  
**Цель:** нативное Android-приложение на Kotlin + Jetpack Compose, которое сохраняет функциональную модель CLI/TUI и существующего Android-клиента, но использует адаптивный Android UI.

## 1. Краткий итог

В репозитории уже есть Android-клиент на Flutter (`clients/asgend_flutter`) и Android runner для него, однако отдельного нативного приложения на Kotlin/Jetpack Compose нет. Flutter-клиент взаимодействует с ядром через JSON-lines TCP API и Control Plane через HTTPS; C runtime внутрь Android-клиента не встраивается. Поэтому native app должен быть отдельным клиентским модулем, а не переписыванием/портированием оркестратора на Android.

Потребуется две совместимые транспортные поверхности:

1. **Local core:** JSON-объект + `\n` поверх TCP. Запрос содержит `op`; при настроенном токене — `auth_token`. Сокет закрывается после ответа. Не включать сырые TCP-секреты в логи.
2. **Control Plane:** HTTPS API v1 для pairing, workspace, задач, событий, команд с проверкой роли, артефактов, logout/revoke. Нельзя автоматически подменять HTTPS endpoint на произвольный URL, доверять URL download ticket без проверки origin или ставить мутации в скрытую offline-очередь.

## 2. Исследованные исходники

- `clients/asgend_cli.c` — режим меню и командные сценарии CLI, RPC-клиент, follow-task/chat, streamed events.
- `include/asgend_tui.h`, `src/asgend_tui.c` — модель экранов/панелей, плиток, навигация, selection, collapse, resize, mouse/keyboard и потоковые превью.
- `src/server.c` — действующий JSON-lines RPC контракт и точные имена операций.
- `clients/asgend_flutter/lib/src/ui/local/` — реализованные клиентские экраны и существующие UX-решения local-core API.
- `clients/asgend_flutter/lib/src/ui/remote/remote_home.dart` и `lib/src/api/control_plane_client.dart` — Control Plane, pairing/session/task/artifact сценарии.
- `clients/asgend_flutter/lib/src/storage/` — модель настроек, секреты, экспорт артефактов и локальный cache.
- `contracts/v1/openapi.yaml`, `contracts/v1/schemas/`, `contracts/v1/examples/` — авторитетная схема удалённого API.
- `.github/workflows/flutter-android.yml` и `clients/asgend_flutter/README.md` — существующая сборка Flutter APK и известные ограничения.

## 3. Функциональная карта CLI/TUI

В интерактивном TUI рабочее пространство содержит три логические панели:

- **Слева:** список задач и To-do; выбор/открытие задачи и смена активной сессии.
- **В центре:** поток чата и событий выполнения; progress/action/file/tool/chat tiles; сворачивание/разворачивание, динамический режим; приватное reasoning и промежуточные аргументы инструмента не должны раскрываться.
- **Справа:** файлы/каталоги текущей рабочей папки и Goals; навигация по путям, текстовый preview, ограниченное редактирование и сохранение через API.

Командные сценарии CLI/TUI также содержат:

- создание задачи с priority `0..9`, открытие по ID/session, polling статуса и событий;
- ответы на clarification/question — выбранный вариант или свободный текст;
- кооперативная отмена и повтор failed/dead-letter задачи;
- health/queue status, drain/worker state;
- модели и ресурсы, enable/disable/start/stop для модели и decision-модели;
- MCP status;
- agents/config get/save, требование перезапуска при структурных изменениях;
- глобальные и task-scoped To-do/Goals;
- workspace list/read/read chunk/write и artifact list;
- история событий с курсором и периодическое обновление данных.

## 4. Существующее Flutter-приложение — важные паритетные функции

Flutter локальная часть уже показывает Tasks, Nodes, Agents и Resources, создаёт задачу и сразу открывает чат, принимает вопросы агента, отменяет задачи, работает с конфигурацией и registry моделей. Remote Control Plane включает workspace switching, pairing по коду или скопированной ссылке `asgend-pair://`, tasks/events, role-gated commands, артефакты с ограниченной загрузкой, SHA-256, export/share и logout/revoke. Native реализация должна не потерять эти возможности; camera QR scanner сейчас не входит в существующую Flutter-функциональность и не считается уже готовым поведением.

## 5. Дизайн-декомпозиция для Android

### Базовый паттерн

Не копировать буквальные TUI-панели на узкий телефон. Перенести ту же информационную архитектуру в нативные паттерны:

- compact window: Material navigation bar до пяти основных направлений; задача открывается отдельным detail screen с back navigation;
- medium/expanded window: navigation rail/drawer и list-detail или 2–3-pane workspace, когда достаточно ширины;
- Workspace: на телефоне список задач → чат, Files/Plan доступны через секции/вкладки/связанные маршруты; на планшете список задач, лента событий и панель файлов/плана рядом;
- динамические состояния TUI становятся карточками timeline с семантическими состояниями, а не анимацией терминального текста;
- priority, status, pending clarification, errors и progress должны быть понятны по подписи и иконке, а не только по цвету;
- клавиатура, TalkBack, крупный текст, landscape, split-screen и predictive back учитывать наравне с touch.

### Material 3 Expressive

Использовать официальную Jetpack Compose реализацию Material 3 / M3 Expressive, включая theme roles, типографику, shape scale, выразительные компоненты и motion там, где это улучшает feedback. Dynamic color включать на поддерживаемых версиях Android и иметь собственную fallback палитру. Не имитировать фирменный стиль набором произвольных ярких цветов или бесцельной анимацией. Обеспечить достаточный контраст и уважать системное уменьшение анимации, если это поддерживается выбранным API.

### Android 17

Android 17 соответствует API level 37. Для целевой платформы выставить `compileSdk = 37`, `targetSdk = 37`, соблюдать совместимость инструментов сборки. Отдельно проверить документированные изменения API/behavior для target 37, глобальные клавиатурные shortcuts, восстановление IME при rotation/configuration change, edge-to-edge/system bars, background execution/permissions и WebView User-Agent только если WebView появится в архитектуре. Не добавлять WebView в обход нативных экранов без необходимости.

## 6. Безопасность и сеть

- Android `localhost`/`127.0.0.1` — само устройство; подключение к удалённому core требует VPN/LAN endpoint. Raw TCP без TLS допустим только по доверенному VPN/SSH tunnel и после явного подтверждения пользователя.
- HTTPS Control Plane — предпочтительный удалённый сценарий. Для него проверять HTTPS scheme, endpoint policy и происхождение artifact download URL.
- Token хранить за Android Keystore; не класть его в обычные preferences, analytics, crash logs, exception text или clipboard без явного действия пользователя.
- Не скрывать серверные ошибки и не объявлять команду успешной до подтверждения сервера.
- Сохранить idempotency semantics для повторной отправки задачи, не создавать duplicate task при потерянном ответе.
- Polling должен иметь backoff, отменяться при закрытии экрана/переходе lifecycle в background и не создавать пересекающиеся запросы.
- Не отображать raw reasoning/private planner JSON или промежуточные секретные аргументы инструментов.

## 7. Текущие ограничения анализа

- В окружении отсутствуют Android SDK/platform API 37 и Gradle; `kotlinc` доступен, но не заменяет Android/Compose compiler. Сборка APK/эмулятор в этой среде не может считаться проверенной.
- Нативный клиент не существует в исходном дереве; оценка UI ниже — перенос требований из CLI/TUI и existing Flutter source, а не визуальное ревью готового Kotlin UI.
- Отдельный внешний сабагент недоступен. Выполнять три локальных ревью-прохода допустимо, но они не являются независимой агентской оценкой.

## 8. Источники внешней спецификации

- Android 17 SDK setup / API 37: https://developer.android.com/about/versions/17/setup-sdk
- Android 17 behavior changes: https://developer.android.com/about/versions/17/summary
- Jetpack Compose Material 3 / Expressive: https://developer.android.com/develop/ui/compose/designsystems/material3
- Material 3 release status (stable 1.4.0, 2026-10-21): https://developer.android.com/jetpack/androidx/releases/compose-material3
- Compose BOM setup (2026.09.00): https://developer.android.com/develop/ui/compose/setup-compose-dependencies-and-compiler
- Material 3 Adaptive (stable 1.3.0): https://developer.android.com/jetpack/androidx/releases/compose-material3-adaptive
- Compose semantics testing/accessibility: https://developer.android.com/develop/ui/compose/testing/semantics

## 9. Рекомендация

Создавать самостоятельный `clients/asgend_android` без миграции или удаления Flutter-клиента. Делить реализацию на incremental stages: (1) audit/spec, (2) app/build/theme/navigation, (3) local core task/chat/plan/workspace, (4) runtime/models/agents/config, (5) remote Control Plane and artifacts, (6) UI/accessibility/security regression + CI/device build. На каждом этапе обновлять этот отчёт, decomposition и TODO и выпускать архив с checksum.


## Implementation update — 2026-10-25

Stage 2 is implemented in source and packaged separately. Stage 3 now wires Workspace, To-do/Goals and Files destinations to the actual core RPC rather than placeholders: submit/list/filter/status/events/answer/cancel/retry, plan CRUD/state changes, workspace listing, bounded chunk fallback and a 1 MiB UTF-8 text editor. Task events are cursor-polled with event-ID deduplication and exponential backoff; explicitly named private-reasoning/model-payload event types are filtered from the timeline. Unit tests and a transport seam are present but could not be executed because the authoring environment lacks Gradle and Android SDK/API 37. Runtime/models/config management and the HTTPS Control Plane are still planned, not implemented.


## Native implementation update — Stage 4A (2026-10-25)

The Runtime destination now exposes core-backed health, model registry/controls, agent listing, resource catalog and MCP status. Sensitive fields returned by the legacy config/model APIs are excluded from UI projections; runtime errors displayed to users are generic. These screens have not yet been compiled/tested via Android Gradle because the current work environment lacks SDK/Gradle; source/contract review must not be confused with an APK build. Guided configuration editing remains next and must not display API keys or auth tokens.


## Configuration editor safety finding — Stage 4B (2026-10-25)

The core's existing `config_get`/`config_save` API exposes a multi-file bundle: `main`, `models`, and `agents`. The config reader intentionally filters legacy fixed-role agent files, and the save path deletes agent JSON files not in the submitted agent array. A client that saves an unrelated setting can therefore cause this existing cleanup side effect. The native UI discloses this behavior and requires acknowledgement; a narrower core endpoint that only updates an explicit allow-list of main-config settings remains a recommended follow-up. The native form edits only validated operational limits and sandbox controls, never renders credential/path/model-registry JSON, preserves hidden values only in transient ViewModel memory for the core's bundle contract, and avoids logging/persisting that object. Android build acceptance is still pending SDK/Gradle CI.


## 10. Implementation update — Stage 5 (2026-10-25)

The standalone native app now covers the remaining local CLI/TUI operations in source: `dead_letters` for terminal failed tasks, `artifact_list` for workspace-confined generated artifact inventory with MIME/size/SHA-256 and scan truncation details, and `lsp_diagnostics` for an explicitly confirmed relative file path under the core-configured project root. Artifact bytes are read only through the existing workspace-confined `workspace_read_bytes` endpoint. Client path validation rejects absolute paths, NUL, backslash and `.`/`..` components; the C core independently resolves and enforces the project-root boundary and 5 MiB regular-file limit. The editor now clears its dirty flag only after a successful core write. Repository contract tests cover bounded dead-letter listing, artifact task scoping, binary chunk bounds/response envelope, absolute/traversal path rejection, LSP boundaries, plan revision long-poll and invalid revision/timeout inputs. The contract harness was compiled and run with local API-compatible stubs: 14/14 tests passed; this does not substitute for Android Gradle/JUnit. The dependency-free source verifier passes and all six Android XML files parse. Android Gradle/Compose/JUnit/device execution remains pending because this environment has no Android SDK 37 or Gradle wrapper runtime.


## 11. Implementation update — Stage 6A: Control Plane pairing (2026-10-25)

A separate native HTTPS Control Plane client is now present; it is deliberately distinct from the local core JSON-line TCP client. The request transport uses platform certificate validation, never enables insecure TLS, rejects non-HTTPS endpoint schemes, strips a terminal `/v1` base suffix, rejects URL credentials/query/fragments/path traversal, disables redirects, caps JSON bodies at 2 MiB, adds request IDs, and checks the `X-ASGEND-API-Version` header when present. Bearer credentials are only added to the Authorization header and never to a URL.

Pairing calls `/v1/pairing/redeem` without auth, passes the one-time code only in JSON, validates `ucp_` token length/character grammar and role, and does not save the code. The remote user token is encrypted with a separate Android Keystore alias and preference slot so pairing/logout do not overwrite the local core token. The Settings screen displays `/v1/me` identity/membership and workspace nodes, and explicit sign-out attempts server-side revocation before deleting local remote credentials even when the server is offline.

The source was cross-checked against `contracts/v1/schemas/common.schema.json`, `pairing.schema.json`, `workspace.schema.json` and OpenAPI paths. The local API-compatible contract harness passed 7/7 tests for endpoint validation, pairing payload, Authorization-header use, malformed-token rejection, API version/redirect policy, no-token network guard and node path encoding. Android Gradle/Compose/JUnit/device acceptance remains pending because this authoring environment has no Android SDK/Gradle. Remote task operations and artifact delivery are not yet implemented; they remain Stage 6B/6C.


## 12. Implementation update — Stage 6B: Control Plane tasks and workspaces (2026-10-25)

The native client now has a workspace selector based on refreshed `/v1/me` membership, remote task list cursor/filter support, task submission with retry-stable idempotency, detail/event long-poll, answer/cancel/retry commands, explicit cancel/retry confirmation and role-based UI gates. The Control Plane client uses the canonical v1 OpenAPI field names and keeps local TCP RPC separate from HTTPS. Mutations refresh membership immediately before sending and still rely on server-side authorization.

Event polling uses `after_event_id`, 25-second `wait_ms` and an additional 20-second transport margin. The cursor is deduplicated and monotonic unless the server explicitly signals a cursor reset. Unknown events advance the cursor without rendering payload data. Known events are transformed to safe text labels rather than keeping raw payload JSON in the screen model. The Android client does not queue mutations offline.

A local Kotlin/JVM harness with API-compatible stubs compiled the HTTPS client and contract tests, with 14/14 tests passing. Static source-contract and XML checks pass. These are not official Android JUnit, Gradle, Compose, APK or device results; no Android SDK/platform 37 or Gradle installation is available here. Process-death restoration of remote event history is left open for Stage 7.
