# Аудит CI-тестов — итерация 2 (исполнение тестов согласовано с триггерами)

Дата: 2026-10-27
База: Stage 20 baseline archive.
Результат: изменения добавлены в Stage 21 source snapshot.

## Что исправлено

1. `.github/workflows/native-android.yml`, `flutter-android.yml` и `qt-client.yml` теперь устанавливают Linux dependencies для root Core, конфигурируют Release build `build-core-ci`, собирают `agent-orchestrator` и запускают в CTest три suites: `core_rpc_contract_inventory`, `core_rpc_runtime_contract`, `core_rpc_state_transitions`.
2. Удалён неиспользуемый одиночный вызов inventory test из client workflows; вместо него CTest исполняет inventory вместе с двумя live runtime suites. Shared API/OpenAPI contract tests сохранены.
3. `tests/test_client_workflow_contract_triggers.py` проверяет 88 обязательных path assertions для трёх workflows и наличие build/test commands (9 дополнительных command assertions).
4. Guard игнорирует закомментированные команды, чтобы comment-only строка не создавала ложноположительный результат.
5. Добавлен `tests/test_client_workflow_test_coverage.py`, включённый в root CTest. Пять тестов проверяют happy path и негативные случаи: нет команды, не все тесты, неверная цель сборки, команды только в комментариях.
6. `CMakeLists.txt` регистрирует `client_workflow_test_coverage`.

## Результаты

- Client workflow guard: PASS — 88 required path assertions и 9 build/test command assertions.
- Guard unit tests: 5/5 PASS.
- OpenAPI contracts: 36 run, 35 PASS, 1 optional skip.
- Core RPC inventory: 4/4 PASS.
- Root Release configure/build target `agent-orchestrator`: PASS.
- Targeted CTest: **5/5 PASS** — `client_workflow_contract_triggers`, `client_workflow_test_coverage`, `core_rpc_contract_inventory`, `core_rpc_runtime_contract`, `core_rpc_state_transitions`.
- Все пять workflows успешно парсятся YAML `BaseLoader`.

## Результат независимого критического прохода по изменению

Проверка специально искала обходы guard: комментарии вместо shell-команд, неполный набор тестов и сборку не той цели. После обнаружения риска comment-only строк guard был ужесточён, и добавлен соответствующий негативный тест. Это независимый по цели adversarial review pass, но не отдельный внешний subagent: в текущей среде нет инструмента создания сабагента.

## Оставшиеся пункты

- Release workflow по-прежнему проверяет только `check_release_readiness.py --strict`; Stage 22 должен выполнять тесты на release tag.
- Client jobs пока не имеют явных job-level timeouts, а Qt CTest не имеет `--timeout`; это будет исправлено в Stage 22.
- Изменённые workflows не запускались на hosted GitHub Actions; локальная валидация не заменяет hosted evidence.
