# ASGEND — отчёт о реализации, итерация 10

Дата: 2026-10-21

## Цель

Продолжить WP5 по `TODO.md`: сделать удалённый Flutter UI удобным для автоматизированной проверки без платформенных каналов и реального Control Plane, затем добавить widget tests для pairing, read-only roles, ошибок сети и завершения сессии.

## Реализация

### Тестируемый composition root

- `clients/asgend_flutter/lib/src/ui/app_environment.dart` теперь принимает `SettingsRepository`, factory HTTP clients (`http.Client Function()`) и `ArtifactExporter`.
- Значения по умолчанию используют реальные platform stores, `http.Client` и `PlatformArtifactExporter`; production-путь не меняет политику подключения.
- `RemoteHomePage` принимает optional `AppEnvironment`, а состояние создаёт `RemoteSession` с переданным transport и сохраняет exporter до detail-screen.
- Добавлен стабильный key `remote-task-list`, чтобы widget test мог прокручивать список задач, не зависеть от порядка внутренних scrollables Flutter.

### Widget tests

Добавлен `clients/asgend_flutter/test/remote_home_widget_test.dart`:

1. Pairing к mock Control Plane с проверкой, что одноразовый код отправляется в теле запроса, bearer-token хранится только в `SecretStore`, а не в prefs/URL.
2. Роль `viewer`: read-only сообщение, task/node dashboard и отключённая кнопка создания задачи.
3. HTTP 503 после pairing: UI показывает offline/retry/error и не удаляет token автоматически; logout всё равно очищает локальные credentials.
4. Валидация пустого endpoint/code без сетевого запроса.

Тест использует `MemoryKeyValueStore`, `MemorySecretStore` и `http/testing.dart` `MockClient`; системный storage/dialog/share plugins не вызываются.

## Проверки

| Проверка | Результат |
|---|---:|
| `cmake -S . -B /tmp/asgend-iter10-build -G Ninja -DCMAKE_BUILD_TYPE=Release` | Успешно |
| `cmake --build /tmp/asgend-iter10-build -j2` | Успешно: 37 compile/link шагов |
| CTest без длительного `integration` | Все 16 non-integration cases подтверждены в нескольких запусках; полный batch превысил общий лимит исполнения |
| `tests/contracts` через pytest | 30 passed, 1 skipped, 1057 subtests passed |
| `server/tests` через pytest | 37 passed |
| `node_agent/tests` через pytest | 37 passed |
| Dart relative imports и `package:` dependencies | 27 Dart files; пропавших relative imports нет; все внешние package imports объявлены |
| Flutter pubspec / GitHub Actions YAML | YAML parse OK |
| Android XML | XML parse OK |

CTest запускавшиеся дольше общего лимита были повторены отдельно: `query_plan_eval_smoke` passed (7.71 s), `memory_ooxml_smoke` passed (2.05 s). Остальные 14 non-integration tests прошли в групповых запусках. Полный integration test намеренно не включался в зачёт.

## Ограничения

- В контейнере нет Flutter/Dart SDK, поэтому новые widget tests **добавлены, но не исполнялись здесь**; `flutter pub get`, `flutter analyze`, `flutter test` и release APK build остаются непроверенными. Для этого настроен существующий GitHub Actions workflow `.github/workflows/flutter-android.yml`.
- `pubspec.lock` не создан без Flutter tooling; после успешного `flutter pub get` его нужно закрепить в исходниках.
- Нет device/emulator acceptance, встроенного QR-camera scanner, Android SAF/share-sheet runtime acceptance, Qt runtime acceptance, Docker/PostgreSQL restore drill и полного CP ↔ node-agent ↔ core E2E.

## Следующий этап

1. Получить результат workflow Flutter 3.29.3: `flutter pub get`, analyze, unit/widget tests и release APK build; исправить analyzer/test/API ошибки, если появятся, и добавить `pubspec.lock`.
2. Провести Android emulator acceptance для pairing, Viewer/Operator permissions, offline retry, logout/revoke и verified artifact export.
3. Закрыть end-to-end acceptance Control Plane ↔ node-agent ↔ core и deployment/restore drill; затем Definition of Done и версия 1.0.0.
