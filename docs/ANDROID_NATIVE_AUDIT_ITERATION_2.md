# ASGEND Native Android — аудит итерация 2/3: безопасность и протоколы

**Дата:** 2026-10-25  
**Фокус:** HTTPS/TCP security boundary, credential lifecycle, API schema validation, mutation idempotency, workspace state consistency.  
**Связь:** уточняет и расширяет `ANDROID_NATIVE_AUDIT_ITERATION_1.md`; это source review, не penetration test.

## Проверки и положительные контроли

- `python3 clients/asgend_android/tools/verify_native_source.py` повторно завершился `PASS`.
- `CoreRpcClient` явно ограничивает JSON-line request/response и использует timeout; приложение не маскирует незашифрованный TCP как TLS и требует подтверждения VPN/SSH.
- `ControlPlaneClient` требует HTTPS, не следует HTTP redirects, использует системную TLS-проверку, не принимает userinfo/query/fragment и ограничивает JSON body 2 MiB; bearer добавляется в `Authorization`, а не в URL.
- `SecureTokenStore` использует AES/GCM и отдельные Android Keystore aliases/preferences для local core и Control Plane; backup/extraction rules исключают app preferences/data.
- Remote mutations перечитывают `/v1/me` и допускают только `owner`/`operator`; это дополнительная клиентская проверка, server authorization остаётся authoritative.
- Remote event UI строит allow-listed summary и не хранит raw event payload в presentation model. Локальный artifact export (отдельная функция, не remote artifacts) ограничен по размеру, сверяет байты/SHA-256 и открывает только private cache через non-exported FileProvider.

Эти проверки подтверждают наличие конкретных защит в source, но не тестируют реальный Android Keystore, сетевые interceptors, TLS, OS lifecycle или все server edge cases.

## Дополнительные находки

### ANDR-007 — workspace switch может оставить смешанное/неверно описанное состояние

**Приоритет: P2 · OPEN.**

В `clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/ui/remote/NativeRemoteViewModel.kt`, функция `selectWorkspace`, последовательность примерно строк 137–164:

1. берётся свежий список membership;
2. новый workspace сохраняется в `SettingsStore` и в `settings`;
3. затем загружаются nodes;
4. только после успешного nodes запроса очищаются данные предыдущей remote задачи и загружается список задач.

Если `listNodes(target.workspaceId)` завершается ошибкой, in-memory/persisted selected workspace уже изменён, но `nodes` и task/event state могут относиться к предыдущему workspace. Catch показывает сообщение “The previous workspace remains selected”, которое в таком пути неверно.

**Исправление:** либо сначала получить target workspace data и только затем атомарно commit selection; либо при переключении сначала очистить все workspace-bound state, показать выбранную рабочую область с явным `refresh failed` и не утверждать, что выбран предыдущий workspace. Добавить тест сбоя nodes/tasks на каждом шаге переключения.

### ANDR-008 — 401 credential cleanup подавляет ошибку сохранения

**Приоритет: P2 · OPEN.**

`NativeRemoteViewModel.onControlPlaneFailure` (примерно строки 373–393) вызывает `runCatching { store.clearControlPlaneSettings() }`, подавляет ошибку и немедленно очищает token только в памяти. `SettingsStore.clearControlPlaneSettings()` может выбросить exception, если не удалось удалить зашифрованный token или metadata. В таком случае UI всё равно сообщает “Session expired · pair again”, но не различает успешное и неуспешное постоянное удаление локального credential.

**Исправление:** сохранить безопасное fail-closed состояние UI, но не подавлять ошибку молча: добавить флаг `credentialCleanupFailed`, нейтральное сообщение “session disabled in memory; local credential cleanup needs retry”, action повторной очистки и телеметрию без значений токена. Не возвращать token в видимое состояние и не печатать исключение/секрет в logcat. Acceptance test должен имитировать ошибку Keystore prefs commit.

### ANDR-009 — тело API response `api_version` не проверяется

**Приоритет: P2 · OPEN.**

`ControlPlaneClient.request()` проверяет `X-ASGEND-API-Version` только когда заголовок присутствует (примерно строки 399–405). При этом OpenAPI schemas, например `contracts/v1/schemas/list.schema.json` (`task_list`, `event_page`) и `contracts/v1/schemas/requests.schema.json` (`health_response`, `download_ticket_response`), требуют поле `api_version` в JSON body. Сейчас body version игнорируется; ответ может иметь заголовок `1`, но несовместимое `api_version` или пропустить required field, а код продолжит разбирать его как подходящий.

**Исправление:** после JSON parse проверять `api_version`, когда его требует контракт endpoint; отвергать missing/mismatched body version с `ControlPlaneException(errorCode="version"/"bad_response")`. Сохранить backward behavior только если конкретный endpoint/schema действительно допускает отсутствие поля. Добавить контракты header/body одинаковые, несовпадающие и отсутствующие.

### ANDR-010 — durable-command idempotency ledger только in-memory

**Приоритет: P1 · OPEN; уточняет ANDR-002.**

Создание задачи хранит `pendingCreateSignature`/`pendingCreateKey`; answer/cancel/retry — `commandKeys`, все обычные поля `NativeRemoteViewModel` (строки 26–28 и 206–220, 321–350). Они не переживают process death. OpenAPI использует `idempotency_key`, чтобы повторять durable command без дубликатов, но после force-stop клиент может сгенерировать новый ключ для payload, который сервер уже принял, а ответ потерялся.

**Исправление:** persist bounded pending request ledger до отправки: operation, workspace/task/node ID, key, digest canonical payload, creation time/state. Сами сырые task/answer body по умолчанию не записывать; для повторной отправки UI должен попросить пользователя заново ввести payload, проверить digest и только затем использовать прежний ключ. Удалять запись после confirmed response; TTL cleanup — явный. Тест: “server accepted, response dropped, app killed, relaunch, same payload” не приводит к созданию нового key.

### ANDR-011 — процессное восстановление task/cursor отсутствует и это отдельный контракт

**Приоритет: P2 · OPEN; уточняет ANDR-002.**

`selectedRemoteTask`, `remoteEventCursor`, `remoteEvents`, выбранный top-level destination и текущий local active task/session в основном живут в Compose/ViewModel state. `rememberSaveable` для верхней destination помогает при конфигурационном изменении, но не обеспечивает bounded durable restoration после процесса смерти. В OpenAPI для событий прямо сказано persist `last_event_id` и продолжать с него после reconnect; в `docs/ANDROID_NATIVE_DECOMPOSITION.md` этот критерий есть.

**Исправление:** хранить активный route/task ID/workspace ID и курсор; event summaries хранить ограниченно и privacy-reviewed (без payload); после cold start перепроверить права/существование task и серверное состояние до восстановления. Не восстанавливать в состояние успешного выполнения старый cached status без refresh.

### ANDR-012 — workspace/event/test coverage должна быть привязана к контрактным edge cases

**Приоритет: P2 · OPEN.**

Предыдущий PASS API-compatible harness покрывает известные happy/negative contracts, но отсутствуют actual Android tests, и перечисленные в ANDR-007…011 случаи не имеют test evidence в текущем `src/test`.

**Приёмка:** fake transport tests для body version/HTTP status; ViewModel tests на partial workspace switch, key persistence и cleanup fail; process restoration; real Android instrumentation для Keystore/lifecycle/network path. Static verifier может дополнительно гарантировать наличие тестов, но не должен объявлять их выполненными без реального Gradle/JUnit run.

## Что не следует ошибочно называть дефектом

- Отсутствие offline mutation queue является корректной безопасной характеристикой: неизвестный исход команды не следует скрыто повторять с новым ключом.
- Проверка `owner`/`operator` в UI/ViewModel не заменяет серверную авторизацию, но является полезным дополнительным UX-гейтингом.
- Для local core raw TCP остаётся plaintext; существующее подтверждение VPN/SSH — осознанная граница, а не TLS. Это нужно документировать и не рекламировать как transport encryption.
- Не удалось подтвердить или опровергнуть build-time компиляцию Compose, поскольку toolchain отсутствует; это `NOT RUN`, не доказанная compile error.

**Итог итерации 2/3:** защита endpoint/transport/secret separation по source review выглядит дисциплинированно, но появились конкретные reliability/schema-validation находки ANDR-007…012. Следующий проход проверит дизайн, lifecycle, CI и единообразие статусов/критериев приёмки.
