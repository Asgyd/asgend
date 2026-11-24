# Итоговый аудит CI-тестов ASGEND — 2026-10-27

## Резюме

Аудит выполнен в три последовательные итерации: исходная инвентаризация (Stage 20), исправление соответствия client triggers и test commands (Stage 21), проверка release tag и ограничений времени (Stage 22). Исходный Stage 19 архив использовался как baseline; все изменения и результаты сохраняются в отдельных iteration reports.

Основные разрывы исходного CI закрыты локально проверяемыми изменениями:

1. Три клиентских workflow ранее включали runtime/state RPC test files в path filters, но не исполняли эти тесты. Теперь каждый workflow собирает Core и запускает `core_rpc_contract_inventory`, `core_rpc_runtime_contract` и `core_rpc_state_transitions`.
2. Guard ранее проверял только trigger path filters. Теперь он проверяет build command, Core target и запуск полного списка RPC tests; добавлены положительные и негативные тесты, включая закомментированные команды.
3. Release gate ранее проверял только metadata/manifest. Теперь он вызывает reusable workflows для Core/Control Plane/Node Agent/PostgreSQL/Compose, Native Android, Flutter и Qt, а обязательный final summary job требует `success` для metadata и всех acceptance workflows.
4. Для client workflows добавлены явные job timeouts; Qt CTest получил `--timeout 60`.

## Findings и итоговый статус

| ID | Severity | Finding baseline | Итоговый статус |
|---|---|---|---|
| CI-01 | P1 | Client RPC test files triggered workflows but runtime/state tests were not run there | **Closed Stage 21** — all three RPC suites run in all three client workflows |
| CI-02 | P1 | Path guard did not check test execution commands | **Closed Stage 21** — path + command guard and five negative/positive tests |
| CI-03 | P1 | Release tag workflow did not re-run tests on the tagged commit | **Closed Stage 22** — four reusable acceptance workflows plus mandatory status fan-in |
| CI-04 | P2 | Android instrumentation/emulator coverage missing | **Open** — device acceptance remains a separate follow-up |
| CI-05 | P2 | Client jobs had no explicit timeouts; Qt CTest lacked timeout | **Closed Stage 22** — Native 35 min, Flutter 40 min, Qt 30 min; Qt CTest 60 sec per test |
| CI-06 | P2 | Current hosted run evidence not indexed by run ID and commit SHA in an audit manifest | **Open** — no hosted run was available to link from this environment |
| CI-07 | P2 | No unified machine-readable verification manifest | **Open** — CI matrix/report is human-readable; manifest generation remains follow-up |
| CI-08 | P2 | Positive Landlock matrix was not demonstrated on a runner with Landlock available | **Open / host-dependent** — local full CTest passed, but this does not prove hosted Landlock availability |
| CI-09 | P2 | Hosted GitHub Actions execution was not observed in the local session | **Open / external evidence required** |
| CI-10 | P3 | Third-party actions use major tags instead of immutable SHA pins | **Deferred** — manage separately with a scheduled dependency-update policy |

## Stage 22 acceptance evidence

- All five workflow YAML files parsed successfully with PyYAML `BaseLoader`.
- `python3 tests/test_release_workflow_coverage.py`: 4/4 PASS. Negative checks detect missing `workflow_call`, missing dependency fan-in and missing `always()` behavior.
- `python3 tests/test_client_workflow_contract_triggers.py`: 88 required path assertions and 9 build/test command assertions PASS.
- `python3 tests/test_client_workflow_test_coverage.py`: 5/5 PASS.
- `python3 tests/contracts/test_contracts.py`: 36 run, 35 PASS, 1 optional skip.
- `python3 tests/test_core_rpc_contract_inventory.py .`: 4/4 PASS.
- `python3 tests/test_release_gate.py`: 6/6 PASS.
- `python3 scripts/check_release_readiness.py --json`: READY, 0 recorded blockers (static metadata gate only).
- Release build: `cmake -S . -B build-ci-audit -DCMAKE_BUILD_TYPE=Release` and `cmake --build build-ci-audit --parallel 2` PASS.
- Full CTest, run as an unprivileged user: **45/45 PASS**, 245.54 seconds, exit code 0. The full log is `docs/CI_TEST_AUDIT_STAGE_22_CTEST_LOG_2026-10-27.txt`.

## CI coverage matrix

| Area | Workflow evidence expected | Local evidence | Hosted/device status |
|---|---|---|---|
| Core Release + CTest | Fast CTest and separate long integration job | 45/45 CTest PASS in local Release build | Hosted job not observed |
| Core RPC inventory/runtime/state | CTest selection in each client workflow | Guard and all three suites PASS | Client hosted jobs not observed |
| OpenAPI/client route contracts | Shared contract suite in CP and three client workflows | 35 PASS, 1 optional skip | Hosted job not observed |
| Control Plane / Node Agent | Python unittest suites and C-core E2E | Covered by workflow definitions; not all server dependencies present in this local environment | Hosted jobs not observed |
| PostgreSQL / backup-restore | Dedicated PostgreSQL service job and `pg_dump`/`pg_restore` drill | Not run in this local Stage 22 validation | Hosted job not observed |
| Compose deployment | Dedicated Compose drill | Not run in this local Stage 22 validation | Hosted job not observed |
| Native Android | Unit tests, APK assembly, release APK existence | Workflow wiring audited, Android SDK build not run here | Hosted/device not observed |
| Flutter Android | Analyze, Flutter tests, unsigned APK | Workflow wiring audited, Flutter SDK build not run here | Hosted/device not observed |
| Qt | Offscreen CTest, package and install smoke | Workflow wiring audited, Qt SDK build not run here | Hosted job not observed |
| Release tag | Metadata gate + reusable acceptance workflows + final mandatory status check | YAML and static release-workflow guard tests PASS | Real tag run not observed |
| Landlock | MCP probe inside Core acceptance tests | Full CTest passes; local kernel may return ENOSYS | Positive hosted matrix not observed |

## Independent review and limitations

A separate critical-review pass checked the fixes for false positives: commented-out commands, incomplete RPC test regex, missing Core build target, missing reusable workflow declaration, missing final job dependency and missing `always()` status evaluation. Those cases are covered by negative regression tests. The tool environment did not expose a separate subagent launcher, so no external subagent run or external-agent score is claimed.

The local run is not a substitute for hosted GitHub Actions. After merging, check the release workflow's run on the exact tag, verify all reusable workflow jobs succeeded, record its run URL/commit SHA, and confirm the positive Landlock path on the hosted runner. Android emulator and real-device acceptance remain open.
## Final adversarial pass

A final source-quality sweep found trailing whitespace in six newly written Markdown reports. It was removed; the subsequent whitespace check passed. The audit distinguishes local test evidence from hosted GitHub Actions/device evidence and leaves the latter open.
