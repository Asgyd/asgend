# TODO — аудит и исправление CI-тестов

Статусы: `[ ]` открыто; `[~]` в работе; `[x]` изменено и локально проверено; `[!]` реализовано, но требует hosted/device подтверждения.

## P0 — ложноположительные сигналы CI

- [x] **CI-01 / Clients:** Native Android, Flutter Android и Qt теперь собирают `agent-orchestrator` и запускают `core_rpc_contract_inventory`, `core_rpc_runtime_contract` и `core_rpc_state_transitions` через CTest. Изменения тестовых файлов продолжают запускать эти workflow по `push` и `pull_request`.
- [x] **CI-02 / Guard:** `test_client_workflow_contract_triggers.py` проверяет path filters и реально заданные build/test commands. Отдельные tests отклоняют отсутствие команды, неполный список suites, неправильную build target и команды, присутствующие только в комментариях.
- [x] **CI-03 / Release:** release-gate сохраняет strict metadata preflight, вызывает reusable acceptance workflows Core/Control Plane/Node Agent/PostgreSQL/Compose, Native Android, Flutter и Qt, а final summary job требует `success` для всех обязательных jobs.

## P1 — надёжность выполнения

- [x] **CI-05 / Timeouts:** Native Android timeout 35 min, Flutter 40 min, Qt 30 min; Qt CTest ограничен 60 секундами на тест.
- [ ] **CI-06 / Evidence freshness:** сохранять hosted run URL/ID, commit SHA и ссылки на артефакты в аудите, когда hosted runs будут доступны. Исторические evidence не переписывать задним числом.
- [ ] **CI-07 / Reporting:** добавить машинно-читаемый verification manifest с командами, exit codes, elapsed time, counts, environment, commit SHA и разделением PASS/SKIP/NOT RUN.

## P2 — остатки acceptance и supply-chain

- [ ] **CI-04 / Android instrumentation:** проверить фактическое наличие `androidTest` cases; добавить emulator job и smoke tests для rotation, process death, network errors, IME/accessibility после определения минимального acceptance scope.
- [!] **CI-08 / Landlock:** положительная матрица добавлена на Stage 19 и запускается внутри MCP test, но доказательство PASS требует hosted Linux runner с доступным `landlock_create_ruleset`. `ENOSYS`/skip не считать подтверждением матрицы.
- [ ] **CI-09 / Hosted evidence:** после push/merge проверить run IDs, результаты и артефакты каждого reusable workflow, особенно release-tag run.
- [ ] **CI-10 / Supply chain:** оценить pinning third-party actions на immutable SHA как отдельную задачу с контролируемым механизмом обновлений.

## Этапы аудита

- [x] **Stage 20 / Iteration 1:** baseline-инвентаризация пяти workflows, CTest, triggers, dependency setup, release gate; записаны findings, декомпозиция и приоритеты.
- [x] **Stage 21 / Iteration 2:** client trigger/command parity, улучшенный guard и негативные тесты.
- [x] **Stage 22 / Iteration 3:** reusable release acceptance workflows, fail-closed summary job, explicit timeouts, release workflow regression tests.
- [x] Финальный аудит, все три iteration reports, полный CTest log и итоговый архив собраны.

## Финальные локальные результаты

- [x] 5/5 workflow YAML файлов успешно разобраны PyYAML BaseLoader.
- [x] Shared API/OpenAPI tests: 36 запущено, 35 PASS, 1 optional skip.
- [x] Client trigger/command guard: 88 path assertions и 9 build/test command assertions PASS.
- [x] Client guard unit tests: 5/5 PASS.
- [x] Release workflow coverage tests: 4/4 PASS.
- [x] Core RPC inventory: 4/4 PASS.
- [x] Release gate regression tests: 6/6 PASS.
- [x] Release readiness metadata: READY / 0 recorded blockers; это статическая metadata-проверка, не hosted evidence.
- [x] Fresh Release build + full CTest as unprivileged user: **45/45 PASS**, 245.54 seconds, exit code 0.
- [!] GitHub-hosted workflow executions, Android emulator/device acceptance и hosted positive Landlock result не были получены локально; для них требуется реальный hosted/device run.
