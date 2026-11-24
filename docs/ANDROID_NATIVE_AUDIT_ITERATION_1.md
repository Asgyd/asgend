# ASGEND Native Android — итоговый аудит (итерация 1/3)

**Дата:** 2026-10-25  
**База:** `clients/asgend_android` из текущего дерева ASGEND, с локальным core workspace и Control Plane Stage 6B.  
**Охват прохода:** архитектура, декомпозиция, функциональный паритет и статус выполнения этапов. Это исходный проход; security/protocol и UX/build будут пересмотрены в итерациях 2 и 3.

## Краткое заключение

Нативный модуль Kotlin + Jetpack Compose действительно существует и отделён от Flutter-клиента. Он реализует локальный core workflow (задачи, события/чат, Plan, Files, Runtime/config) и Control Plane pairing/remote tasks. Статический source verifier проходит. Однако модуль **нельзя считать Android-release-ready**: в доступной среде не выполнены Gradle/JUnit/Compose/APK/device acceptance, а Stage 6C remote artifact delivery, process-death restoration и полноценные UI/accessibility checks остаются незакрытыми.

### Проверки, реально выполненные в этом проходе

- `python3 clients/asgend_android/tools/verify_native_source.py` — **PASS**.
- Проанализированы `docs/ANDROID_NATIVE_DECOMPOSITION.md`, `docs/ANDROID_NATIVE_TODO.md`, `docs/ANDROID_NATIVE_STAGE_LOG.md`, Stage 6B, manifest, Gradle/CI, репозитории, ViewModels и основные Compose screens.
- В `clients/asgend_android/app/src/test` найдено 5 JVM test source-файлов; `src/androidTest` с instrumented Compose/device tests отсутствует.
- `gradle`, Gradle wrapper JAR, Android SDK/platform 37 и emulator в среде не обнаружены. Соответственно APK/Compose/JUnit сборка здесь не запускалась.
- Проверка официальных Android Developers страниц подтверждает: Android 17 — API 37; M3 `1.4.0` обозначен как stable на 2026-10-21; AGP 9.4 требует минимум Gradle 9.6.0. Ссылки приведены в конце.

## Находки

### ANDR-001 — Stage 6C remote artifact delivery не реализован

**Приоритет: P1 для заявленного паритета; состояние: OPEN.**

`ControlPlaneClient.kt` содержит pairing/identity/nodes/tasks/events/answer/cancel/retry/logout, но не содержит API для remote artifact inventory, ticket creation или скачивания. В `RemoteControlScreen.kt` нет UI для remote artifacts; тип `artifact_available` в timeline сводится к уведомлению «An artifact is available». В `docs/ANDROID_NATIVE_TODO.md` Stage 6C ещё полностью `[ ]`.

**Риск/эффект:** нативная версия не покрывает часть Control Plane возможностей существующего клиента и утверждённой декомпозиции: список артефактов, ticket, same-origin guard, потоковое скачивание, проверку размера/SHA-256 и безопасный share/export.

**Критерий закрытия:** реализованы контракты из `contracts/v1/openapi.yaml` и схем; ticket URL проверяется до открытия; bearer-токен не передаётся в download URL/на посторонний origin; есть streaming size cap, SHA-256, очистка неудачных временных файлов, FileProvider/ShareSheet только после проверки и контрактные тесты.

### ANDR-002 — критичные операции и event cursor не переживают process death

**Приоритет: P1 для безопасного повторения удалённых команд; состояние: OPEN.**

`NativeRemoteViewModel.kt` хранит `pendingCreateSignature`, `pendingCreateKey` и `commandKeys` только в полях ViewModel (строки 26–28). Сам ViewModel также хранит выбранную задачу и `remoteEventCursor` в обычных `mutableStateOf` (примерно строки 39–50). Существующий TODO отмечает восстановление курсора, но не отмечает потерю ключей идемпотентности при завершении процесса.

**Риск/эффект:** после неясного результата HTTP mutation (сервер мог принять команду, а ответ не дошёл) процесс может быть уничтожен. Повтор создания задачи/answer/cancel/retry после нового запуска получит новый idempotency key и не обязан дедуплицироваться Control Plane. Утрата event cursor также приводит к потере видимого timeline-контекста или повторной загрузке с нуля.

**Рекомендация:** privacy-reviewed bounded persistence для ожидающих request keys и точного хеша неизменяемого payload, workspace/task/event cursor и выбранного task ID. Ключ для повторной попытки должен переиспользоваться только с идентичным signature; истёкшие/подтверждённые ключи очищать. Не сохранять секреты/сырые event payloads.

### ANDR-003 — нативный APK и Compose ещё не прошли обязательную сборку

**Приоритет: P1 для выпуска; состояние: BLOCKED/NOT VERIFIED.**

Файлы Gradle и workflow присутствуют, но в текущей среде нет `gradle`, `gradle-wrapper.jar`, Android SDK/platform 37 или emulator. В текущем дереве нет `src/androidTest`; Compose tests не обнаружены. `verify_native_source.py` проверяет текстовые контрактные признаки, а не Kotlin type-check/Compose API compatibility.

**Риск/эффект:** ошибки связывания Compose, несовместимые API, manifest/target SDK issues и runtime/UI дефекты могут остаться незамеченными, несмотря на PASS статического checker.

**Критерий закрытия:** успешный внешний запуск `testDebugUnitTest`, `assembleDebug`, `assembleRelease`, проверка наличия unsigned APK, Android 17 emulator smoke, плюс instrumented tests для ключевых сценариев. В отчёте должен быть настоящий run ID/log, а не только наличие workflow.

### ANDR-004 — фактический M3 Expressive/UI acceptance не доказан

**Приоритет: P2; состояние: PARTIAL.**

Theme использует Material 3 `1.4.0`, dynamic colors и custom shape scale (`AsgendTheme.kt`), но основной UI преимущественно построен из стандартных `Scaffold`, `NavigationBar`, `NavigationRail`, `Card`, `Button`, `OutlinedTextField`. По исходникам не обнаружены Compose `androidTest`, accessibility semantics tests, responsive-window acceptance или проверка motion/reduced-motion. Это не означает, что применение M3 неверно; означает, что требование «Material 3 Expressive + Android 17 adaptation» пока подтверждено версией/темой, но не целостным визуальным и accessibility acceptance.

**Критерий закрытия:** композиционная ревизия на compact/medium/expanded окнах, TalkBack, крупном шрифте, landscape/IME, edge-to-edge/back, светлой/тёмной и dynamic/fallback palette, с Compose semantics/UI тестами и ручным скриншотным приёмочным листом. Отдельно подтвердить Android 17 API 37 target behavior changes.

### ANDR-005 — CI не запускается при изменениях API-контракта ядра

**Приоритет: P2; состояние: OPEN.**

`.github/workflows/native-android.yml` ограничивает запуск путями `clients/asgend_android/**`, `docs/ANDROID_NATIVE_*.md` и самим workflow. Изменения `contracts/v1/openapi.yaml`, JSON schemas, `src/server.c` или `docs/PROTOCOL.md` сами по себе могут не запустить native Android workflow.

**Риск/эффект:** клиентские контрактные тесты не обязательно прогонятся при изменении API, которому приложение подчиняется.

**Рекомендация:** добавить в `push.paths`/`pull_request.paths` `contracts/v1/**`, `src/server.c`, `docs/PROTOCOL.md` и связанные API contract docs; убедиться, что native workflow триггерится и от schema changes.

### ANDR-006 — конфигурационная запись зависит от потенциально разрушительного legacy endpoint

**Приоритет: P1/P2; состояние: MITIGATED, NOT CLOSED.**

`ANDROID_NATIVE_DECOMPOSITION.md` и Stage 4B note сообщают, что legacy `config_save` принимает полный bundle и может удалить legacy fixed-role agent files, не возвращённые `config_get`. UI показывает предупреждение и просит подтверждение, но пользовательское подтверждение не устраняет риск ошибочного удаления/рассинхронизации конфигурации.

**Рекомендация:** предпочесть серверный non-destructive PATCH/update endpoint с schema validation и атомарным merge. До его появления оставить явное предупреждение; предусмотреть бэкап/rollback и acceptance-тест сохранения невидимых legacy-профилей. Это не следует обозначать как полностью исправленное только из-за предупреждающего текста.

## Матрица функционального покрытия

| Область из декомпозиции | Статус в исходниках | Аудиторское заключение |
|---|---|---|
| Native foundation / API 37 / M3 theme | Реализовано в исходниках | APK/Compose build не проверен |
| Local tasks/chat/events/clarification/cancel/retry | Реализовано в исходниках | Нужны process-death/UI tests |
| To-do/Goals + revision long-poll | Реализовано в исходниках | Нужны lifecycle/device tests |
| Files/edit/LSP/local artifacts | Реализовано в исходниках | Статика/контракт не заменяют Android acceptance |
| Runtime/models/resources/MCP/agents/config | Реализовано в исходниках | `config_save` legacy side effect остаётся архитектурным риском |
| Control Plane pairing/identity | Реализовано в исходниках | Android runtime не выполнен |
| Control Plane remote tasks/events/role gate | Реализовано в исходниках | Idempotency/cursor не персистентны через process death |
| Control Plane remote artifact delivery | Не реализовано | P1 feature gap |
| Compose/accessibility/adaptive/Android 17 acceptance | Не подтверждено | P1 release gate / P2 design gap |

## Официальные ссылки на платформу и зависимости

- [Android 17, behavior changes for all apps](https://developer.android.com/about/versions/17/behavior-changes-all) — проверять изменения, применимые независимо от target.
- [Android 17, behavior changes for apps targeting API 37](https://developer.android.com/about/versions/17/behavior-changes-17) — требуется target-specific acceptance.
- [Compose Material 3 release notes](https://developer.android.com/jetpack/androidx/releases/compose-material3) — stable `1.4.0` отмечена на 2026-10-21.
- [Android Gradle Plugin compatibility](https://developer.android.com/build/releases/about-agp) — AGP 9.4 требует Gradle 9.6.0; конфигурация репозитория совпадает с этой минимальной парой.
- [Material 3 in Compose](https://developer.android.com/develop/ui/compose/designsystems/material3) — dynamic color доступен на Android 12+, с fallback схемой для старых устройств.

---

**Итог итерации 1/3:** аудит архитектуры и функционального паритета сформирован. Дальше проверить безопасность/протоколы и устойчивость, затем визуальную адаптацию/CI и повторно свести итоговые статусы.
