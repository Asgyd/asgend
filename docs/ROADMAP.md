# ASGEND Roadmap

> **Release status (2026-10-22):** all required `1.0.0` gates have passed with reproducible local evidence; the maintainer approved the stable release on 2026-10-22 ([`../release/versions.json`](../release/versions.json)). Evidence: [`RELEASE_CHECKLIST_1.0.md`](RELEASE_CHECKLIST_1.0.md), [`RELEASE_STATUS.md`](RELEASE_STATUS.md); notes: [`RELEASE_NOTES_1.0.0.md`](RELEASE_NOTES_1.0.0.md).

Этот документ — основной план крупных подсистем. Версия ядра `0.9.9` реализует scan/state, OKF extraction, decision/title/description pipeline, document/chunk embedding persistence, BM25 и hybrid query, source-aware chunks, локальный file graph, локальную query-intent классификацию, constrained decision-model reranker, retrieval evaluation и persistent random-hyperplane LSH ANN для больших корпусов, автоматическую синхронизацию отдельного resource-index и project-memory перед retrieval, а также автоматическую инъекцию актуального локального контекста в агентскую модель.

## Архитектурные инварианты

1. **Execution Plane остаётся локальным/управляемым узлом.** Существующий ASGEND Orchestrator, decision model, модели-исполнители и запуск инструментов работают на выделенном узле пользователя/оператора, а не внутри нового удалённого control-plane сервера.
2. **Сервер — только Control Plane.** Сервер хранит состояние и события, аутентифицирует пользователей и узлы, передаёт команды подключённым узлам и выдаёт артефакты. Он не декомпозирует задачи, не вызывает decision model/LLM, не запускает программы/MCP-инструменты и не предоставляет LLM рабочую среду.
3. **Единый протокол и источник истины для задач.** Desktop/mobile клиенты используют общий контракт задач, событий, вопросов, отмены, повторного запуска и артефактов. В удалённом режиме исполнителем остаётся зарегистрированный Execution Node.
4. **Сырьевые файлы не мутируются индексатором памяти.** Все производные поля, checkpoints, embedding и связи сохраняются в `.asgend_mem`; индексатор не добавляет и не обновляет YAML `metadata`/frontmatter в существующих файлах.
5. **Модуль памяти написан на C и вызывает модели через существующий API/адаптер ASGEND.** Сам модуль не включает собственный inference runtime и не превращается в самостоятельный orchestrator.
6. **Надёжность по умолчанию.** Удалённые команды имеют идентификаторы идемпотентности; долгие задания индексирования имеют сохраняемые checkpoints; загрузки артефактов проверяются checksum; операции с файлами ограничены разрешённым корнем.

## Приоритеты и зависимости

| ID | Приоритет | Инициатива | Зависимости / результат |
|---|---|---|---|
| A0 | P0 — сначала | Архитектурные контракты и ADR | Общие модели task/event/artifact, граница Control Plane / Execution Plane, схема auth и threat model |
| M1 | P1 | C-модуль памяти: сканирование, извлечение и индекс состояния | Scan/state 0.9.0; extraction/OKF read-only 0.9.1 (schema v2); schema v3 и per-stage `pipeline_jobs` добавлены в 0.9.2. PDF и DOCX/XLSX/PPTX OOXML extractors реализованы с ограничениями ввода/распаковки и regression tests; остаётся их corpus-level acceptance |
| M2 | P1 | Конвейер metadata → title/description → embeddings | В 0.9.3 реализованы decision metadata, title/description, embedding API и сохраняемые vectors; в 0.9.4 retrieval подключён к agent route. Остаются provider registry integration и tuning производительности на целевых корпусах |
| M3 | P1 | Граф и retrieval вместо поиска только через decision model | BM25 + hybrid cosine/BM25 (0.9.3), graph expansion и agent evidence (0.9.4), chunk FTS/filters/token budget (0.9.5), local query-intent classifier и chunk embeddings (0.9.6), constrained reranker и offline metrics (0.9.7), LSH16 ANN (0.9.8); `query --plan` добавлен как opt-in strict-JSON, query-only decision planner. Остаются оценка на большом human-judged корпусе, ANN quality tuning. |
| S1 | P1 | Серверный Control Plane в Docker | A0; сохраняет и передаёт удалённые команды, состояния и артефакты |
| Q1 | P2 | Нативный Qt 6 клиент Linux | A0/S1 для удалённого режима; локальный режим через Unix socket/adapter |
| A1 | P2 | Android-клиент | A0/S1; использовать существующий Flutter-проект как стартовую базу |
| S2 | P2 | Эксплуатационная готовность сервера | S1; резервное копирование, обновления схемы, наблюдаемость, восстановление |
| M4 | P1 | Автоматический lifecycle и разделение resource/project memory | Реализовано в 0.9.9: resources.sqlite3 отдельно от index.sqlite3; hash-sync/FTS/BM25 перед задачей; project scan/extract и graph invalidation автоматически; опциональный auto enrich/embed при заданных model endpoints; agent context injection без отдельных resource-classifier calls. Остались benchmark на большом живом проекте и мониторинг длительных проходов |
| M5 | P2 | Производительность и lifecycle памяти | Реализованы JSON `stats`, безопасная stale-row GC с опциональным VACUUM, синтетический 5,200-vector ANN regression и paired exact-vs-ANN benchmark; остаются ANN tuning на real-world корпусе и длительный pipeline monitoring |

Параллельно Q1 и M1 можно проектировать после A0. Для удалённого доступа с Android и Qt завершение S1 и HTTPS/auth обязательно; небезопасный raw TCP core API не должен открываться в Интернет.

---

## A0. Общие архитектурные контракты

### A0.1 Task / event / artifact contract

- Описать стабильную модель `Task`, `TaskEvent`, `UserInputRequest`, `Node`, `Artifact`, `Workspace` и `CommandEnvelope`.
- Зафиксировать task states, timestamps, attempt count, cancellation, retry и ожидание ответа пользователя.
- Использовать `task_id`, `event_id`, `command_id`, `node_id`, `artifact_id` и `idempotency_key` с уникальностью и правилами повтора.
- Определить курсор событий (`after_event_id`), reconnect/resume, обработку пропусков и heartbeat/lease узла.
- Версионировать клиентский API; неизвестные поля должны игнорироваться совместимыми клиентами, несовместимые изменения — оформляться новой версией API.

**Приёмка:** спецификация содержит схемы JSON и примеры submit/status/events/answer/cancel/retry/artifact; тесты проверяют повторную доставку команд, восстановление соединения и неизвестные дополнительные поля.

### A0.2 Граница доверия и угрозы

- Описать threat model для удалённых пользователей, скомпрометированного клиента, потерянного мобильного устройства, недоверенного Execution Node, подмены артефакта и повторной отправки команды.
- Разделить credential пользователя, credential узла и короткоживший URL/токен скачивания артефакта.
- Определить роли как минимум `owner`, `operator`, `viewer`, области доступа и аудит операций.
- Зафиксировать, какие данные допустимо сохранять в Control Plane; секреты моделей, prompt-конфигурации и рабочие исходники туда автоматически не копируются.

**Приёмка:** архитектурное решение (ADR) отвечает на угрозы, задаёт ротацию/revoke credential и описывает fail-closed поведение без действительной авторизации.

---

## M1–M4. Отдельный C-модуль памяти и retrieval

### Delivered in 0.9.9 — automatic indexing and resource/project split

- Resource directories (`skills`, `knowledge`, `tools`, `system_prompts`) have their own `.asgend_mem/resources.sqlite3` with local FTS5/BM25 and hash-based refresh before each subtask. Resource selection no longer calls a decision model four times; tools remain restricted by the selected agent's configured allow-list.
- Project files have a separate `.asgend_mem/index.sqlite3`. Agent execution automatically runs incremental scan/extract before retrieval, excludes resource directories from the project corpus, refreshes the graph when file state changes, and injects the local top-K evidence into the same selected agent model.
- Every five minutes the agent-path scanner runs full hashes as a safety net for content changes whose size/mtime were preserved. Default retrieval is local-only. If compatible decision + generation endpoint/model pairs are explicitly configured, pending metadata/title/description stages are automatically resumed; if an embedding endpoint/model is configured, document and chunk embeddings are refreshed and hybrid query is enabled. Credentials remain environment-only.
- Integration acceptance criteria: create/update/delete a resource after server startup; create/update a project document after startup; next task must use latest text without restart/manual CLI; no per-category decision-model calls; project and resource DBs remain separate; source files/frontmatter remain untouched.


Подробная спецификация, этапы реализации, схема данных и критерии готовности находятся в [MEMORY_INDEX.md](MEMORY_INDEX.md).

Краткий объём:

**Статус реализации памяти в `0.9.4`:** schema v5 добавляет `file_edges`; `graph` строит локальные связи из Markdown links/wiki links, C `#include` и относительных source imports. `query` возвращает excerpt/related edges и расширяет кандидатов связанными документами с небольшим graph score. `agent_execute` выполняет local-only BM25 query, если база памяти уже существует в корне проекта, и добавляет JSON evidence в системный контекст той же agent-модели. Не реализованы query classification, metadata filters и chunk-level offsets.

1. Рекурсивный обход полного каталога с вложениями, запись в SQLite записи **для каждого файла**, включая неизвестные/бинарные файлы с результатом извлечения `unsupported`/`binary`/`error`.
2. Стабильная идентичность файла и состояние, похожее на Git: SHA-256 содержимого, размер, время изменения, статус, предыдущий hash/snapshot; обнаружение create/update/delete/rename и инкрементальная обработка.
3. Последовательные, сохраняемые и возобновляемые проходы: decision model заполняет классификационные metadata; выбранная генеративная модель создаёт title/description; выбранная embedding-модель строит vector по description. Каждый проход параллельный с ограниченным concurrency, retries и checkpoint на файл/стадию.
4. Построение графа связей между файлами, SQLite FTS5/BM25, семантический retrieval, фильтры по полям metadata, распространение релевантности по графу и последующая квалификация кандидатов.
5. Интеграция с orchestrator: retrieval заменяет чистый поиск файлов через decision model. Собранный контекст и источники передаются обратно той модели, которой маршрутизирован исходный запрос.
6. Хранение в `<корень-каталога>/.asgend_mem/`; данные metadata остаются в БД и не записываются в исходные документы.

### Приоритетные ограничения

- `.asgend_mem` исключается из индексируемого каталога во избежание рекурсии; символьные ссылки по умолчанию не обходятся за пределы корня.
- Для `.md` поддерживается чтение уже существующего OKF/YAML frontmatter; отсутствие frontmatter не означает права модифицировать файл. OKF-схема используется при хранении/интерпретации metadata в БД, без автоматической записи блока в исходник.
- Модельный вывод валидируется по схеме, помечается происхождением (`model_id`, `prompt_version`, timestamp, confidence/status) и не затирает проверенные вручную поля без явной политики.
- Ошибка отдельного файла не останавливает весь каталог; стадии и причины ошибок доступны для диагностики и повторного запуска.

---

## S1–S2. Серверный Control Plane в Docker

Подробная спецификация: [REMOTE_CONTROL_PLANE.md](REMOTE_CONTROL_PLANE.md).

### S1.1 API удалённого управления

- Создание задачи с удалённого устройства и адресация её конкретному разрешённому Execution Node/workspace.
- Список задач и фильтрация; состояния активных и завершённых задач; поток/polling событий и запросов пользовательского ввода.
- Операции answer/cancel/retry и доставка команд на зарегистрированный онлайн-узел с подтверждениями, lease, TTL и идемпотентностью.
- Просмотр/загрузка артефактов завершённых задач, метаданные, размер, MIME type, checksum, лимиты, resumable transfer и авторизация каждого скачивания.
- Сохранение и выдача состояния между reconnect/restart: задач, событий, узлов, command outbox, artifact metadata и audit trail.

### S1.2 Явные запреты серверу

- Не включать orchestrator/decision model/LLM runtime и не запускать модельные процессы.
- Не выполнять пользовательские программы, MCP tools, shell-команды, сборку или индексацию на сервере.
- Не монтировать пользовательский workspace в серверный контейнер и не предоставлять ему рабочую среду LLM.
- Не считать серверное принятие команды фактом исполнения: фактические состояния поступают от Execution Node и сохраняются как подтверждённые события.

### S1.3 Docker и хранение

- `Dockerfile` и `docker-compose.yml` для сервиса `asgend-control-plane`; БД и объектное хранилище оформлены как отдельные сервисы/volume, без совместного контейнера с оркестратором.
- Целевой вариант хранения: PostgreSQL для управляющего состояния; файловый volume или S3-совместимое object storage для артефактов. Конкретный вариант закрепить ADR после прототипа.
- Контейнер работает без root, с read-only root filesystem где возможно, ограничениями CPU/RAM/PIDs, health/readiness endpoints и корректным shutdown.
- Secrets подаются через secret mounts/environment runtime, не коммитятся в образ и не включаются в логи.
- TLS обязателен на внешнем контуре; поддержать reverse proxy и/или прямой TLS, настройки CORS/Host/origin, rate limiting, body/artefact limits и audit.

### S1.4 Этапы реализации

1. ADR и threat model; JSON/OpenAPI контракт и схема данных.
2. Регистрация/отзыв Execution Node, heartbeat, reconnect, secure enrollment и lease.
3. Task command API, persistent outbox/inbox, ack и доставка команд подключённому узлу.
4. Event/state ingest от узла, sequence/cursor, дедупликация, snapshot и восстановление после restart.
5. Загрузка/скачивание артефактов с checksum и авторизацией; quotas.
6. Docker Compose, migration/backup/restore, metrics/logs/audit.
7. Нагрузочные и security тесты, пример production deployment с TLS.

**Критерии приёмки:** после перезапуска сервера состояния и event cursor не теряются; повтор команды не создаёт дубликат локальной задачи; offline node получает команду после reconnect или истечения TTL возвращается ясный terminal error; артефакт нельзя получить из чужой workspace; контейнер не содержит моделей, не запускает orchestrator и не имеет доступа к workspace/исполняемым файлам пользователя.

---

## Q1. Qt 6 клиент для Linux-дистрибутивов

### Progress update — 2026-10-21, iteration 8

A minimal native Qt Widgets client was added on top of the existing protocol adapters. It includes local socket/loopback TCP/remote HTTPS profiles, remote pairing, reconnect manager, task filters/submit/status/event polling, answer/cancel/retry, dead letters, artifact listing/download, and node/agent/model overview. Linux desktop/AppStream metadata, CPack TGZ/DEB/RPM configuration, a Qt Test source target, and Ubuntu CI workflow are now present. This is **in progress, not release-accepted**: Qt 6 SDK is absent from the current build environment, so CMake stopped at `find_package(Qt6)` and no C++ test or GUI runtime result is claimed. AppImage/PKGBUILD/Flatpak, UI integration coverage, settings export UI and tray/notification support remain open.

### Q1.1 Технологическая база

- Отдельный нативный клиент `clients/asgend_qt/` на C++ и Qt 6; первоначальный UI — Qt Widgets для предсказуемой работы на разных окружениях.
- UI не реализует orchestration logic. Вся логика отображения состояния работает через общий client/protocol adapter.
- Поддержать два способа подключения: локальный Unix socket/локальный core и удалённый Control Plane по TLS. Не использовать внешний raw TCP API без защищённого туннеля.
- Сохранить независимость от конкретного дистрибутива и desktop environment; операции, не требующие root, не должны требовать root.

### Q1.2 Декомпозиция функций

1. Connection manager: endpoint, проверка сертификата, token/device pairing, состояния offline/reconnecting и безопасное хранение credentials.
2. Task workspace: создание задачи, список/фильтры, состояние выполнения, журнал событий в реальном времени, запрос уточнения и отправка ответа.
3. Actions: отмена, повтор failed-задачи, просмотр dead letters и деталей ошибок.
4. Artifacts: список, download, локальное сохранение, checksum и лимит размера; ошибки доступа должны быть видимыми.
5. Nodes/models/agents: обзор доступных узлов и состояний; операции управления только в рамках разрешений текущего пользователя.
6. Настройки: рабочие каталоги/endpoint, reconnect, логирование без утечки токенов, импорт/экспорт настроек без credentials.
7. UX: системный tray/уведомления, keyboard navigation, high-DPI, локализация и доступность. Tray — дополнительная возможность, не обязательная для headless WM.

### Q1.3 Упаковка и тестирование

- Сборка Release в CI на актуальных окружениях Debian/Ubuntu, Fedora и Arch Linux.
- Форматы поэтапно: AppImage для первого переносимого дистрибутива; `.deb`, `.rpm` и Arch PKGBUILD; Flatpak после проверки sandbox/perms и socket integration.
- Проверка отсутствия сетевых секретов в логах, безопасного TLS, reconnect и миграции настроек; integration tests с mock server.
- Smoke test запуска UI на X11 и Wayland, разных DPI; package install/uninstall test.

**Приёмка Q1-MVP:** пользователь может подключиться локально и удалённо, создать задачу, получать живые состояния/вопросы, ответить, отменить/повторить задачу и скачать артефакт. На CI собираются все поддерживаемые пакеты выбранного релиза; документация перечисляет явно поддерживаемые дистрибутивы и версии.

---

## A1. Android-клиент

В `clients/asgend_flutter/` уже присутствует общий Flutter source tree, однако исходники не считаются готовым Android-релизом до реальной сборки, проверки на устройстве и выпуска signing-ready артефактов. Поэтому первым шагом является аудит/доведение этого клиента, а не дублирование всей UI-логики в отдельном стеке.

### A1.1 Доставка и подключение

1. Проверить архитектуру проекта, обновить Flutter dependencies, восстановить Android platform scaffold и обеспечить reproducible build.
2. Подключение через Control Plane HTTPS: pairing/QR или ручной endpoint, проверка TLS, короткожившие credentials и безопасное хранение токена Android Keystore/secure storage.
3. Переход между Wi-Fi/мобильной сетью, reconnect/resume событий с cursor; понятные состояния offline, node disconnected, permission denied.
4. Задачи: создание, list/filter, детализация событий, ожидание ответа пользователя, answer/cancel/retry и статусы.
5. Артефакты: безопасное скачивание, проверка checksum, Android Storage Access Framework/Share, подтверждение перед открытием потенциально исполняемых файлов.
6. Node/model/agent screens показывают только разрешённые сервером команды; для долгих операций корректно восстанавливаться после остановки процесса системой.
7. Локальный кеш последнего известного состояния с отметкой времени и визуальным указанием, когда это устаревшие данные; офлайн-операции должны иметь определённую политику повторов/idempotency.

### A1.2 Безопасность и release

- Не подключаться с телефона к `127.0.0.1` хоста и не открывать незашифрованный TCP API в Интернет.
- Не записывать токены в лог/аналитику, не включать секреты в экспорт настроек; logout должен удалять сохранённые credentials и отзывать refresh/device session, если это предусмотрено сервером.
- Проверить deep links, Android permissions, cleartext traffic disabled, backup-exclusion credentials, сетевые ошибки и истёкшую сессию.
- Сначала подписанный тестовый APK, затем AAB для канала релиза; signing keys не хранить в Git/CI logs. Push notifications — отдельный optional stage после надёжной серверной модели событий.
- Решение о нативном Kotlin/Compose принимается только если аудит докажет функциональный блокер Flutter; не поддерживать две независимые Android реализации без ADR.

**Приёмка A1-MVP:** воспроизводимая сборка APK/AAB; тесты установки/обновления/отзыва авторизации; на физическом устройстве пользователь создаёт задачу, отслеживает события, отвечает на вопрос, отменяет задачу и получает артефакт через HTTPS; соединение восстанавливается после перехода в фон и смены сети.

---

## S2. Операционная готовность Control Plane

- Миграции БД с откатом/совместимостью и проверкой версии схемы.
- Резервное копирование БД и артефактов, документированное восстановление и проверка контрольных сумм.
- Metrics: online nodes, command delivery latency, event ingest delay, artifact bytes, failed auth, storage/quota usage; не экспортировать prompt, секреты или содержимое файлов в labels.
- Audit для login/pair/revoke, task submit/cancel/retry, artifact access, role/permission changes и администрирования.
- Quotas: задачи, параллельные сессии, объём/время хранения артефактов, частота API; rate limits для auth и дорогих запросов.
- Проверка горизонтального масштабирования. До реализации согласованной distributed queue не запускать несколько активных экземпляров, которые принимают одни и те же node-команды без общей lease/idempotency модели.

**Приёмка:** документированный restore drill, измеримые health/metrics, security regression suite и сценарий upgrade с предыдущей схемы данных.

---

## Сквозной Definition of Done

- Требование имеет отдельные unit/integration/acceptance tests и отрицательные security tests.
- Ошибки, timeout, retry, отмена и восстановление после restart описаны явно.
- Логи/метрики не содержат credential, raw prompts или секретные части файлов по умолчанию.
- Схемы API/БД версионированы, миграции протестированы на предыдущем schema version.
- Документация различает реализованное, проверенное в CI и только запланированное.
- Для релиза предоставлены воспроизводимые инструкции сборки, проверки и восстановления.


## Delivered in 0.9.6 — semantic chunks and query-aware graph ranking

- Schema v8 adds `chunk_embeddings` with source/input hashes, model ID, dimension checks and cascading cleanup when chunks are rebuilt or deleted.
- `asgend-memory embed ROOT --chunks [--chunk-tokens N]` optionally embeds each source-aware chunk using title + description + chunk text. Existing document embeddings remain the default behavior; successful chunk vectors are cached and repeated runs skip current inputs.
- `query` merges document-level and chunk-level semantic candidates with BM25. Semantic chunk hits return their matched source excerpt and exact byte/line coordinates. The query path validates source and embedding input hashes before using a vector.
- If `--chunk-tokens` is omitted, query now preserves the existing per-project chunk size and rebuilds only when current files have stale/missing chunk state. This prevents the agent's automatic retrieval from invalidating vectors generated with a non-default chunk size. An explicit `--chunk-tokens` still requests that size.
- Added deterministic local query intent classes (`dependency`, `debugging`, `configuration`, `code`, `documentation`, `general`) with a transparent reason in the JSON response. The classifier prefers import/include or Markdown/wiki edges as appropriate and varies the graph-only ranking weight. It makes no external request; decision-model classification remains optional future work.
- Agent evidence now carries query classification and semantic/graph ranks. Agent-path retrieval remains lexical-only, local, and instruction-safe; chunk embeddings are used by explicit hybrid CLI queries when an embedding endpoint is configured.
- Regression tests cover schema migration to v8, cache skip, chunk semantic retrieval, query classification, existing document-level hybrid behavior and source immutability.


## Delivered in 0.9.7 — constrained reranking and retrieval evaluation

- `asgend-memory query ... --rerank [--rerank-candidates N]` optionally submits only the first 1..20 pre-retrieved candidates to an OpenAI-compatible decision/reranking endpoint. Reranking is opt-in; ordinary CLI and agent-path retrieval remain local-only unless explicitly requested.
- The model receives ephemeral candidate aliases (IDs 1..N), paths, title, description and bounded excerpts. The response must be strict JSON. Unknown IDs, duplicate IDs, non-integer relevance values and relevance scores outside 0..4 are ignored. Unranked candidates remain available below valid reranked candidates; the reranker cannot introduce arbitrary paths/files. Retrieved text is labelled as untrusted data in the system prompt.
- Remote reranking requires HTTPS even when the endpoint has no API key; plain HTTP is allowed only for loopback test/local servers. Keys are read from `ASGEND_MEMORY_RERANK_API_KEY`, never persisted in SQLite. Query text and candidate contents are not written to the index or query logs.
- Added `scripts/evaluate_memory_retrieval.py` and a v1 judged-query JSON schema. The tool reports macro Precision@K, Recall@K, MRR@K and binary nDCG@K plus per-query rankings. Offline lexical BM25 is the default for reproducibility; `--hybrid` explicitly enables a configured embedding endpoint.
- Regression tests use a local OpenAI-compatible mock, confirm reranking order, reject unknown candidate IDs, ensure credentials do not enter the database, reject remote cleartext and run a small judged corpus benchmark.
- Remaining: ANN index/performance evaluation on large corpora; broader human-judged retrieval dataset for hybrid and planned search; Qt/Linux client; Android client; Docker Control Plane runtime acceptance. PDF and DOCX/XLSX/PPTX OOXML text extraction are implemented and covered by smoke tests; corpus-level and production integration acceptance remain separate.


## Delivered in 0.9.8 — persistent ANN index and scale benchmark

- SQLite schema v9 adds `ann_signature` to `embeddings` and `chunk_embeddings` and indexes `(model_id, ann_signature)`. Migration checks existing columns before adding them, so partially upgraded/legacy databases do not fail on duplicate-column migrations. File IDs and previous vectors are preserved.
- Vector signatures use deterministic 16-bit random-hyperplane LSH. The query probes all buckets within Hamming distance 4 (2,517 signatures), retaining approximate candidates with cosine scoring and existing document/chunk fusion. Candidate probes run through the persistent SQLite index rather than calculating signatures for every vector at query time.
- New document and chunk embeddings store their signatures in the same transaction as vector persistence. `asgend-memory ann-index ROOT` backfills signature fields for older vectors in bounded 512-row transactions and reports invalid legacy vectors without looping forever.
- ANN activates automatically only for a model table with at least 5,000 current, source-valid vectors, no unindexed signatures and at least 32 probe candidates. Smaller corpora or incomplete ANN data use exact cosine. Queries with metadata filters also use exact cosine so approximate filtering does not silently lower recall. `--exact-vectors` provides an explicit reference path.
- JSON response exposes `vector_search`, activation flags, vector counts and ANN candidate counts. `scripts/benchmark_memory_vectors.py` runs paired exact/ANN queries and reports median wall time, speedup and Recall@K overlap. The benchmark accounts for embedding-endpoint latency and documents that caveat.
- `tests/test_memory_ann.py` seeds 5,200 128-dimensional vectors, validates backfill/migration, verifies candidate reduction and confirms that both ANN and exact search return the planted nearest document. The full existing suite keeps its tests for schema migration, source immutability, model pipeline, FTS, reranking and orchestrator integration.

### Current ANN limits

- This is a single-table random-hyperplane LSH index, not HNSW/IVF. Radius 4 trades recall against candidate count; use the paired benchmark and a judged evaluation set before tuning it for a production corpus.
- ANN is only used in explicit hybrid CLI queries. Automatic agent retrieval remains local BM25 to avoid hidden model/network calls.
- The scale fixture is synthetic; production performance/recall must be measured on the target corpus and embedding model.


## M5 lifecycle acceptance (2026-10-21)

`asgend-memory stats ROOT` now returns JSON operational counters, schema and DB/WAL/SHM bytes, current/stale chunks and vectors, job stage/state breakdown, and last scan data. `asgend-memory gc ROOT [--vacuum]` transactionally drops stale derived rows and FTS postings while retaining file tombstones, scan history, snapshots, file events, and description history. `tests/test_memory_gc.py` covers stale chunk/document-vector cleanup, tombstone/history retention and SQLite integrity. ANN scale test (`tests/test_memory_ann.py`, 5,200 synthetic vectors) also passes; tuning on a representative real-world corpus remains open.

### Query-plan quality evaluation (iteration 7)

The retrieval evaluator supports `--plan` and paired `--compare-plan` modes, reporting Precision@K, Recall@K, MRR@K, nDCG@K and planned-minus-baseline deltas. A deterministic mock-based smoke test validates the measurement path. This is not a substitute for an independent human-judged dataset or a live-provider quality review; those remain acceptance tasks.
