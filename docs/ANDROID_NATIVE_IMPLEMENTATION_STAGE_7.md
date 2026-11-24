# Нативный Android — отчёт реализации Stage 7: надёжность и API-контракты

**Дата:** 2026-10-25  
**База:** текущее дерево с нативными Stage 2–6B (основа/локальные сценарии/Control Plane pairing и remote tasks).  
**Findings:** NAT-02, NAT-03 (remote subset), NAT-17, NAT-18, NAT-19, NAT-20.

## Выполнено

1. **Атомарное переключение workspace (NAT-17).** Список nodes и первая страница tasks загружаются для целевой workspace в локальные переменные. Только после успеха обеих сетевых операций и локального commit публикуется новое workspace/state. При промежуточной ошибке старая workspace selection и её видимое состояние остаются согласованными. `refresh()` также собирает nodes/tasks до публикации snapshot.
2. **Fail-closed очистка credentials (NAT-18).** При HTTP 401 и при logout токен/identity/node/task состояние сначала отцепляются в памяти. Ошибка удаления Keystore/metadata больше не подавляется; есть явный статус и действие повторной очистки. Пока cleanup не завершён, authenticated клиент не остаётся активным.
3. **Проверка API version body (NAT-19).** Каждый успешный JSON response проверяется на numeric `api_version == 1` в теле дополнительно к необязательному response header. Отсутствующий, строковый или несовместимый version отклоняется как `version`; пустой/невалидный JSON отклоняется как `bad_response`.
4. **Durable idempotency ledger (NAT-02/20).** Создание remote task и команды answer/cancel/retry получают ключ, записанный синхронно в private preferences до mutating HTTP запроса. Хранятся только operation slot, SHA-256 digest, key и timestamp; исходный текст задачи/ответ и raw payload не сохраняются. Запись удаляется только после подтверждённого серверного результата. Неопределённый результат сохраняет ключ для повторной попытки идентичной операции.
5. **TTL и fail-closed recovery.** Неопределённые операции старше 7 дней не получают новый ключ автоматически. Пользователь видит предупреждение и может удалить устаревшую запись только после явного подтверждения, что проверил remote tasks/events. Ledger ограничен 128 unresolved records; при переполнении новая мутация блокируется, а не вытесняет запись молча.
6. **Восстановление remote view state (часть NAT-03).** По `user + workspace` сохраняются selected task ID, task filter, cursor и до 100 event summaries из разрешённого перечня типов. Raw event payload, credentials и task/answer text не сохраняются. На старте selected task повторно валидируется через сервер.

## Проверки, фактически выполненные в этой среде

| Проверка | Результат | Ограничение |
|---|---|---|
| `verify_native_source.py` | PASS | Статические source-contract assertions, не Kotlin/Compose compile |
| Android XML | 6/6 PASS | Разбор XML, без runtime проверки ресурсов |
| `RemoteOperationLedger.kt` standalone `kotlinc` | PASS | Чистый Kotlin ledger без AndroidX |
| Runtime harness ledger | 4/4 PASS | Повтор ключа, expired fail-closed, удаление и отказ durable write |
| JUnit `ControlPlaneClientContractTest` и `RemoteOperationLedgerTest` | NOT RUN | Нет Gradle/JUnit classpath и Android SDK |
| ViewModel/Compose compile | NOT RUN | Нет Gradle, AndroidX artifacts и SDK API 37; общий parser probe завершился по timeout и не считается PASS |
| API37 emulator/device/process-kill tests | NOT RUN | SDK/emulator отсутствуют |

## Исправления после проверки

- Обновлён source guardrail, который ожидал старую последовательность logout и не учитывал fail-closed detach-before-delete.
- Подтверждённая сервером мутация больше не представляется как неизвестная только из-за ошибки очистки локального ledger. Неудалённый ключ безопасно остаётся тем же для дедупликации.
- Ошибка восстановления selected task с 401/403 больше не подавляется как безобидная; 404 сбрасывает удалённую selection/cursor, временная ошибка сохраняет retry context.
- Повреждённый ledger читается fail-closed: он не превращается в пустой список, что могло бы выдать повтору новый ключ и допустить duplicate.

## Осталось открытым

- Process-kill/relaunch acceptance на устройстве и ViewModel/JUnit tests для workspace switch, 401 cleanup, body versions и ledger restore.
- Полное восстановление локального core task/session/event cursor (сейчас закрыта remote часть NAT-03).
- Remote artifact ticket/stream/verified export (NAT-01), non-destructive config endpoint (NAT-04), Android Gradle/APK/API37/device acceptance (NAT-05).

**Статус Stage 7:** source-level implementation complete for listed fixes; runtime build/device acceptance remains BLOCKED/NOT RUN. Не считать release-ready.
