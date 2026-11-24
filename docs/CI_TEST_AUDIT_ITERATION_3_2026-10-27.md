# Аудит CI-тестов — итерация 3 (release gate и окончательная проверка)

Дата: 2026-10-27
База: Stage 21.
Изменения: Stage 22.

## Изменения

1. Четыре acceptance workflows стали reusable посредством `on.workflow_call`: Core/Control Plane/Node Agent/PostgreSQL/Compose, Native Android, Flutter Android и Qt.
2. `release-gate.yml` сохранил strict metadata preflight, а после успешного preflight вызывает все четыре acceptance workflows из текущего commit. Это позволяет проверять теги без копирования тестовых команд в новый workflow.
3. Добавлен `release-verification-summary` с `if: ${{ always() }}`; он требует, чтобы metadata job и каждый acceptance workflow имели результат `success`, а failed/skipped state приводит к неуспешному итоговому job.
4. Добавлены job timeouts: Native Android 35 min, Flutter 40 min, Qt 30 min. Qt CTest использует `--timeout 60`.
5. Добавлен CTest guard `release_workflow_coverage`; четыре теста проверяют структуру повторного использования и негативные случаи.

## Результат финальной проверки

- PyYAML BaseLoader: 5/5 workflows parse успешно.
- Release workflow coverage: 4/4 PASS.
- Client trigger/command guard: 88 path assertions + 9 build/test command assertions PASS.
- Client guard negative/positive suite: 5/5 PASS.
- API/OpenAPI contract suite: 36 run, 35 PASS, 1 optional skip.
- RPC inventory: 4/4 PASS; release-gate unit tests: 6/6 PASS.
- Release readiness script: READY / 0 recorded metadata blockers.
- Root Release build: PASS.
- Полный CTest: **45/45 PASS**, 245.54 seconds, exit code 0 (run as unprivileged user to match the integration test's sandbox assumptions).

## Оставшиеся ограничения

- Hosted GitHub Actions не запускались из этой среды. После push/merge нужно сохранить URL run, commit SHA и результат каждого reusable workflow.
- Android emulator instrumentation/device acceptance не входит в этот local test run.
- Положительный Landlock allow/deny набор нужно подтвердить на runner с доступным syscall; `ENOSYS` локальной среды не считается PASS этой матрицы.
- `release/1.0.0-gates.json` сохраняет historical evidence; его не переписывали задним числом. Для полного traceability остаётся CI-06: индексировать hosted run ID, commit SHA и артефакты.
- Отдельный внешний сабагент недоступен в данном инструментальном окружении. В рамках аудита выполнен отдельный adversarial pass с негативными тестами; он не выдаётся за внешний агентский запуск.

Ссылки GitHub Actions reusable workflow syntax: [Reuse workflows](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows) и [Workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax).
## Final adversarial pass

A final source-quality sweep found trailing whitespace in six newly written Markdown reports. It was removed; the subsequent whitespace check passed. The audit distinguishes local test evidence from hosted GitHub Actions/device evidence and leaves the latter open.
