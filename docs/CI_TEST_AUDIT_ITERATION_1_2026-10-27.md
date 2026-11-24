# Аудит CI-тестов — итерация 1 (baseline)

Дата: 2026-10-27
Исходный снимок: `asgend-project-audit-stage-19-mcp-sandbox-parity.tar.gz` (Stage 19).
Тип прохода: инвентаризация и проверка исходного состояния до изменений.

## Резюме

Проверены пять workflow GitHub Actions, CTest-регистрация корневого проекта, команды Python/Node Agent/Control Plane, Android/Flutter/Qt тестовые шаги, release gate и существующий guard фильтров путей.

Подтверждён главный разрыв: три клиентских workflow включают файлы `test_core_rpc_runtime_contract.py` и `test_core_rpc_state_transitions.py` в `push`/`pull_request` path filters, но сами эти workflow запускают только RPC inventory test. Текущий `client_workflow_contract_triggers` guard проверяет наличие путей, но не проверяет, что соответствующие тесты запускаются. Поэтому он не способен обнаружить этот разрыв.

Второй существенный риск: `release-gate.yml` на теге запускает только `scripts/check_release_readiness.py --strict`. Это проверяет metadata и записанные статусы evidence, но не компилирует и не тестирует код на самом release tag. `release/1.0.0-gates.json` содержит доказательства, датированные 2026-10-22, тогда как текущий снимок кода уже содержит изменения Stage 15–19. Текущая проверка статической готовности не подтверждает свежесть evidence относительно SHA тега.

## Инвентаризация

| Workflow | Запускаемые проверки | Что не следует считать доказанным этим workflow |
|---|---|---|
| `control-plane-node-e2e.yml` | Release metadata/tests, API contracts, CP и Node Agent unittest, Release build + CTest без `integration`, отдельный `integration`, PostgreSQL acceptance/backup-restore, Compose drill | Hosted результат не наблюдался локально; отдельные native Android/Flutter/Qt builds принадлежат другим workflow |
| `native-android.yml` | Shared API contracts, RPC inventory, source verifier, Gradle unit tests, debug/release assemble, проверка APK | Нет `connectedAndroidTest`/emulator instrumentation в workflow |
| `flutter-android.yml` | Shared API contracts, RPC inventory, Flutter analyze/test, unsigned release APK | Нет явного выполнения новых live Core RPC request/response и state-transition тестов в этом workflow |
| `qt-client.yml` | Shared API contracts, RPC inventory, Qt build/tests, TGZ/DEB packaging, install smoke в Ubuntu-контейнере | Нет явного выполнения новых live Core RPC request/response и state-transition тестов в этом workflow |
| `release-gate.yml` | Strict проверка версий и записанных обязательных gate статусов | Не запускает build/test на SHA тега и не перепроверяет свежесть acceptance evidence |

## Подтверждённые локальные результаты baseline

- `python3 tests/contracts/test_contracts.py`: 36 тестов, 35 пройдено, 1 optional skip.
- `python3 tests/test_client_workflow_contract_triggers.py`: PASS, 88 path assertions.
- `python3 tests/test_core_rpc_contract_inventory.py .`: 4/4 PASS.
- `python3 tests/test_release_gate.py`: 6/6 PASS.
- `python3 scripts/check_release_readiness.py --json`: READY, 0 blocker, 0 version errors.
- Все пять workflow-файлов загружены парсером YAML `BaseLoader` без ошибок.

Эти проверки выполняются локально и не доказывают, что GitHub Actions запускался на hosted runner.

## Результаты итерации 1

| ID | Приоритет | Наблюдение | План |
|---|---|---|---|
| CI-01 | P1 | Клиентские workflow триггерятся от runtime/state RPC тестов, но не запускают эти файлы | Добавить явный root-core configure/build и запуск трёх RPC CI tests в каждый из трёх client workflow |
| CI-02 | P1 | Guard проверяет только path filters; он не обнаруживает несовпадение триггера и реально исполняемых тестов | Расширить guard и негативные тесты, чтобы требовать и filter coverage, и command coverage |
| CI-03 | P1 | Release workflow доверяет статическим gate statuses, не выполняя тесты на release tag | Добавить release-tag verification jobs для core CTest и Python/contract suites; явно отделить live acceptance, где требуются сервисы |
| CI-04 | P2 | Android workflow запускает unit tests, но не emulator instrumentation | Зафиксировать как отдельный TODO; проверить наличие `androidTest` источников перед планированием emulator job |
| CI-05 | P2 | Client jobs не имеют собственных `timeout-minutes`; Qt CTest не передаёт явный timeout | Установить job timeouts и timeout для Qt CTest |
| CI-06 | P2 | В release manifest evidence датировано 2026-10-22, без привязки к tested commit SHA | Добавить к отчётности политику freshness/SHA; не объявлять исторические evidence свежим hosted результатом |
| CI-07 | P2 | Нет единого, машинно-читаемого отчёта, отделяющего PASS/SKIP/NOT RUN по workflow checks | Добавить audit matrix и точные команды/evidence в итоговый markdown; генератор manifest оценить отдельно после исправления coverage |
| CI-08 | P2 | Положительная Landlock матрица Stage 19 зависит от ядра; текущая локальная среда вернула ENOSYS | Оставить hosted runner доказательством; не подменять skip положительным тестом |

## Ограничения аудита

- GitHub Actions hosted execution, UI build на managed runner и проверки устройства Android не запускались из этой среды.
- Независимый внешний subagent launcher недоступен в текущем окружении. Рецензия далее будет отдельным adversarial review-проходом, но не будет выдаваться за реальный запуск независимого агента.
- Самооценочные заметки не публикуются; публикуются только результаты аудита, подтверждённые выводами команд или проверкой файлов.
