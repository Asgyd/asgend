# ASGEND 1.0 — декомпозиция плана и todo-лист

Источник: `docs/ROADMAP.md`, `docs/REMOTE_CONTROL_PLANE.md`, `docs/MEMORY_INDEX.md`, `docs/IMPLEMENTATION_PLAN.md` (состояние 1.0.0).
Общий контракт для параллельной работы: `contracts/v1/CONTRACT_SKETCH.md`.

Легенда: `[ ]` — не начато, `[~]` — в работе, `[x]` — сделано и проверено, `[!]` — сделано, но не проверено в этой среде (нет тулчейна).

## WP1 — A0: архитектурные контракты и ADR  (агент: contracts)
- [x] A0.1 JSON Schema для Task/TaskEvent/UserInputRequest/Node/Artifact/Workspace/CommandEnvelope/AuditEvent/Error (`contracts/v1/schemas/`); проверка схем проходит
- [x] A0.1 OpenAPI 3.1 (`contracts/v1/openapi.yaml`) для всех user и node API; контрактный тест сверяет endpoint inventory и `$ref`
- [x] A0.1 Примеры submit/status/events/answer/cancel/retry/artifact (`contracts/v1/examples/`); `index.json` проверяется тестами
- [x] A0.1 Правила cursor/reconnect/gap/heartbeat/lease, версионирование и unknown fields зафиксированы в `docs/API_CONTRACT.md` и `state_machine.json`
- [x] A0.2 ADR: граница Control/Execution Plane, хранение (Postgres+volume), auth/RBAC, threat model, ротация/revoke и fail-closed (`docs/adr/0001-*` … `0003-*`)
- [x] Тесты контрактов: примеры валидны, неизвестные поля допускаются по политике схем, негативные примеры отклоняются; `python3 tests/contracts/test_contracts.py` passed

## WP2 — S1+S2: Docker Control Plane  (агент: control-plane)
- [x] Хранилище: versioned migrations и schema-version gate; 2026-10-22 live PostgreSQL 16.15: upgrade с v1 до latest, повторная миграция no-op, рестарт сервера (`test_postgres_acceptance.py` passed)
- [x] Identity/workspace/RBAC, user/device tokens (hash), pairing codes, logout/revoke (`api_user.py`/`admin.py`); security suite и PostgreSQL restart acceptance passed
- [x] Node enrollment, heartbeat, lease, revoke, token rotation; Compose drill подтверждает, что node token, выданный до backup, работает после restore
- [x] Task API + durable command/outbox delivery, idempotency, TTL → delivery_failed, lease/ack, redelivery; PostgreSQL restart/lease-expiry/redelivery acceptance passed
- [x] Event ingest: seq-дедупликация, gap detection, snapshot/resync, long-poll cursor; события переживают backup/restore в Compose drill
- [x] Artifacts: resumable chunked upload, SHA-256, download tickets, Range, quotas, retention; байты и SHA-256 сверяются после restore (Compose drill) и в `pg_dump`/`pg_restore` drill
- [x] Audit, log redaction, Prometheus metrics, rate limits, quotas; 1.0.0 operations review: нагрузочный ramp через NGINX TLS, saturation snapshot, отсутствие токенов/тикетов в логах (`docs/SECURITY_OPERATIONS.md`)
- [x] Dockerfile (UID 10001, read-only rootfs) и Compose (PostgreSQL + CP + NGINX TLS) проверены в Docker 29 / Compose 5.6: исправлены конфликт статического IP, права на file secret, `CHOWN` для NGINX, `exec` без ENTRYPOINT, утечка download-тикетов в access log
- [x] Backup/restore: `server/scripts/compose_drill.sh` — seed → backup → mutate → verify-only → restore → restart → verify; исправлены `docker cp`-права и `pg_restore -` в `restore.sh`, потоковая передача dump в `backup.sh`; drill добавлен в CI
- [x] Security regression tests (workspace isolation, viewer RBAC, idempotency key reuse, node-token revoke, checksum rejection, unreadable-secret diagnostics) passed
- [x] In-memory rate limiter с жёстким `max_keys`; решение 1.0.0 — поддерживается ровно одна реплика Control Plane на БД (масштабирование — после 1.0)

## WP3 — Execution Node agent  (агент: node-agent)
- [x] `node_agent/`: enroll, heartbeat, lease, локальная дедупликация команд (SQLite), ack — `test_enroll.py`, `test_commands.py`
- [x] Адаптер к core TCP/Unix JSON API: submit(idempotency `cp:<cmd>`)/answer/cancel/retry_task/status/events — `test_commands.py`, `test_resilience.py`
- [x] Event pump core→CP с seq, snapshot при gaps/reconnect, offline-буфер — `test_events.py`
- [x] Загрузка артефактов из workspace задачи (`artifact_list`/`workspace_read_bytes`), resumable, checksum — `test_artifacts.py`
- [x] Node-agent unit suite (`37/37`) passes. Iteration 11 adds live HTTP Control Plane + real NodeAgent with MockCore; iteration 12 adds `node_agent_c_core_e2e` using the compiled C service: submit → lease/ack → C-core status/events → real workspace artifact read/upload/download SHA-256; PostgreSQL remains open

## WP4 — Q1: Qt 6 клиент Linux  (агент: qt-client)
- [x] `clients/asgend_qt/`: собирается без предупреждений на Qt 6.4.2 (Ubuntu 24.04), Qt 6.12 (Arch) и Fedora 42; исправлены две ошибки компиляции (`FromBase64Result`, неоднозначный `qBound`)
- [x] Connection manager: профили local socket / loopback TCP / remote HTTPS, TLS validation, pairing/reconnect, credential store; fallback-файл 0600 предупреждает пользователя и не считается encrypted secure storage
- [x] Task workspace: список/state/text filter, создание, polling events, question/free-text/choice answer, cancel/retry, dead letters
- [x] Artifacts: список и загрузка через adapter, ограничение размера и SHA-256 verification перед выдачей файла
- [x] Nodes/models/agents: обзорные диалоги; model actions и revoke node capability-gated
- [x] Qt Test core tests: 8/8 passed (offscreen) на всех трёх платформах
- [x] Packaging: TGZ + DEB (установка/запуск/удаление в чистом ubuntu:24.04) и RPM (Fedora 42) проверены; CI собирает DEB и делает install smoke

## WP5 — A1: Android (Flutter)  (агент: flutter-android)
- [x] Flutter-код приведён к закреплённому SDK 3.29.3 (убраны API из более новых Flutter: `RadioGroup`, `initialValue`, `Utf8Decoder` на `Stream<Uint8List>`); `flutter analyze` — 0 issues
- [x] Android scaffold: INTERNET, cleartext traffic off, backup/device-transfer exclusions; `bootstrap_android_wrapper.sh` теперь создаёт `local.properties` для чистых checkout'ов; unsigned release APK собирается (Gradle 8.10.2, AGP 8.7.3)
- [x] Platform secure storage подключено к локальному core token; добавлена одноразовая миграция токена из legacy SharedPreferences. Remote Control Plane экран подключён: ручной pairing, ввод скопированного `asgend-pair://` payload, workspace switching, logout/revoke и role-gated действия
- [x] API/state/storage modules: reconnect/event cursor/offline cache, инъекция AppEnvironment/HTTP/exporter
- [x] Core UI сохраняет create/list/events/answer/cancel/retry/follow-up; remote UI использует `RemoteSession`/`EventFollower` для задач, событий, answer/cancel/retry и workspace nodes. Remote artifacts подключены через download tickets, ограниченный поток и SHA-256 перед экспортом; активные/исполняемые файлы требуют явного подтверждения
- [x] Flutter unit и widget tests: 20/20 passed на Flutter 3.29.3 (исправлен поиск `FilledButton.icon` в widget-тесте); signing secrets в Git не добавлялись

## WP6 — M1–M5 остатки памяти (C)  (агент: memory)
- [x] PDF text extractor подключён к M1-extract (FlateDecode через zlib, text operators, bounded 64 MiB random-access input); неполный разбор сохраняется как индексируемый `parsed_partial`, encrypted/image-only PDFs — `unsupported`; `tests/test_memory_pdf.py` passed
- [x] Office OOXML (docx/xlsx/pptx) extractor: ZIP central directory + store/deflate, DOCX/PPTX text nodes, XLSX sharedStrings/worksheet values; 64 MiB input, 16 MiB/member, 64 MiB total decoded XML, entry/count caps, CRC/local-header validation and zip-bomb test (`tests/test_memory_ooxml.py`)
- [x] Decision-model query planning (`query --plan`, opt-in; strict JSON/allowlist, bounded rewrites, validated relative-path/type/tag filters, query-only input, local fallback on provider/schema error); `tests/test_memory_query_plan.py` passed
- [x] Paired benchmark `scripts/evaluate_memory_retrieval.py --compare-plan` (Precision@K, Recall@K, MRR@K, nDCG@K, deltas) с deterministic smoke fixture. Решение 1.0.0: `query --plan` и `--rerank` помечены EXPERIMENTAL (CLI help, README, MEMORY_INDEX); автоматический retrieval агента остаётся локальным. Human-judged оценка — после 1.0
- [x] M5: `asgend-memory gc` (stale chunks/vectors/jobs/edges, сохранение scan history и событий, VACUUM опционально), `stats` (JSON-счётчики и размеры БД); `tests/test_memory_gc.py` passed
- [x] Масштабный benchmark `scripts/benchmark_memory_vectors.py` и синтетический 5,200-vector acceptance test `tests/test_memory_ann.py` passed; tuning на real-world корпусе ещё требуется

## WP7 — Core: артефакты для Execution Node (C)  (агент: core-artifacts)
- [x] `workspace_read_bytes` (base64, bounded chunk, workspace-confined, no symlink); `tests/test_core_artifacts.py` passed
- [x] `artifact_list` (regular files, size, sha256, MIME guess, limits; traversal and scan budgets covered by `tests/test_core_artifacts.py`)
- [x] Документация `docs/PROTOCOL.md` и integration/regression tests (`tests/test_core_artifacts.py`, node-agent artifact tests)

## WP8 — Интеграция и проверка  (оркестратор)
- [x] 2026-10-22: чистая Release-сборка без предупреждений; CTest 19/19, включая долгий `integration` (≈2 мин; запускать не от root — MCP sandbox сбрасывает root до UID 65534) и `node_agent_c_core_e2e`; CI получил job `core-integration`
- [x] Contracts, Control Plane, node-agent, release-gate suites; live PostgreSQL 16 acceptance и `pg_dump`/`pg_restore` drill (клиентские утилиты той же major-версии — тест теперь явно сообщает о несовпадении)
- [x] Docker Compose runtime + backup/restore/readiness drill (`server/scripts/compose_drill.sh`, CI job `compose-drill`)
- [x] Qt: сборка/тесты/пакеты/установка на Ubuntu 24.04, Fedora 42, Arch; Flutter analyze/test/APK на 3.29.3
- [x] Версии 1.0.0 во всех источниках (core, Control Plane, API, Qt; Flutter `1.0.0+2`); `release/1.0.0-gates.json` обновлён с evidence; итог — `IMPLEMENTATION-STATUS-2026-10-22-ITERATION-17.md`
- [x] Стабильный релиз утверждён мейнтейнером 2026-10-22 (`release_approved=true`, `release_channel=stable`); осталось поставить тег `v1.0.0` в репозитории — workflow `release-gate` проверит `--strict`

## После 1.0.0
- [ ] Несколько реплик Control Plane: общий rate limiter / long-poll notifier (сейчас поддерживается одна реплика)
- [ ] Human-judged оценка retrieval и query planner на реальном корпусе; снять пометку EXPERIMENTAL с `--plan`/`--rerank`
- [ ] Ручной QA remote-сценария (pairing, secure storage, export) на физическом Android-устройстве — сейчас есть только launch smoke в эмуляторе API 34; сканирование QR камерой
- [ ] Qt: импорт/экспорт настроек в UI, system tray/уведомления, полная локализация; AppImage/Flatpak/PKGBUILD
- [ ] Cross-architecture сборка (aarch64) и матрица поддерживаемых целей
- [ ] Декомпозиция legacy local UI в `clients/asgend_flutter/lib/main.dart`

## Cross-project audit continuation — 2026-10-25

See `docs/PROJECT_AUDIT_ITERATION_1_2026-10-25.md`, `docs/PROJECT_REVIEW_DECOMPOSITION_2026-10-25.md` and `docs/PROJECT_AUDIT_TODO_2026-10-25.md` for the updated evidence and cross-subsystem TODO. CORE-15 MCP startup/restart race is fixed and covered by `core_mcp_launcher_smoke`. CORE-16 SSE watchdog is now implemented and verified; `core_llm_sse_watchdog` passed 1/1 (39.31 s) and the latest full integration passed 1/1 (126.75 s, exit code 0). A single complete combined CTest invocation remains the next acceptance gate.

## Cross-project audit continuation — 2026-10-26 (Stage 13)

See `docs/PROJECT_AUDIT_FINAL_2026-10-26.md`, `docs/PROJECT_AUDIT_STAGE_13_API_CONTRACT_PARITY.md`, `docs/PROJECT_REVIEW_DECOMPOSITION_2026-10-26.md` and `docs/PROJECT_AUDIT_TODO_2026-10-26.md`. CONTRACT-01..04 add bidirectional runtime/OpenAPI parity, per-route security parity, client endpoint coverage and client-workflow execution of the shared contract suite. Current CP route inventory is 37/37 vs OpenAPI; 49 endpoint references across native Android, Flutter, Qt and Node Agent match documented paths. Remaining: hosted Actions run, Qt verb inference, and prior cross-project TODOs.

## Cross-project audit continuation — 2026-10-26 (Stage 14)

`docs/PROJECT_AUDIT_STAGE_14_CORE_RPC_PARITY.md` records the complete 33-operation local TCP/Unix JSON-line RPC inventory, exact parity against `server_handle_request()`, client operation coverage, and CTest registration. `RPC-DOC-01`, `RPC-CLIENT-01`, and `RPC-TEST-01` are marked complete; operation request/response schemas and live client acceptance remain open follow-up work.


## Cross-project audit continuation — 2026-10-26 (Stage 14)

The local Core RPC inventory is now machine-checked: all 33 dispatcher operations match `docs/PROTOCOL.md`, client scanners find only supported operations, and `core_rpc_contract_inventory` is registered in CTest. The latest Release build and unified suite passed **41/41** (227.53 seconds, exit code 0). Native Android/Flutter/Qt workflow filters include the inventory test source and trigger guard passes 76 path assertions. See `docs/PROJECT_AUDIT_STAGE_14_CORE_RPC_PARITY.md`, `docs/PROJECT_AUDIT_FINAL_2026-10-26.md`, `docs/PROJECT_REVIEW_DECOMPOSITION_2026-10-26.md`, and `docs/PROJECT_AUDIT_TODO_2026-10-26.md`.

## Cross-project audit continuation — 2026-10-27 (Stage 15)

Stage 15 extends the Stage 14 Core RPC operation inventory with live TCP request-field/type checks, representative response-shape assertions, and a contract table for all 33 operations in `docs/PROTOCOL.md`. New target: `core_rpc_runtime_contract`. The fresh Release build and full CTest passed: **42/42** (222.46 s, exit code 0); the focused RPC runtime target passed as part of the same run. See `docs/PROJECT_AUDIT_STAGE_15_CORE_RPC_SEMANTICS.md`, `docs/PROJECT_AUDIT_ITERATION_3_2026-10-27.md`, and `docs/PROJECT_AUDIT_FINAL_2026-10-27.md`. Successful state-transition fixtures and hosted CI/device acceptance remain open.

## Cross-project audit continuation — 2026-10-27 (Stage 16)

Stage 16 adds `core_rpc_state_transitions`, a focused live-RPC fixture for cancellation of an active stream, answering a real clarification request, redriving a seeded dead letter, managed-model start/stop/enable/disable, and a successful mock LSP diagnostics response. The targeted Release CTest passed **1/1** in 6.28 seconds; full CTest passed **43/43** in 228.09 seconds (exit code 0). Remaining hosted-CI/device limits are recorded in `docs/PROJECT_AUDIT_STAGE_16_CORE_RPC_STATE_TRANSITIONS.md` and the cumulative final audit.

## Cross-project audit continuation — 2026-10-27 (Stage 17)

Stage 17 fixes client CI path filters: `tests/test_core_rpc_runtime_contract.py` and `tests/test_core_rpc_state_transitions.py` now trigger native Android, Flutter Android and Qt workflows on `push` and `pull_request`. The workflow trigger guard now validates 88 path assertions (44 per event type) and passes. All five workflow YAML files parsed locally, the Core RPC inventory passed 4/4 assertions, and release readiness remains READY. Hosted Actions and physical-device acceptance are still open; see `docs/PROJECT_AUDIT_STAGE_17_CI_TRIGGER_COVERAGE.md`.


## Cross-project audit continuation — 2026-10-27 (Stage 18)

Stage 18 resolves the open Qt route-scanner item from the Stage 13 audit: `tests/contracts/test_contracts.py` now infers HTTP methods from Qt `ControlPlaneAdapter` request blocks, expands the dynamic task command route for `answer`/`cancel`/`retry`, and verifies the direct artifact-download GET. The full shared contract suite passed **36 tests, 35 PASS / 1 optional skip**. See `docs/PROJECT_AUDIT_STAGE_18_QT_METHOD_PARITY.md`. Native Qt UI execution and hosted Actions remain unverified here.


## Cross-project audit continuation — 2026-10-27 (Stage 19)

Stage 19 adds MCP sandbox parity coverage in `tests/test_core_mcp_launcher.py`: a conditional Landlock allow/deny probe, strict fail-closed when Landlock is unavailable, cleanup after a child startup/exec failure, and four concurrent MCP tool requests over a restart boundary with four Core workers. The launcher smoke test timeout is 60 seconds. Targeted Release CTest passes 1/1 in 21.49 seconds. This container reports `ENOSYS` for `landlock_create_ruleset`; the Landlock positive matrix is implemented but awaits execution on a Landlock-capable Linux runner. See `docs/PROJECT_AUDIT_STAGE_19_MCP_SANDBOX_PARITY.md`.


## Performance audit continuation — 2026-10-28 (Stage 30)

Stage 30 measures actual C `db_list_tasks()` latency against an isolated candidate with mutex wait, prepare/step, C JSON mapping and serialization timing. The profile includes 1k/10k/100k rows, high-running and four-running/large-queued distributions, concurrency 1/4, 60 paired runs and 6,000 raw samples. All ordered JSON pairs match. The candidate gains 65–70× only in the distribution where at least 200 running rows fill the API limit; at 100k with four running tasks it is 1.85–2.06× slower. Candidate partial indexes therefore remain benchmark-only. Release full CTest passed 53/53 in 255.72 s. See `docs/PERFORMANCE_AUDIT_STAGE_30_TASK_LIST_C_API_2026-10-28.md`, `docs/PERFORMANCE_TASK_LIST_C_API_BENCHMARK_2026-10-28.md` and the raw JSON. Next: age-aware queued ordering experiment remains open; Stage 31 memory scan/index profiling is now measured locally (see Stage 31 section below).


## Performance audit continuation — 2026-10-28 (Stage 31)

Stage 31 measures the real Release C `asgend-memory scan ROOT` for 1k and 10k deterministic file corpora, three content profiles and fresh-index/no-op/one-file-change scenarios: 90 raw samples, five repetitions per cell. Full 10k fresh-index medians range from 10.649 to 11.177 seconds; no-op medians are 3.009–3.344 seconds; one-file-change medians are 3.011–3.892 seconds across profiles. The independent verifier checked 33,000 fixture hashes/sizes (56,320,000 bytes), scan metadata and the binary/tool hashes. Linux page cache was not cleared, so this is not cold physical-I/O throughput, and logical MiB/s is normalized corpus bytes/wall time. The full final CTest including the timeout process-tree guard passed **54/54 in 270.30 s, exit 0**; all 54 timeouts are positive/finite and the post-run process check is clean. The Stage 31 audit bundle and updated cumulative archive were both created and checked for readability/safe paths; SHA-256 sidecars next to each archive were verified.
