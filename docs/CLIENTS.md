# Клиенты и протокол ядра ASGEND

## Принцип архитектуры

`agent-orchestrator` — автономный сервер ядра. Он отвечает за очередь, SQLite, оркестратор, decision-модель, модели исполнителей, ресурсы, MCP, конфигурацию и журнал событий. Web UI — необязательный клиент, а не часть бизнес-логики. Клиенты могут обращаться к ядру по line-delimited JSON TCP API:

- `ping`, `health`
- `submit`, `list_tasks`, `status`, `events`, `answer`, `cancel`, `dead_letters`, `retry_task`
- `models`, `model_enable`, `model_disable`, `model_start`, `model_stop`
- `config_get`, legacy `config_save`, allow-listed `config_update_settings` (server/runtime/sandbox only)

Каждый запрос — один JSON object и завершающий LF; ответ — один JSON object и LF. При заданном `server.auth_token` клиент добавляет `auth_token` в запрос. `events` использует `after_id`, поэтому polling клиента может продолжаться без загрузки всей истории.
Для нативного Android guided config применяется `config_update_settings`: он изменяет только разрешённые поля основного config файла и не посылает назад model registry, agent profiles или credentials. Полно-бандловый `config_save` оставлен для совместимости старого административного редактора; новые формы настроек не должны использовать его для частичных изменений.

Интерактивный C CLI находится в `clients/asgend_cli.c` и устанавливается командой `asgend-cli`. Запуск без аргументов открывает меню; в чате задачи он опрашивает события и статус, показывает payload действий/инструментов, позволяет отвечать на вопросы и отменять задачу. Параметры подключения: `ASGEND_HOST`, `ASGEND_PORT`, `ASGEND_TOKEN` или аргументы `--host`, `--port`, `--token`.

## Кроссплатформенный Flutter-клиент

Путь: `clients/asgend_flutter/`. Один Dart/Flutter source tree предназначен для Linux desktop и Android. Он включает:

- список задач; новая задача открывает её чат сразу после принятия ядром;
- текущий статус, журнал событий оркестратора, агентов, моделей и инструментов;
- модальное окно вопроса с предлагаемыми вариантами и пользовательским текстовым ответом;
- продолжение сессии, отмену задачи;
- Nodes: локальные модели, `localhost`, LAN и Internet endpoints; состояние узлов и модели; enable/disable/start/stop;
- создание и редактирование профилей гибких агентов;
- отдельный режим Control Plane поверх HTTPS API: one-time pairing code или скопированный `asgend-pair://` QR payload, выбор workspace, nodes/tasks/events, role-gated submit/answer/cancel/retry, logout/revoke;
- выгрузка удалённых артефактов по одноразовому download ticket с потоковым ограничением размера и проверкой SHA-256 перед экспортом/шарингом.

### Сборка Flutter-клиента: Android и Linux

Для Android используется Flutter 3.29.3 / Dart 3.5+ и JDK 17. Android scaffold находится в `clients/asgend_flutter/android/`: manifest объявляет `INTERNET`, запрещает cleartext HTTP, отключает app backup/device transfer для локальных credentials и кеша, а версии AGP/Kotlin/Gradle зафиксированы. Gradle wrapper jar и launcher scripts генерируются шаблоном выбранного Flutter SDK, не хранятся как вручную скопированные бинарники:

```bash
cd clients/asgend_flutter
flutter doctor -v
flutter pub get
flutter analyze --no-pub
flutter test --no-pub
./scripts/bootstrap_android_wrapper.sh
flutter build apk --release --no-pub
```

CI выполняет те же шаги и публикует unsigned APK. Не добавляйте signing keystore или `key.properties` в Git; подпись production-релиза должна настраиваться отдельно из защищённых секретов. `flutter_file_dialog` закреплён на 3.0.2 для согласованности с Gradle 8.10.2 / AGP 8.7.3.

Linux runner не включён в исходный архив. Для desktop-сборки создайте platform runner из временного проекта, скопируйте только `linux/` и не заменяйте `lib/`. Установите `clang`, `cmake`, `ninja`, `pkgconf`, GTK 3 development headers, `make` и `libsecret-1-dev` (для платформенного secret store), затем выполните `flutter build linux --release`.

Проверено 2026-10-22 на закреплённом Flutter 3.29.3: `flutter analyze` — без замечаний, `flutter test` — 20/20, unsigned release APK собирается, приложение запускается в эмуляторе Android 14. Подробности — [`../IMPLEMENTATION-STATUS-2026-10-22-ITERATION-17.md`](../IMPLEMENTATION-STATUS-2026-10-22-ITERATION-17.md).

## Нативный Qt 6 клиент Linux

Исходники C++17/Qt Widgets находятся в `clients/asgend_qt/`. Добавлен базовый MainWindow поверх Core/Control Plane adapters: выбор профиля, безопасное pairing удалённого HTTPS Control Plane, task list/state/text filter, создание задачи, просмотр статуса/events, question/answer, cancel/retry, dead letters, node/agent/model views и скачивание артефактов через ограниченный checksum-verifying adapter. Model lifecycle actions доступны только когда backend объявляет соответствующую capability. Raw TCP core API разрешён только на loopback и не должен открываться в Интернет.

Для воспроизводимой проверки есть Qt Test unit tests, Linux CI workflow, desktop/AppStream metadata и CPack TGZ/DEB/RPM. 2026-10-22: сборка без предупреждений и тесты 8/8 на Qt 6.4.2 (Ubuntu 24.04), Fedora 42 и Qt 6.12 (Arch); DEB и RPM устанавливаются в чистые контейнеры, GUI стартует (offscreen), пакет удаляется.

## Android-релиз: состояние и roadmap

`clients/asgend_flutter/` — UI содержит локальный core flow и отдельный remote Control Plane flow. Для 1.0.0 проверены analyze/test, воспроизводимая сборка unsigned APK и запуск в эмуляторе; ручная проверка remote-сценария на физическом устройстве и сканирование QR запланированы после 1.0.0 (см. [`../TODO.md`](../TODO.md)).

## Подключение Android/desktop к ядру

На Linux ядро обычно по умолчанию слушает loopback, поэтому к нему нельзя обратиться с телефона через `127.0.0.1` телефона. Для удалённого клиента:

1. Выберите защищённый сетевой путь (предпочтительно VPN/WireGuard/SSH tunnel) между устройством и хостом ядра.
2. Настройте ядро слушать на адресе, достижимом через этот защищённый интерфейс, задайте длинный неповторяющийся `server.auth_token` и ограничьте firewall только IP/интерфейсом VPN.
3. В приложении задайте IP/hostname сервера, порт и токен.

Протокол ядра — raw TCP с аутентификацией, но без встроенного TLS. **Не открывайте этот порт прямо в Интернет**: bearer-токен передаётся без шифрования. Используйте VPN/туннель или добавьте TLS-терминацию до внешнего доступа. UI/Web API — отдельный необязательный компонент.

## Модель ресурсов

Ядро учитывает `MemAvailable` из `/proc/meminfo`, доступную VRAM (если доступен `nvidia-smi`) и оценки RAM/VRAM запущенных управляемых моделей из их профилей. Это предсказательная/admission модель, а не точное измерение памяти процесса на любой GPU/драйвере. Заполните `ram_estimate_mb` и `vram_estimate_mb`; для приоритетных этапов назначайте отдельную `analysis_model_id` или `tier: "large"`. Если модель завершила работу, процесс останавливается перед делегированием/следующей подзадачей, а новые старты проходят через общий resource-admission mutex. Если оценка памяти в профиле равна нулю, соответствующая проверка не выполняется.
