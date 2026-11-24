# ASGEND — отчёт о реализации, итерация 9 (завершение)

Дата: 2026-10-21

## Цель

Продолжить WP5 (Flutter/Android) из `TODO.md`: подключить существующие Control Plane API/state/storage модули к реальному экрану и закрыть основные lifecycle/security paths.

## Реализация

### Control Plane UI

- Добавлен `clients/asgend_flutter/lib/src/ui/remote/remote_home.dart` и подключён из app bar локального клиента через кнопку Control Plane (HTTPS).
- Экран pairing принимает endpoint + одноразовый pairing code и device name. Поддерживается вставка скопированного `asgend-pair://` payload (например, полученного из QR); встроенного camera QR scanner пока нет.
- Remote dashboard показывает статус соединения, stale/offline cache banner, список задач и nodes, workspace switching, текущую роль и logout/revoke.
- Создание задач, ответы, cancel/retry доступны только для `owner`/`operator` и только создание блокируется при offline; неуспешные мутации отображаются, автоматически в очередь не ставятся.
- Детальная страница задачи подключена к `EventFollower`: cursor persisted через `TaskCache`, события дедуплицируются, состояние и artifacts обновляются.
- Артефакты скачиваются через download ticket в приватный app cache с ограничением размера и потоковой SHA-256 проверкой; при экспорте потенциально исполняемых файлов запрашивается подтверждение пользователя.
- Logout вызывает серверный revoke (best effort), затем удаляет credential и remote cache локально.
- Создан `src/ui/app_environment.dart`, ранее импортировавшийся локальным UI, но отсутствовавший в source tree.

### Надёжность

При интеграции обнаружено и исправлено поведение `RemoteSession`: initial refresh при offline состоянии заменял one-shot retry timer периодическим таймером, который запускал refresh только в online состоянии. Теперь после старта/pairing периодический polling включается только после успешного refresh; retry timer остаётся активным и восстанавливает соединение.

### Тесты

- Добавлен `test/remote_session_test.dart`: pairing, bearer token только в secure store, чтение `/v1/me`, nodes/tasks, viewer role, logout/revoke, очистка cached tasks/cursor.
- Добавлен тест автоматического восстановления после первого временного HTTP 503 — он проверяет, что повторный запрос выполняется и session переходит из offline в online.
- В `test/client_security_test.dart` добавлена проверка парсинга pairing links и redaction одноразового кода в `toString()`.
- Обновлены `TODO.md`, корневой README, `clients/asgend_flutter/README.md` и `docs/CLIENTS.md`.

## Проверки

| Проверка | Результат |
|---|---:|
| Контракты (`tests/contracts/test_contracts.py`) | 31 passed, 1 skipped |
| Control Plane (`server/tests`) | 10 passed |
| Node-agent (`node_agent/tests`) | 37 passed |
| Короткие CTest, длительный `integration` исключён | 16/16 passed |
| Анализ относительных Dart imports и объявленных package dependencies | Все пути разрешаются; 9 внешних package imports объявлены |
| YAML: `pubspec.yaml`, Android CI workflow | Разбираются |
| Android XML | 6 файлов разбираются |
| Android security manifest | cleartext HTTP запрещён, backup отключён |
| Bootstrap Android Gradle wrapper shell syntax | `bash -n` passed |
| Баланс скобок в изменённых Dart-файлах | Проверка прошла (статическая эвристика, не Dart analyzer) |

## Не проверено / ограничения

- Flutter и Dart SDK отсутствуют в этой среде; `flutter pub get`, `flutter analyze`, `flutter test`, Gradle и фактическая APK-сборка не запускались. Добавленные Dart tests являются исходниками тестов, их успешное выполнение здесь не утверждается.
- `pubspec.lock` пока не создан; для воспроизводимой сборки его нужно сгенерировать `flutter pub get` и добавить в репозиторий после проверки совместимости plugin/toolchain.
- QR payload можно вставить из clipboard; встроенного сканирования камерой пока нет.
- Remote UI интегрирован в рабочий поток, но device/emulator acceptance, stale-cache behavior в реальной сети и artifact export через SAF/share sheet ещё нужно проверить на Android.
- Qt SDK и Docker/PostgreSQL runtime по-прежнему не доступны для acceptance; полный end-to-end CP ↔ node-agent ↔ core и длительный CTest `integration` остаются открытыми.

## Следующие шаги

1. В окружении с Flutter 3.29.3/Dart 3.5+: `flutter pub get`, `flutter analyze`, `flutter test`, затем Android release APK build; исправить найденные analyzer/API совместимости и зафиксировать `pubspec.lock`.
2. Добавить widget tests с внедряемыми platform stores/fake HTTP transport и проверить pairing, viewer role, error/offline states, task mutations и artifact export.
3. Выполнить device/emulator acceptance, затем реальный CP ↔ node-agent ↔ core end-to-end сценарий и deployment/restore acceptance.
