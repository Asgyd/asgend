# ASGEND — продолжение реализации (20 октября 2026)

## Реализовано в версии 0.8.1: отмена активных запросов модели

- OpenAI-compatible streaming и System One JSON transport используют `CURLOPT_XFERINFOFUNCTION` и проверяют `db_task_cancelled()` во время активного HTTP-запроса. При отмене libcurl прерывает transfer, и orchestrator завершает задачу со статусом `cancelled` вместо ожидания полного `request_timeout_seconds`.
- System One больше не является исключением: ранее его POST-запросы полагались только на общий HTTP-таймаут, хотя streaming-адаптер уже умел реагировать на отмену.
- `resources_prepare_agent` передаёт ID владельца задачи в четыре независимых потока классификаторов. Каждый отдельный поток устанавливает собственный thread-local task context вокруг model call и очищает его перед возвратом; fallback-выполнение без создания потока сохраняет родительский контекст.
- Тестовый System One endpoint умеет намеренно задерживать запрос одного из параллельных resource classifiers. Новый тест отменяет задачу во время этого вызова и проверяет, что она переходит в `cancelled` до обычного HTTP-таймаута. Это отдельно проверяет передачу контекста в worker thread, а не только отмену запроса главного потока.
- Версия проекта повышена до 0.8.1; README, архитектура и roadmap синхронизированы.

## Проверки этой итерации

- Release-конфигурация и полная сборка CMake: успешно, без ошибок компиляции.
- `python3 -m py_compile tests/integration.py`: успешно.
- Целевой e2e-тест отмены активного запроса System One из параллельного resource-classifier потока: успешно.
- Полный `python3 -u tests/integration.py build-final/agent-orchestrator .`: успешно; завершённый вывод `integration tests passed`, код возврата 0. В первом CTest-прогоне интеграционный сценарий один раз не дождался кадра в PTY-навигации TUI; повторный полный integration-run этот участок прошёл и завершился успешно. CTest `ui_asset_upgrade` и `tui_library_smoke` прошли.

## Границы после версии 0.8.1 (до последующего исправления в 0.8.2)

- Отмена активных LLM HTTP-запросов прерывает transport через libcurl progress callback; точная задержка зависит от того, как часто libcurl вызывает callback, и не является hard real-time гарантией.
- В 0.8.1 синхронные MCP-вызовы проверяли отмену только на границе возврата; это ограничение устранено ниже в версии 0.8.2.
- Остальные пункты roadmap остаются: оценка token budget и семантический retrieval, TLS deployment, дополнительная изоляция недоверенного кода, multi-tenant quotas/audit/metrics и распределённая очередь.


## Продолжение: версия 0.8.2 — активная отмена MCP

- Для MCP Streamable HTTP добавлен libcurl progress callback, который проверяет `cancel_requested` активной задачи и прерывает HTTP transfer. Диагностика отличает отмену от обычного транспортного сбоя.
- Для MCP stdio отмена проверяется в циклах `poll()` при записи и чтении, с интервалом проверки до 100 мс, а также при ожидании server mutex.
- Отмена активного stdio-запроса не запускает supervisor restart сама по себе; обычные сбои транспорта по-прежнему инициируют восстановление.
- Runtime/task context передаётся в MCP-вызов через thread-local scope и восстанавливается после возврата, не разделяясь между worker threads.
- Добавлены end-to-end сценарии отмены активного Streamable HTTP запроса (тестовый timeout 8 секунд) и активного stdio-вызова (mock server пишет маркер в sandbox и задерживает ответ). Тест требует статус `cancelled` до обычного timeout.
- Версия проекта повышена до 0.8.2.

## Проверки версии 0.8.2

- Release-сборка CMake прошла успешно; `agent-orchestrator -v` выводит `agent-orchestrator-c 0.8.2`.
- Полный запуск `python3 -u tests/integration.py build-next/agent-orchestrator .` прошёл с кодом возврата `0` и финальным сообщением `integration tests passed`. Включены новые активные MCP-cancellation сценарии, а также прежние проверки SSE, очереди задач, миграций, TUI, CLI, MCP supervisor, LSP, моделей и Web UI.
- CTest `ui_asset_upgrade` и `tui_library_smoke` прошли: 2/2.
- `python3 -m py_compile tests/integration.py tests/mock_mcp_server.py` прошёл успешно.

## Продолжение: версия 0.8.3 — изоляция окружения MCP stdio

- В 0.8.2 очищенное окружение создавалось только внутри `child_sandbox_setup`; этот путь раньше обходился при `sandbox.enabled=false`. Теперь очистка окружения запускается для каждого stdio MCP child независимо от режима OS sandbox, поэтому служебные токены/API-ключи не наследуются автоматически.
- Добавлен per-server allow-list `mcp.servers[].env`: только явно заданные строковые значения передаются конкретному MCP stdio серверу. Максимум 16 пар; имена проверяются, значения ограничены размером поля, а `PATH`, `HOME`, `TMPDIR`, `LD_*` и `DYLD_*` нельзя переопределять. Для `streamable_http` параметр `env` отклоняется как неприменимый.
- Безопасная среда child включает фиксированный PATH, C locale, `PYTHONDONTWRITEBYTECODE=1`, `HOME`/`TMPDIR` внутри sandbox workspace (либо `/tmp`, когда sandbox отключён).
- Тест современного MCP-протокола теперь передаёт `ASGEND_MCP_MODERN_MOCK=1` через явный per-server allow-list; он больше не полагается на случайное наследование переменной окружения от тестового процесса.
- Добавлены тесты, которые отключают sandbox, задают секрет в родительском окружении и проверяют, что дочерний MCP его не видит, а явно настроенная переменная доступна. Отдельно проверяется отказ переопределить `PATH`.
- Версия повышена до 0.8.3; обновлены README, архитектура и roadmap.

## Проверки версии 0.8.3

- Release-сборка CMake успешна; `agent-orchestrator --version` выводит `agent-orchestrator-c 0.8.3`.
- Целевой smoke-test environment isolation прошёл: `parent_secret=null`, explicit env доступна, `PATH` фиксирован, `LD_PRELOAD` отсутствует, ожидаемые `HOME`/`TMPDIR` установлены при отключённом sandbox.
- Целевой запуск современного MCP протокола с per-server env завершился успешно.
- Полный `python3 -u tests/integration.py build-next/agent-orchestrator .` завершился с кодом `0` и финальным `integration tests passed`; в прогон входят SSE/отмена LLM, SQLite/backoff/dead-letter, TUI/CLI, MCP (включая environment isolation, modern handshake и restart), LSP, модели и Web UI.
- `ctest --test-dir build-next --output-on-failure -E integration`: `ui_asset_upgrade` и `tui_library_smoke`, 2/2 успешно. Полный интеграционный скрипт запускался отдельно и успешно завершился; один общий вызов CTest был прерван внешним лимитом команды до завершения полного integration-test.
- `python3 -m py_compile tests/integration.py tests/mock_mcp_env_probe.py tests/mock_mcp_server.py` успешно.
- SHA-256 бинарника этой сборки: `762b60fb121f42d38f9de47826b3f2b29d0e7f4ad0b08aa26474f68675dd421c`.


## Продолжение: версия 0.9.0 — C memory index M1 scan/state

- Добавлена статическая C-библиотека `asgend_memory` (`include/asgend_memory.h`, `src/memory/index.c`) и отдельный CLI `asgend-memory` с командами `scan`, `verify`, `status`, `events`.
- SQLite база создаётся в `<ROOT>/.asgend_mem/index.sqlite3`. В schema v1 есть `scan_runs`, `files`, `file_snapshots`, `file_events`, `schema_migrations`; включены WAL, foreign keys, `synchronous=FULL`, индексы и version guard.
- Рекурсивный обход использует directory file descriptors, `fstatat(AT_SYMLINK_NOFOLLOW)` и `openat(O_NOFOLLOW)`. `.asgend_mem` исключается, symlink не открываются, mount points пропускаются по `statx` mount ID, с fallback по `st_dev`.
- SHA-256 рассчитывается через OpenSSL EVP с лимитом 256 MiB по умолчанию (`--max-file-size` меняет лимит). Нормальный scan использует size + nanosecond mtime как cache hint; `verify` принудительно перечитывает файлы, включая те, у которых size/mtime совпали.
- События отражают `added`, `modified`, `deleted`, `renamed`, `restored` и файловые ошибки. Переименование сохраняет `file_id`, только если содержимое уникально сопоставляется одному непосещённому старому пути и одному новому. Если обход не завершён, scan получает статус `partial` и пропускает глобальное проставление deleted, чтобы ошибка доступа не приводила к ложным удалениям.
- `.gitignore`, `install.sh`, CMake install rules, README и спецификация `MEMORY_INDEX.md` обновлены. Эта итерация реализует только основу M1 scan/state; extraction, OKF parser, decision/generation/embedding stages, граф, BM25 и orchestrator retrieval ещё не заявляются как реализованные.

### Проверки версии 0.9.0

- `cmake -S . -B build-memory -DCMAKE_BUILD_TYPE=Debug` и сборка всех targets: успешно.
- `python3 tests/test_memory_index.py build-memory/asgend-memory`: `memory index tests passed`.
- Проверены первичное/повторное сканирование, SHA-256, уникальное переименование с сохранением file_id, modified/deleted/added events, symlink exclusion, отказ индексации через symlink `.asgend_mem`, file-size limit, `PRAGMA integrity_check` и `verify` для изменения при прежних размере и mtime.
- Full orchestrator integration: `python3 -u tests/integration.py ./build-memory/agent-orchestrator .` завершился с кодом `0` и финальным `integration tests passed` (SSE, очередь задач, TUI/CLI, MCP, LSP, managed models и Web UI).
- Финальный `ctest --test-dir build-memory --output-on-failure`: **4/4 passed**; integration 131.43 s, `ui_asset_upgrade`, `tui_library_smoke` и `memory_index_smoke` — все успешно. Запуск включает финальную `statx` mount-ID проверку для индексатора.
- `cmake --install build-memory --prefix /tmp/asgend-memory-install`: staged install passed, включая CLI, статическую библиотеку и header.
- ASan/UBSan: `build-memory-asan` собран; `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_memory_index.py ./build-memory-asan/asgend-memory` прошёл.
- GCC `-fanalyzer` для `src/memory/index.c`: без диагностик; проверка локальных Markdown-ссылок прошла.

- Final full CTest: `ctest --test-dir build-memory --output-on-failure` — 4/4 passed (total 132.99 s).


## Продолжение: версия 0.9.1 — M1-extract (текст + OKF read-only)

- Добавлена команда `asgend-memory extract ROOT [--max-file-size BYTES] [--max-text-bytes BYTES] [--retry]` и публичный C API `asgend_memory_extract()`. Извлечение отделено от `scan`, что позволяет выполнять/повторять pipeline независимо.
- SQLite schema обновлена с v1 до v2 с миграцией без потери файлов, file IDs, snapshots и событий. Добавлены `file_contents` (извлечённый текст, хеш источника, raw frontmatter и статусы) и `file_metadata` (JSON-значения по отдельным ключам с provenance/hash).
- Встроенный extractor принимает UTF-8 text/code/config formats, устанавливает лимит на размер источника (256 MiB по умолчанию) и сохраняемый текст (4 MiB по умолчанию, верхний предел настройки 64 MiB), проверяет UTF-8/NUL/control bytes, и помечает unsupported binary formats явно. PDF/image/archive не обрабатываются специализированными парсерами на этом этапе.
- Для `.md` распознаётся existing frontmatter, делимитированный `---` в начале файла и закрытый `---`/`...`. Body сохраняется отдельно; raw YAML header сохраняется без модификации. Поддерживаются OKF top-level scalar fields, flow/block scalar lists, flat maps и sequence-of-map (например `sources`). Неизвестные простые поля сохраняются; если встретилась YAML конструкция вне безопасного subset, `metadata_status='parsed_partial'`, сырой frontmatter остаётся доступен.
- Все источники открываются через directory FD и `O_NOFOLLOW`, путь/размер/mtime/SHA-256 перепроверяется до и после чтения. При расхождении с состоянием последнего `scan` extraction даёт `stale_source` без записи непроверенного текста. Источники байтово не изменяются.
- Тесты добавлены для OKF metadata, no-frontmatter Markdown, unknown-extension UTF-8 sniffing, binary/PDF, same-hash skip, re-extract after source change, UTF-8-boundary-safe truncation, symlink replacement после scan и migration v1→v2.

### Финальная проверка версии 0.9.1 (M1-extract)

- Полный `ctest --test-dir build-m2 --output-on-failure`: **4/4 passed**, 137.03 s. Включены integration, `ui_asset_upgrade`, `tui_library_smoke` и `memory_index_smoke`.
- Release сборка `build-m2-release` повторно проверена после финальных исправлений; `agent-orchestrator --version` выводит `agent-orchestrator-c 0.9.1`.
- `python3 tests/test_memory_index.py ./build-m2-release/asgend-memory`: passed.
- ASan/UBSan повторно: `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_memory_index.py ./build-m2-asan/asgend-memory`: passed, утечек/ошибок не найдено.
- Ранее прошли GCC `-fanalyzer` для `src/memory/extract.c`, staged install в `/tmp/asgend-m2-install` и проверка локальных Markdown-ссылок.
- Ограничения текущего этапа сохранены явно: YAML/OKF parser покрывает безопасное подмножество синтаксиса и оставляет сырой frontmatter; специализированное извлечение PDF/Office, модельные стадии, embeddings, граф, BM25/retrieval и интеграция с роутингом исходной модели не входят в M1-extract.


## Продолжение: версия 0.9.2 — M2 decision metadata + title/description

- Добавлена публичная API `asgend_memory_enrich()` и `asgend-memory enrich ROOT`. Используются два OpenAI-compatible Chat Completions endpoint: отдельная decision-модель и генеративная модель. URL/model IDs конфигурируются переменными окружения; API keys читаются только из env и никогда не записываются в базу.
- Schema v2→v3 добавляет `pipeline_jobs`, `model_metadata`, `descriptions`. Статусы/attempt/backoff/checkpoint сохраняются; pipeline перезапускается после сбоя и пропускает успешные результаты с теми же source SHA-256, model IDs и prompt versions. Description provenance включает decision-model profile, чтобы его смена инвалидировала зависимый title/description.
- Decision metadata нормализуются по allow-list ключей/типов и ограниченным длинам. Source content считается untrusted; generated metadata хранится отдельно от OKF `file_metadata`. Исходные документы остаются неизменными.
- Реализован bounded parallelism до 16 файловых workers, отдельное SQLite-соединение на worker, HTTP request timeout, response cap 4 MiB, запрет redirects и ограничение protocol на HTTP/HTTPS; failure получает exponential backoff, `--retry` обходит отложенный retry.
- Добавлен `tests/test_memory_pipeline.py` с локальным mock API: проверяет две стадии, concurrency, cache/idempotent rerun, invalidation при смене generation/decision модели, API-key non-persistence, partial failure/backoff/retry.

### Проверки M2

- Release CMake build `build`: успешно, без compiler warnings.
- `ui_asset_upgrade`, `tui_library_smoke`, `memory_index_smoke`, `memory_pipeline_smoke`: 4/4 passed после финальной migration v1/v2→v3 logic.
- Pipeline tests проверяют schema `user_version=3`, `PRAGMA integrity_check`, cache semantics, dependency invalidation, no source mutation, no API-key persistence and error retry.
- Подробные команды CLI и ограничения endpoint compatibility обновлены в `README.md` и `docs/MEMORY_INDEX.md`. Full orchestrator integration and sanitizer/static-analysis verification are being run for the final build; embeddings/graph/BM25/retrieval remain unimplemented.

### Итоговая проверка версии 0.9.2 (M2 decision + description pipeline)

- Release build после последних изменений, включая межпроцессную блокировку `enrich`, обновлённые version strings и проверки URL/JSON: успешно; `./build/agent-orchestrator --version` → `agent-orchestrator-c 0.9.2`.
- Полный CTest: `ctest --test-dir build --output-on-failure` — **5/5 passed**, integration **130.25 s**, общий итог **133.95 s**. Включает прежний полный integration suite и четыре быстрых теста.
- Короткий повтор на финальных исходниках: `ui_asset_upgrade`, `tui_library_smoke`, `memory_index_smoke`, `memory_pipeline_smoke` — **4/4 passed**.
- ASan/UBSan с `detect_leaks=1` повторно прошёл `tests/test_memory_index.py` и `tests/test_memory_pipeline.py` на актуальном `build-asan/asgend-memory`; утечек/ошибок не обнаружено.
- GCC `-fanalyzer` для `src/memory/pipeline.c`: без диагностик; Python regression tests прошли `py_compile`.
- Staged install `cmake --install build --prefix /tmp/asgend-092-install-final`: успешно, установлены `agent-orchestrator`, `asgend-cli`, `asgend-memory`, статические библиотеки, публичные заголовки и UI/instructions resources.
- Проверка относительных Markdown-ссылок в README, ROADMAP, MEMORY_INDEX, IMPLEMENTATION_PLAN и этом статус-файле: успешно.
- Безусловно зафиксированы ограничения: M2 пока реализует только decision metadata и title/description для OpenAI-compatible Chat Completions. Embeddings, граф, BM25/semantic retrieval, специализированный PDF/Office extraction и передача найденного контекста обратно исходному model route ещё не реализованы. Qt/Linux, Android и Docker Control Plane остаются отдельными пунктами roadmap.


## Продолжение: версия 0.9.3 — M3 embeddings, FTS5 BM25 и hybrid query

- Добавлен `src/memory/embedding.c`: C API `asgend_memory_embed()` и `asgend_memory_query()`, команды CLI `embed` и `query`.
- Embedding stage обращается к OpenAI-compatible `/v1/embeddings`, имеет ограниченный параллелизм 1..16, отдельные SQLite connections, межпроцессную блокировку `.embed.lock`, сохраняемые `pipeline_jobs`/backoff и идемпотентность по SHA-256 точного `title + newline + description` + model ID.
- SQLite schema v3→v4 добавляет `embeddings` и persistent FTS5 `memory_fts` поверх `memory_search_docs`. Триггеры синхронизируют поисковый документ при изменении source paths/content/OKF metadata/descriptions. Источники не модифицируются.
- `query` поддерживает локальный BM25 без сети и hybrid режим при настроенном embedding endpoint/model. Семантический поиск использует cosine similarity, фильтрует vectors с несоответствующим hash/current description и объединяет результаты с BM25 через Reciprocal Rank Fusion. Возвращается JSON с path/title/description/ranks/scores; query text не сохраняется.
- Безопасность: JSON vector schema/type/dimension/norm validation, предел 65 536 dimension и HTTP response 4 MiB; dimension invariant на model ID; redirects отключены; ключ в environment, remote cleartext HTTP с ключом запрещён; API key не записывается в БД.
- Version strings в CLI/server/LSP обновлены до `0.9.3`, README, implementation plan, roadmap и MEMORY_INDEX синхронизированы.

### Проверки версии 0.9.3

- Release/Debug CMake build всех targets: успешно, compiler warnings отсутствуют после разбиения SQL migration на поддерживаемые C literals.
- `tests/test_memory_index.py`: passed, включая v1→v4 migration, file IDs, FTS schema availability и `PRAGMA integrity_check`.
- `tests/test_memory_pipeline.py`: passed на schema v4.
- `tests/test_memory_embedding.py`: passed; локальный mock подтверждает три вектора, одновременные запросы, idempotent rerun без повторного вызова API, invalidation по изменению описания, semantic + lexical ranking, BM25-only query, запрет remote plain HTTP с ключом и no source mutation.
- `tests/test_ui_asset_upgrade.py` и `test-asgend-tui`: passed.
- На момент промежуточной записи оставались полный CTest, sanitizer/static-analysis, staged install и упаковка; итоговые результаты перечислены в финальном разделе ниже.

### Явные ограничения 0.9.3

- Cosine search точный линейный проход по vectors; ANN/vector database пока отсутствует.
- Не реализованы graph edges, chunk-level offsets, query classifier, развитые metadata filters и автоматическая передача найденных фрагментов в исходный model route.
- PDF/Office extractors, Qt Linux client, Android client и Docker Control Plane остаются отдельными направлениями roadmap.


### Финальная проверка версии 0.9.3 (M3 embeddings + BM25/hybrid query)

- Debug CTest `ctest --test-dir build-m3 --output-on-failure`: **6/6 passed**, включая основной integration suite (**136.41 s**) и пять коротких тестов; общий итог **141.55 s**, exit code `0`.
- Release build `build-m3-release`: успешно; `agent-orchestrator --version` → `agent-orchestrator-c 0.9.3`. Release CTest без integration: **5/5 passed**.
- Memory regression: schema/migration v1→v4, persistent FTS5 triggers, `embed` parallel workers/cache/invalidation, BM25-only query, hybrid cosine/RRF ranking, no-source-mutation and endpoint/API-key rules — passed against a local OpenAI-compatible mock.
- ASan/UBSan + LeakSanitizer (`detect_leaks=1`) на `test_memory_index.py`, `test_memory_pipeline.py` и `test_memory_embedding.py`: успешно без диагностик. GCC `-fanalyzer` для `src/memory/embedding.c`: без диагностик.
- Staged install `cmake --install build-m3-release --prefix /tmp/asgend-m3-install`: успешно; установлены `agent-orchestrator`, `asgend-cli`, `asgend-memory`, публичные headers, статические libraries и ресурсы UI/instructions.
- Проверка локальных Markdown-ссылок в README и документации: 9 файлов проверены, неразрешённых ссылок не найдено.
- Ограничения M3 остались явными: cosine vector search — линейный scan, ANN нет; file graph, query classification/filters, chunk-level retrieval и передача найденного evidence в исходный orchestrator route ещё не реализованы. PDF/Office extractors, Qt/Linux, Android и Docker Control Plane остаются roadmap items.


## Продолжение: версия 0.9.4 — M4 file graph и automatic agent retrieval

### Реализовано

- SQLite schema v4→v5 добавляет `file_edges` с source/target foreign keys, `edge_type`, `reference_text`, hash источника и индексами направления. Старые записи базы сохраняются.
- Добавлен `src/memory/graph.c`, public API `asgend_memory_graph()` и CLI `asgend-memory graph ROOT`. Graph pass транзакционно строит локальные связи из Markdown link targets, wiki links `[[...]]`, C/C++ quoted `#include` и относительных JS/TS-style imports. Внешние URL, self-links и пути за пределы корня пропускаются. Повторное построение идемпотентно, устаревшие связи удаляются.
- `query` теперь возвращает короткий актуальный source excerpt, список соседних файлов/тип связи, прямой/graph provenance и число расширенных кандидатов. Из первых 12 прямых кандидатов добавляются до 6 graph neighbors на узел; score графа намеренно ниже прямого BM25/semantic RRF, чтобы точные совпадения имели приоритет.
- `agent_execute` автоматически выполняет локальный BM25 retrieval по исходному task + assigned instruction только если в корне уже существует настоящий `.asgend_mem/index.sqlite3`. В prompt добавляется JSON evidence (path/title/description/excerpt/related/provenance) для той же выбранной agent model. Не происходит второго model call, embedding/network call или подмены маршрута. Нет индекса/совпадений/ошибка поиска — выполнение продолжается без контекста памяти.
- Принципы безопасности: symlink на `.asgend_mem` не принимается, retrieval data явно помечаются как недоверенные источники, raw files не меняются, JSON-значения экранируются сериализатором, implicit retrieval принудительно lexical-only и не записывает query в БД/журнал.
- Документация и версии server/LSP/CLI обновлены до `0.9.4`; CMake включает graph module, устанавливаемая статическая библиотека экспортирует публичный API.

### Проверки версии 0.9.4

- Clean Release build: успешно, без compiler warnings; `agent-orchestrator --version` → `agent-orchestrator-c 0.9.4`.
- Быстрые тесты до полного итогового запуска: memory index/pipeline/embedding/graph, UI asset upgrade и TUI smoke — **6/6 passed**.
- Новый graph test покрывает migration v5, Markdown/wiki/C include edges, игнорирование external URL, `excerpt`/`related`, graph neighbor expansion, provenance и удаление устаревших edges.
- Автоматическая retrieval injection подтверждена интеграционным test: уникальный маркер из индексированного `project-memory-proof.md` был найден и отмечен в system context запроса агентской модели; интеграционный тест завершился с exit code 0 (132.73 s).
- GCC `-fanalyzer` для `src/memory/graph.c`: без диагностик. ASan/UBSan + LeakSanitizer для graph end-to-end test: без диагностик.
- `cmake --install build-m4 --prefix /tmp/asgend-0.9.4-install`: успешно; установлены executable, статическая библиотека, public headers и ресурсы.
- Проверка локальных ссылок Markdown в README и docs: неразрешённых ссылок не найдено.

### Оставшиеся ограничения

- Граф перестраивается отдельной командой после `scan` + `extract`; автоматическое точечное invalidation/rebuild по изменению одного файла ещё не добавлено.
- Нет chunk-level offsets/чанков, query classification через decision model, metadata field filters или token-budget-aware evidence packing. BM25 и embedding semantic search в CLI поддерживаются; автоматический agent retrieval в целях predictability использует BM25-only.
- Cosine vector search остаётся линейным; ANN/index для крупного корпуса, PDF/Office extractors, Qt/Linux, Android и Docker Control Plane остаются будущими roadmap этапами.


### Финальная проверка версии 0.9.4 (повторный полный CTest)

- Финальная Release-сборка `build-m4`: успешно; `agent-orchestrator --version` выводит `agent-orchestrator-c 0.9.4`; в чистом build-логе нет предупреждений компилятора.
- Полный `ctest --test-dir build-m4 --output-on-failure`: **7/7 passed**, общий runtime **138.06 s**; интеграционный тест **132.05 s**, и шесть отдельных smoke-тестов для UI/TUI и memory index/pipeline/embedding/graph.
- Интеграционный тест проверяет маркер из `project-memory-proof.md` в системном контексте запроса выбранной agent-модели; тест завершился с exit code 0.
- ASan/UBSan + LeakSanitizer для graph smoke test прошли без диагностик; GCC `-fanalyzer` для `src/memory/graph.c` без диагностик.
- Staged install во временный prefix успешен; внутренние Markdown-ссылки в README и `docs/*.md` разрешаются.
- В релизный архив включены исходники, тесты и документация; исключены build-каталоги, локальные `.asgend_mem` базы, `.git`, Python-кэш и собранные бинарники.


## Продолжение: версия 0.9.5 — M5 source-aware chunks, filters и token-budget retrieval

### Реализовано

- Добавлен `src/memory/chunks.c`, публичный API `asgend_memory_chunk()` и CLI `asgend-memory chunk ROOT [--max-chunk-tokens N]`. Чанки создаются по валидному извлечённому содержимому, на UTF-8 границах, с разбиением около newline и небольшим overlap.
- SQLite schema v6 добавляет `file_chunks`, `file_chunk_state`, `memory_chunk_fts` и FTS5 синхронизацию; schema v7 добавляет `body_start_byte`/`body_start_line` для source offsets после OKF frontmatter. Extractor version bump обновляет старые записи; source files по-прежнему не меняются.
- `query` синхронизирует chunk state и использует fragment-level BM25. Результаты несут `source_start_byte`, `source_end_byte`, `source_start_line`, `source_end_line`; excerpt truncation корректирует координаты. Graph expansions проходят те же metadata/path filters.
- Добавлены фильтры `--type`, `--tag`, `--path-prefix`, `--metadata KEY=VALUE`; metadata берётся из current OKF, а если ключ отсутствует — из latest current decision-model JSON.
- Добавлены `--context-tokens` и `--chunk-tokens`. Packing оценивает tokens как консервативные UTF-8 bytes/3 плюс overhead; это оценка, а не точный tokenizer модели. Agent-path ограничен 1200 estimated tokens, остаётся local-only BM25 и передаёт source coordinates в prompt evidence.
- Триггеры обновляют chunk-level title/description/metadata text после enrich, что покрыто regression тестом.

### Проверки версии 0.9.5

- Release build: успешно; версия `agent-orchestrator-c 0.9.5`; после полной пересборки compiler warnings отсутствуют.
- Memory tests: **7/7 быстрых CTest passed**, включая `memory_chunk_query_smoke`; tests покрывают schema v1→v7, chunk idempotency, coordinates, filters, context budget, FTS refresh и source no-mutation.
- End-to-end orchestrator integration (`python3 tests/integration.py ./build-m5-release/agent-orchestrator .`): **passed**, exit code `0`; проверены task lifecycle, SSE/cancellation, MCP isolation/restart, LSP, managed models, resource classifiers и Web UI.
- Полный Release CTest `ctest --test-dir build-m5-release --output-on-failure`: **8/8 passed**, общий runtime **139.73 s**; integration — **131.66 s**, все 7 дополнительных UI/TUI/memory smoke-тестов также прошли.
- ASan/UBSan + LeakSanitizer (`detect_leaks=1`) прошли на memory index, pipeline, embedding, graph и chunk query. GCC `-fanalyzer` для `src/memory/chunks.c` и `src/memory/embedding.c`: без диагностик.
- Staged install `cmake --install build-m5-release --prefix /tmp/asgend-0.9.5-install`: успешно; установлены executables, public headers, static libraries и UI/instruction resources.
- Markdown link check: 10 файлов, 17 локальных ссылок, неразрешённых ссылок нет. ZIP integrity повторно проверена после добавления окончательного CTest результата в этот статус; итоговый SHA-256 указан рядом со ссылкой на release-архив.

### Известные ограничения

- `context_tokens_estimated` — консервативная оценка, а не tokenizer-exact budget.
- BM25 использует chunk-level FTS; embedding-векторы пока относятся к title/description документа, отдельные chunk embeddings не создаются.
- Query classification через decision model, PDF/Office extractor, Qt/Linux, Android и Docker Control Plane остаются roadmap.


## Продолжение: версия 0.9.6 — M6 chunk embeddings и query-aware retrieval

### Реализовано

- SQLite schema v8 добавляет `chunk_embeddings` с `chunk_id`/`file_id`, source SHA-256, input SHA-256, model ID, размерностью и JSON-вектором. Миграция v1–v8 сохраняет прежние file IDs, snapshots, events, pipeline outputs, document embeddings, graph edges и chunks.
- Команда `asgend-memory embed ROOT --chunks [--chunk-tokens N]` создаёт отдельные векторы для source-aware фрагментов. Используются worker pool, checkpoint/cache по source/input hash и model ID, проверка размерности, повторные попытки и существующая защита embedding endpoint/API key. Секреты не попадают в `.asgend_mem`.
- `query` объединяет BM25 по чанкам с document-level и chunk-level cosine ranking через RRF; семантическое попадание возвращает соответствующий source excerpt и координаты, а JSON указывает `semantic_source` и число `chunk_semantic_matches`.
- Добавлен детерминированный локальный классификатор intent (`dependency`, `debugging`, `configuration`, `code`, `documentation`, `general`). Он объясняет классификацию в JSON и меняет порядок/вес расширения графа для import/include либо Markdown/wiki edges. Дополнительных model/API calls для классификации нет.
- Исправлен lifecycle chunk cache: обычный query и agent retrieval без явного `--chunk-tokens` сохраняют уже выбранный `chunker_version` и существующие vectors, не перестраивают документы каждый запрос и не удаляют embeddings каскадно. Если chunks действительно отсутствуют/устарели, синхронизация сохраняет предпочтительный размер, а не принудительно применяет default.
- Agent retrieval остаётся local-only BM25 без дополнительных LLM/embedding calls; классификация добавляется к evidence. Chunk semantic embeddings применяются в explicit hybrid CLI query при настроенном embedding endpoint.
- README, roadmap, plan и memory spec синхронизированы с фактическим функционалом и ограничениями версии.

### Проверки версии 0.9.6

- Release build: успешно, compiler warnings отсутствуют; `agent-orchestrator --version` → `agent-orchestrator-c 0.9.6`.
- Полный Release CTest `ctest --test-dir build-m6 --output-on-failure`: **8/8 passed**, общее время **137.43 s**; integration — **130.61 s**, дополнительно прошли UI asset upgrade, TUI smoke, memory index, pipeline, embedding, graph и chunk query smoke.
- Самостоятельный end-to-end `tests/integration.py`: passed, exit code `0`; покрыты SSE/HTTP, cancellation, legacy SQLite migration, retry/dead letters, task lifecycle, TUI/CLI, MCP, LSP, managed models, resource classifiers и Web UI.
- Все семь коротких Release CTest прошли. ASan/UBSan + LeakSanitizer (`detect_leaks=1`) для memory index, pipeline, embedding, graph и chunk query завершились без диагностик.
- GCC `-fanalyzer` для `src/memory/embedding.c`, `src/memory/graph.c` и `src/memory/chunks.c`: без диагностик.
- Staged install `cmake --install build-m6 --prefix /tmp/asgend-0.9.6-install`: успешно. Проверены локальные Markdown-ссылки в README и документации.
- В исходный релизный ZIP не включаются локальные базы `.asgend_mem`, каталоги сборки, `.git`, Python cache или бинарные артефакты.

### Ограничения, перенесённые на следующие итерации

- Query intent классификатор — прозрачная локальная эвристика, не decision model; decision-model query classification/reranking пока не реализованы.
- Chunk semantic search выполняет точный линейный scan векторов, ANN пока нет. Evaluation качества ranking на большом реальном корпусе не проведён.
- Agent-path retrieval остаётся локальным BM25; chunk embeddings используются explicit hybrid CLI query, чтобы не добавлять скрытых сетевых вызовов в request path.
- Инкрементальное построение/invalidating file graph, специализированные PDF/Office extractors, Qt Linux client, Android app и Docker Control Plane остаются отдельными пунктами roadmap.


## Продолжение: версия 0.9.7 — constrained decision-model reranking и retrieval evaluation

### Реализовано

- В `asgend-memory query` добавлены явные опции `--rerank` и `--rerank-candidates N` (диапазон 1–20). Без `--rerank` поиск остаётся прежним и не вызывает decision/reranking API.
- Reranker получает только уже найденный ограниченный список top-K кандидатов: временные alias IDs 1..N, путь, title, description и ограниченный source excerpt. Модель не может добавлять неизвестные документы; неизвестные/повторные IDs, некорректные типы relevance и значения вне 0..4 отбрасываются. Кандидаты, не упомянутые в валидном ответе, остаются ниже возвращённых, они не удаляются.
- Запрос к OpenAI-compatible Chat Completions endpoint валидируется; URL не может содержать встроенные credentials/query/hash. Удалённый endpoint требует HTTPS даже без API key; HTTP допускается только для loopback/local test. API key читается из `ASGEND_MEMORY_RERANK_API_KEY`, не сохраняется в SQLite. Запросы и результаты не записываются в индекс/логи. Документы и query явно помечаются в промпте как недоверенные данные.
- Добавлена утилита `scripts/evaluate_memory_retrieval.py` и установленная команда `asgend-memory-evaluate`. Benchmark принимает небольшой judged JSON dataset и рассчитывает macro Precision@K, Recall@K, MRR@K и nDCG@K с per-query результатами; по умолчанию использует offline BM25, `--hybrid` включает настроенный embedding endpoint.
- Добавлены fixture dataset и регрессионные тесты: mock OpenAI-compatible reranking endpoint, валидация candidate IDs, отсутствие сетевого вызова без `--rerank`, запрет удалённого HTTP без TLS, отсутствие API key в БД, качество поиска на синтетическом корпусе и отсутствие модификации исходных документов.
- README, `docs/ROADMAP.md`, `docs/MEMORY_INDEX.md` и `docs/IMPLEMENTATION_PLAN.md` обновлены; `asgend-memory-evaluate` включён в CMake install.

### Проверки версии 0.9.7

- Release-сборка `build-097`: успешна, compiler warnings отсутствуют; `agent-orchestrator --version` выводит `agent-orchestrator-c 0.9.7`.
- Полный объединённый Release CTest: **10/10 passed**, общее runtime **162.19 s**. Длительный integration test прошёл за **148.16 s**; UI asset upgrade, TUI smoke и все семь тестов памяти/retrieval также прошли.
- Девять тестов без integration отдельно перепроверены: **9/9 passed**. Дополнительно standalone `tests/integration.py` завершился с exit code `0`.
- ASan/UBSan + LeakSanitizer (`detect_leaks=1`) прошли для `memory_rerank_smoke`, `retrieval_quality_smoke` и chunk query без диагностик.
- GCC `-fanalyzer` для `src/memory/embedding.c`: без диагностик.
- `cmake --install build-097 --prefix /tmp/asgend-0.9.7-install-final`: успешно; установлены `agent-orchestrator`, `asgend-memory`, libraries, public headers и executable `bin/asgend-memory-evaluate`. Установленный evaluator запускается и корректно выводит `--help`.
- Проверка локальных ссылок README и docs: 9 Markdown-файлов, 17 локальных ссылок, неразрешённых ссылок нет.
- Релизный ZIP содержит исходники, тесты, judged fixture и документацию; исключены build directories, `.git`, `__pycache__`, локальные `.asgend_mem` базы и бинарники.

### Оставшиеся ограничения / следующий шаг

- Reranking применяется только в explicit CLI query; agent request path по-прежнему local-only, чтобы не добавлять скрытые сетевые задержки/передачу содержимого проекта.
- Текущий query-intent classifier остаётся локальной эвристикой. Decision-model query planning до BM25/semantic retrieval ещё не реализован.
- Векторный cosine search остаётся точным линейным scan; ANN и performance benchmark на крупном реальном корпусе — следующий этап. Для поиска качества добавлен инструмент метрик, но отдельного большого production relevance corpus пока нет.
- Специализированные PDF/Office extractors, нативный Qt/Linux клиент, Android-приложение и отдельный Docker Control Plane остаются задачами roadmap.


## Продолжение: версия 0.9.8 — persistent ANN для масштабирования памяти

### Реализовано

- SQLite schema v9 добавляет `ann_signature` и индексы `(model_id, ann_signature)` для document embeddings и chunk embeddings. Миграция проверяет фактические колонки через `PRAGMA table_info`, поэтому повторный переход и база с сохранёнными новыми колонками не приводят к `duplicate column`.
- Новый `src/memory/ann.c` вычисляет детерминированную 16-битную random-hyperplane LSH signature. Для запроса создаются bucket probes на Hamming radius ≤4 (2 517 сигнатур); cosine score точно вычисляется уже на найденном подмножестве.
- `asgend-memory ann-index ROOT` заполняет недостающие signatures для старых document/chunk vectors пакетами до 512 строк и сообщает количество обработанных, некорректных и ещё не индексированных векторов.
- Новые document и chunk embeddings получают signature при сохранении. ANN включается только для выбранного `model_id`, когда найдено не менее 5 000 актуальных source-valid векторов, отсутствуют пропуски сигнатур и bucket probes возвращают не менее 32 кандидатов.
- Для небольших индексов, неполного ANN backfill, узких metadata/path filters и флага `--exact-vectors` используется прежний полный cosine scan. Query JSON сообщает режим поиска, количество текущих векторов и размер множества ANN-кандидатов.
- `scripts/benchmark_memory_vectors.py` сравнивает ANN и exact cosine на одном проекте, выдаёт median latency, speedup и Recall@K по совпадению путей. Скрипт включён в установку как `asgend-memory-benchmark-vectors`; отчёт явно включает задержку embedding endpoint в измеряемое время.
- Добавлен `tests/test_memory_ann.py`: миграция/backfill проверяются на синтетическом корпусе из 5 200 векторов, ANN сокращает candidate pool, а planted nearest neighbour остаётся первым также в exact cosine reference mode.

### Проверки версии 0.9.8

- Чистая Release-сборка `build-098`: успешно; `agent-orchestrator --version` выводит `agent-orchestrator-c 0.9.8`, compiler warnings не обнаружены.
- Полный Release CTest: **11/11 passed** на финальной сборке после централизованного обновления версии, общее время **193.89 s**; integration — **177.96 s**, оставшиеся десять UI/TUI/memory/retrieval/ANN smoke-тестов также прошли.
- Standalone `tests/integration.py` завершился с exit code `0`, включая LSP diagnostics, MCP, модельный lifecycle и Web UI. Один ранний объединённый запуск получил временной LSP timeout одновременно с тяжёлым GCC analyzer; повтор standalone и финальный CTest без параллельного анализа прошли.
- `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 python3 tests/test_memory_ann.py ./build-098-asan/asgend-memory`: успешно, `memory ANN smoke test passed`.
- Python compile check для `scripts/benchmark_memory_vectors.py` и `tests/test_memory_ann.py`: успешно. GCC `-fanalyzer` для `src/memory/ann.c` завершился без диагностик; попытка анализировать большие translation units превысила лимит отдельной команды и не считается полностью завершённой.
- `cmake --install build-098 --prefix /tmp/asgend-0.9.8-install`: успешно; `asgend-memory-benchmark-vectors --help` работает после установки.
- Проверка документации: 10 Markdown-файлов, 17 локальных ссылок, ошибок нет.
- Релизный ZIP не содержит каталоги сборки, `.git`, `.asgend_mem`, Python-кэш и собранные бинарники.

### Ограничения, оставшиеся в roadmap

- LSH16 с Hamming radius 4 — приближённый single-table ANN, а не HNSW/IVF. Порог ≥5 000 и радиус требуют тюнинга на реальном размеченном корпусе; синтетический тест подтверждает корректность маршрута/сокращение кандидатов, но не заменяет production recall benchmark.
- ANN используется в explicit hybrid CLI query. Автоматический retrieval в agent request path остаётся локальным BM25 без дополнительных сетевых вызовов.
- Query-planning через отдельную decision model, специализированные PDF/Office extractors, Qt/Linux, Android и Docker Control Plane остаются отдельными пунктами roadmap.


## Версия 0.9.9 — автоматическая синхронизация resource/project memory

В версии 0.9.9 завершено разделение ресурсного каталога и проектной памяти, а стандартный выбор ресурсов переведён с четырёх параллельных вызовов decision model на local BM25.

- `ROOT/.asgend_mem/resources.sqlite3` хранит отдельный индекс `skills`, `system_prompts`, `tools`/live MCP descriptions и `knowledge`; перед каждой подзадачей выполняется hash-sync/FTS refresh. Разрешения на инструменты по-прежнему ограничены profile allow-list.
- `ROOT/.asgend_mem/index.sqlite3` автоматически обновляется `scan` + `extract` перед агентским выполнением. Корневые каталоги ресурсов исключаются, граф пересобирается при file-state changes, полная hash-проверка выполняется периодически. Найденные excerpts автоматически передаются выбранной agent-модели как untrusted evidence.
- Настроенные через env decision+generation providers автоматически запускают возобновляемые metadata/title/description stages; настроенный embedding provider — document/chunk embeddings и hybrid query. Без этих env vars не выполняются скрытые сетевые обращения, local BM25 остаётся доступен.
- README, UI resources status, Flutter resources status, integration tests и roadmap обновлены под `selection_mode=local_bm25`. Legacy `instructions/select_*.md` остаются для совместимости, но не вызываются при нормальном выборе ресурсов.
- Release version source централизованно повышен до `0.9.9`.

### Финальная проверка автоматической памяти — версия 0.9.9

- Release-сборка актуальна; `agent-orchestrator --version` → `agent-orchestrator-c 0.9.9`.
- Полный Release CTest на финальном исходном коде: **11/11 passed**, общее время **152.76 s**; интеграция — **139.17 s**, 10 отдельных UI/TUI/memory/retrieval/ANN smoke-тестов — успешно.
- Отдельный `tests/integration.py` на финальном коде также завершился с кодом `0`.
- Основной acceptance-сценарий создаёт/обновляет файлы после старта сервера, затем подтверждает новые содержимое в следующем запросе выбранной агентской модели без перезапуска сервера и без ручных `scan`/`extract`.
- Подтверждены отдельные БД `root_dir/.asgend_mem/resources.sqlite3` и `paths.project_root/.asgend_mem/index.sqlite3`; ресурсы и исходники не дублируются, если project root не включает resource root folders.
- `formalization[].resource_selection` сообщает `selection_mode=local_bm25`, `resource_index_auto_sync=true`, `decision_model_calls=0`; интеграционный журнал не содержит вызовов `purpose=resource_classifier`. Полномочия tools остаются ограничены агентским allow-list.
- `paths.project_root` теперь настраивается отдельно от конфигурационного/resource root. Для обратной совместимости по умолчанию используется `root_dir`; путь проверяется как существующий каталог без symlink в конечном компоненте.
- Model-based metadata/title/description и document/chunk embeddings запускаются автоматически только при явно настроенных соответствующих endpoint/model env vars. Без этого scan/extract/FTS/BM25 полностью локальны и не выполняют скрытых сетевых обращений.
- `cmake --install build --prefix /tmp/asgend-0.9.9-install-final`: успешно; установочный бинарник выводит версию 0.9.9. Python compile check успешен; проверено 17 локальных Markdown-ссылок, ошибок нет.

### Эксплуатационные оговорки

- Быстрый проектный scan сравнивает size + nanosecond mtime и повторно проверяет полный SHA-256 раз в 300 секунд; изменения с сохранёнными обеими метриками могут появиться в проектной памяти с задержкой до следующего полного hash pass. Resource memory синхронизируется при подготовке каждой подзадачи и пересчитывает SHA-256 ресурсов.
- Auto enrich/embeddings не включаются без явной конфигурации моделей через переменные окружения. Для полностью семантического автоматического retrieval нужно настроить embedding endpoint/model; без него используется локальный BM25.
