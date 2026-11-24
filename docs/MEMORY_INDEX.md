# ASGEND Memory Index — спецификация модуля на C

Статус: **в 0.9.9 реализованы scan/state, UTF-8/OKF extraction, PDF и Office OOXML text extractors, decision/title/description pipeline, document/chunk embeddings, FTS5 BM25/hybrid query, source-aware chunks, локальный граф, local intent, opt-in reranker/query planner, retrieval benchmark, persistent LSH ANN, stats/gc и lifecycle resource/project memory**. Для query planner и ANN остаются human-judged relevance review и real-world tuning; клиентское production-развёртывание ASGEND 1.0 остаётся открытым. План находится в [ROADMAP.md](ROADMAP.md).

## 1. Назначение и границы

`asgend_memory` — отдельный C-модуль (library/API) и CLI для индексации, структурирования и агрегации файлов в проектную память. Он предоставляет orchestrator API для поиска релевантных файлов и фрагментов и заменяет поиск, при котором decision model самостоятельно угадывает, какие файлы нужно просмотреть.

Модуль не является LLM runtime и не исполняет оркестрацию задач. Все вызовы decision model, выбранной генеративной модели и embedding-модели проходят через явный адаптер к существующему ASGEND model/provider API. Модуль должен работать без подключённой модели в режимах `scan`, `hash`, `extract` и `status`; модельные stages ожидают доступность соответствующего provider и возобновляются позже.

## 2. Файлы и каталог хранения

Для заданного каталога источников `ROOT` база создаётся по пути:

```text
ROOT/
  ... исходные проектные файлы ...
  .asgend_mem/
    index.sqlite3
    index.sqlite3-wal
    index.sqlite3-shm
    snapshots/
    logs/
```

- `.asgend_mem/` автоматически исключается из сканирования.
- Не изменять, не переименовывать и не перезаписывать исходные файлы. Не создавать/не обновлять блоки YAML `metadata` или frontmatter в существующих документах.
- Для `.md` читать существующий OKF/Open Knowledge Format frontmatter, если он имеется; извлечённые поля сохраняются в БД. Для документов без frontmatter metadata остаётся только в БД.
- Внутреннюю модель полей проектировать совместимой с OKF; каноническую спецификацию и версию фиксировать при реализации: [Open Knowledge Format, SPEC.md](https://github.com/GoogleCloudPlatform/open-knowledge-format/blob/main/okf/SPEC.md). Совместимость формата не означает изменение исходных Markdown.
- По умолчанию не следовать symlink за пределы `ROOT`; избегать циклов и обхода mount points без явной опции. Корень канонизируется; пути в БД относительные.

## 2.1 Реализованная часть M1 scan/state в версии 0.9.0

Собираемые цели:

- `asgend_memory` — статическая C-библиотека (`include/asgend_memory.h`, `src/memory/index.c`).
- `asgend-memory` — CLI (`clients/asgend-memory.c`).
- По умолчанию база хранится в `<ROOT>/.asgend_mem/index.sqlite3`; каталог имеет режим `0700`, основной файл БД — `0600`.
- В 0.9.0 база начиналась со schema v1: `scan_runs`, `files`, `file_snapshots`, `file_events`, `schema_migrations`; включены foreign keys, WAL, `synchronous=FULL`, busy timeout и `PRAGMA user_version`. Версии 0.9.1–0.9.6 последовательно мигрируют схему до v8; миграции сохраняют `file_id`, snapshots и events. v3 добавляет pipeline jobs/model outputs, v4 document embeddings + BM25, v5 file graph, v6/v7 source-aware chunks/offsets, v8 — `chunk_embeddings`.
- Скрипт сканирует обычные файлы рекурсивно через directory fd + `openat`/`O_NOFOLLOW`, не следует symlink, не обходит `.asgend_mem` и пропускает mount points по Linux `statx` mount ID, с fallback по `st_dev`.
- SHA-256 вычисляется через OpenSSL EVP. Обычный scan может переиспользовать digest только при неизменных размере и nanosecond mtime; `verify` принудительно перечитывает содержимое. Для одного файла есть default size cap 256 MiB (`--max-file-size BYTES`).
- Сохраняются события `added`, `modified`, `deleted`, `renamed`, `restored`, `read_error`, `too_large`. Rename сохраняет `file_id` только если hash однозначно сопоставляется одному ранее не увиденному пути и одному новому пути, что уменьшает ложные переименования одинаковых дубликатов.
- Если обход каталога неполон из-за ошибки доступа/чтения, отсутствующие в текущем проходе файлы не помечаются удалёнными. Ошибка одного файла сохраняется отдельно и не обрывает весь scan.

CLI:

```bash
asgend-memory scan ROOT [--verify] [--max-file-size BYTES]
asgend-memory verify ROOT [--max-file-size BYTES]
asgend-memory extract ROOT [--max-file-size BYTES] [--max-text-bytes BYTES] [--retry]
asgend-memory enrich ROOT [--workers N] [--timeout-seconds N] [--max-input-chars N] [--retry]
asgend-memory embed ROOT [--workers N] [--timeout-seconds N] [--retry]
asgend-memory query ROOT QUERY [--limit N] [--timeout-seconds N] [--lexical-only] [--plan] [--rerank [--rerank-candidates N]]
asgend-memory graph ROOT
asgend-memory status ROOT
asgend-memory stats ROOT
asgend-memory gc ROOT [--vacuum]
asgend-memory events ROOT [LIMIT]
```

`scan` без `--verify` использует metadata fast path, поэтому изменения содержимого, при которых одновременно сохранились размер и mtime, гарантированно обнаруживаются только при `verify`. CLI не запускает модели. Текст извлекается отдельной командой `extract`; все metadata и snapshots хранятся только в БД, исходные файлы, включая Markdown, не изменяются.

## 2.2 M1-extract в версии 0.9.1

- `asgend-memory extract ROOT [--max-file-size BYTES] [--max-text-bytes BYTES] [--retry]` выполняется отдельно от `scan`, так что индексирование состояния и извлечение можно повторять и диагностировать независимо.
- Встроенный extractor поддерживает UTF-8 текст, Markdown, исходный код, типовые текстовые конфигурации и PDF. По умолчанию сохраняет не более 4 MiB извлечённого текста на файл; жёсткий предел параметра `--max-text-bytes` — 64 MiB, максимальный размер источника по умолчанию — 256 MiB.
- Для Markdown читает YAML frontmatter только если блок находится в начале файла и закрыт строкой `---` или `...`. `content_text` содержит тело без frontmatter, `frontmatter_raw` сохраняет исходный блок, а поддерживаемые верхнеуровневые scalar/flow/block list/map поля записываются в `file_metadata` как JSON-значения. Сложные/неподдерживаемые YAML конструкции сохраняются в `frontmatter_raw`; метаданные помечаются `parsed_partial`, а не объявляются полностью разобранными.
- Существующие `type`, `title`, `description`, `resource`, `tags`, `sources` и неизвестные простые OKF поля читаются без записи обратно в `.md`. Если frontmatter отсутствует, `metadata_status='none'`; malformed/truncated header помечается отдельно, а сам текст остаётся доступным для чтения.
- Таблица `file_contents` хранит исходный `source_sha256`, извлечённое тело, raw frontmatter, extractor/status, длины и ошибку. `file_metadata` содержит отдельную строку на ключ с JSON value, value type, source и hash происхождения. На каждый файл extraction сохраняется транзакционно; повторный проход пропускает тот же hash и успешный/terminal result, а stale/read errors можно повторить через `--retry`.
- Извлечение заново открывает путь через `openat`/`O_NOFOLLOW`, проверяет regular-file, размер, mtime и SHA-256. Если источник изменился между `scan` и `extract`, содержимое не индексируется до повторного `scan`. Нулевые байты, некорректный UTF-8 и недопустимые binary-control bytes не попадают в текстовый extractor. PDF разбирается встроенным extractor через bounded random-access input (максимум 64 MiB) и zlib для FlateDecode; зашифрованные, image-only и нечитаемые PDF отмечаются `unsupported`, неполно разобранный документ получает `extract_status='parsed_partial'` и остаётся доступным retrieval. DOCX/PPTX извлекают текст из Word/PowerPoint text nodes, XLSX — из sharedStrings и ячеек листов. OOXML разбирается in-memory без записи ZIP members на диск; проверяются ZIP central/local headers, CRC32 и размеры. Лимиты: 64 MiB для входного архива, максимум 4096 entries, 16 MiB на XML member и 64 MiB на суммарно декодированный XML. DTD/custom XML entities не разворачиваются, ZIP64/multi-disk/encrypted package parts отклоняются или маркируются как частичный разбор; превышение лимитов не приводит к неограниченной распаковке. Изображения и произвольные архивы остаются `unsupported`; для неизвестного расширения валидный UTF-8 текст может быть извлечён по содержимому.
- Миграция schema v1→v2 добавляет `file_contents` и `file_metadata`, не удаляя существующие записи и историю scan.

**Проверки M1-extract:** OKF scalar/tag sequence/`sources` list of mappings, Markdown без frontmatter, unknown-extension UTF-8 sniff, binary status, bounded UTF-8-safe truncation, no-source-mutation, hash-based skip/re-extract, symlink replacement race и v1→v2 migration входят в `memory_index_smoke`. `tests/test_memory_pdf.py` дополнительно проверяет валидный PDF, fallback с `parsed_partial`, encrypted PDF, SHA-256 и сохранение исходных байтов. `tests/test_memory_ooxml.py` проверяет DOCX/PPTX текст, XLSX sharedStrings/inline/numeric cells, malformed package, повторный запуск, сохранение исходных архивов и ограничение большой XML member.

## 3. Архитектура модуля

Предлагаемая структура:

```text
include/asgend_memory.h      публичный C API
src/memory/scan.c            обход дерева и обнаружение изменений
src/memory/extract.c         безопасное извлечение текст/метаданных из типов файлов
src/memory/db.c              схема, миграции, транзакции, checkpoints
src/memory/hash.c            SHA-256 и snapshot state
src/memory/pipeline.c        очередь стадий и ограниченная параллельность
src/memory/model_adapter.c   вызовы decision/generation/embedding API
src/memory/graph.c           обнаружение ссылок и file edges
src/memory/search.c          FTS5/BM25, metadata filters, graph expansion
src/memory/retrieval.c       объединение candidate scores и передача контекста
clients/asgend-memory.c      CLI для scan/status/retry/reindex/query
```

Это целевая декомпозиция, не описание уже существующих файлов. Точный layout можно изменить в ходе реализации, если публичные интерфейсы и требования сохраняются.

## 4. Модель состояния и схема БД

Минимальные логические таблицы:

- `files`: `file_id`, `relative_path`, `extension`, `mime/type`, `size`, `mtime_ns`, `sha256`, `status`, `last_seen_scan_id`, `created_at`, `updated_at`, `deleted_at`, `error_text`.
- `file_contents` (schema v2): `file_id`, `source_sha256`, `content_text`, `frontmatter_raw`, `extractor_id`, `extract_status`, `metadata_status`, `source_bytes`, `extracted_bytes`, `extracted_chars`, `error_text`, `extracted_at`.
- `file_metadata` (schema v2): `(file_id, metadata_key)` primary key, `value_json`, `value_type`, `source`, `source_sha256`, `updated_at`. In the future model-generated values need separate provenance/stage states; do not overwrite user/OKF fields by implicit model policy.
- `file_snapshots`: история hash/state для обнаружения create/update/delete/rename и сравнения с предыдущим успешным индексированием.
- `file_contents` или `file_chunks`: нормализованный извлечённый текст/чанки с ограничением размера, порядком и offsets; большие бинарные данные в БД не копировать.
- `file_metadata`: типизированные поля и массивы тегов/keywords; происхождение, confidence, `model_id`, `prompt_version`, время генерации и статус проверки. Допускается нормализованная схема или JSON с индексируемыми часто используемыми полями.
- `pipeline_jobs`: file, stage, state, attempt, `next_attempt_at`, model/profile, error, checkpoint, updated timestamp.
- `descriptions`: title, description, source hash, model id/version, prompt version, status.
- `embeddings`: file/chunk ID, description hash, embedding model ID/version, dimension, vector, timestamp. Вектор недействителен после изменения description/hash или модели/размерности.
- `file_edges`: source file, target file, relation type, provenance, confidence, discovered-at, source hash.
- `scan_runs`, `schema_migrations`, `query_logs` (без хранения приватного query текста по умолчанию) и `index_stats`.
- SQLite `FTS5` virtual table для BM25 по именам, путям, title, description, extracted text, tags/keywords и выбранным metadata полям.

Все изменения проходят через транзакции. Включить foreign keys, WAL и миграции схемы; сохранить способность проверять/восстанавливать DB после некорректного shutdown.

## 5. Полный проход индексации

### M1 — рекурсивный scan и состояние файлов

1. Принимать канонический `ROOT`, exclusions/size limits и опциональные типовые extractor rules.
2. Обходить всё дерево рекурсивно и создавать запись для каждого обычного файла, включая файлы неизвестного расширения. Ошибка чтения одного файла записывается в status и не прерывает scan.
3. Вычислять SHA-256; `mtime` и `size` используются как быстрые hints, но решение о содержимом опирается на hash. В нормальном режиме не перечитывать файл целиком, если metadata неизменна и policy допускает оптимизацию; периодическая verify mode перепроверяет hash.
4. Сравнивать текущий scan с последним завершённым: новый файл, изменённый файл, исчезнувший файл, rename candidate (тот же hash, новый путь). Сохранять состояние и историю snapshots.
5. Извлекать текст из поддерживаемых типов через отдельные безопасные extractors; бинарные/неподдерживаемые файлы остаются в каталоге с описанием типа и ошибкой/статусом extractor, а не тихо пропускаются.
6. Не запускать исполняемые файлы из источников. Ограничить CPU, RAM, размер файла, длину извлечённого текста и runtime конкретного extractor.
7. Предоставить `scan`, `verify`, `status`, `retry-failed`, `reindex` CLI/API операции.

### M2 — три model stages

Три стадии логически разделены и независимо возобновляются; порядок соблюдается для каждого файла, но разные файлы могут находиться на разных стадиях параллельно после завершения необходимой зависимости.

**Stage A: Decision model → metadata**

- Вход: extracted text/chunks, путь/тип файла и уже существующие OKF metadata (read-only).
- Выход: структурированные категории/type, tags/keywords, language, content kind, entities/references, confidence и опциональные поля домена. Точная JSON schema versioned.
- Decision model не должна писать metadata в исходник и не должна выполнять произвольные shell/tools в этом pipeline.

**Stage B: выбранная генеративная модель → title и description**

- Вход: содержимое (для длинных файлов — выборка/чанки с явной политикой coverage) и результат metadata stage.
- Выход: короткий title и фактологическое description с указанием модели/prompt version; не выдумывать детали, которых нет в источнике.
- Хранить result в БД; обновлять только если source hash или profile/prompt version политики требует regeneration.

**Stage C: выбранная embedding model → embeddings description**

- Создавать embedding для каждого действующего description; сохранять vector, dimension, model ID/version и hash embedded text.
- Не смешивать в одном query vectors разных моделей/размерностей. При смене embedding model предусмотрен отдельный re-embedding pass.
- При изменении содержимого сбросить релевантные последующие стадии и пометить прошлые результаты stale, не теряя аудит/историю.

**Параллелизм и восстановление**

- У каждой стадии есть настраиваемый concurrency, rate limit, timeout, retry/backoff и лимиты токенов. Не запускать неограниченное число модельных запросов.
- Job lease и checkpoints сохраняются в БД; после crash незавершённая работа подбирается повторно с идемпотентным ключом `file_id + content_hash + stage + model/prompt version`.
- Сохранять сырые ответы модели только если включена настройка диагностики и с учётом политики хранения секретов; по умолчанию достаточно нормализованного результата и provenance.

## 5.1 Реализованный M2 pipeline в версии 0.9.2

CLI-команда `asgend-memory enrich ROOT` реализует две возобновляемые стадии через OpenAI-compatible `POST /chat/completions` endpoints. Настройки передаются переменными окружения: `ASGEND_MEMORY_DECISION_BASE_URL`, `ASGEND_MEMORY_DECISION_MODEL`, `ASGEND_MEMORY_GENERATION_BASE_URL`, `ASGEND_MEMORY_GENERATION_MODEL`; необязательные API keys — `ASGEND_MEMORY_DECISION_API_KEY` и `ASGEND_MEMORY_GENERATION_API_KEY`. Секреты не записываются в SQLite и не включаются в журналы. Транспорт использует HTTPS или HTTP для локально управляемых endpoints, без follow-redirect; ответ ограничен 4 MiB.

- Decision stage получает и существующие OKF metadata (read-only, ограниченный размером JSON), и тело файла; `memory-decision-v1` выдаёт нормализованный JSON (`type`, `tags`, `keywords`, `language`, `content_kind`, `entities`, `references`, `confidence`). Неизвестные ключи отбрасываются, длины строк и число элементов ограничены. Содержимое источника считается недоверенными данными, а не инструкциями модели.
- Description stage `memory-description-v1` создаёт `title` и `description`. Ключ задания включает model/prompt зависимости decision stage, поэтому смена classifier model пересоздаёт description. Обе стадии привязаны к `file_id + source_sha256 + model_id + prompt_version`.
- В `pipeline_jobs` хранятся состояние, попытки, ошибки и `next_attempt_at`. Ошибки получают ограниченный экспоненциальный backoff; `--retry` принудительно обходит ожидание. `--workers` задаёт ограниченный параллелизм 1–16, каждый worker использует своё соединение SQLite. Параллелизм по файлам не нарушает порядок двух стадий внутри одного файла.
- `model_metadata` отдельно хранит модельную классификацию; исходный `file_metadata` остаётся зарезервированным под metadata, прочитанные из OKF. `descriptions` хранит title/description и provenance. Оригинальные файлы не меняются.
- Перед выполнением `enrich` надо запустить `scan` и `extract`. При изменении источника новые результаты формируются только после обновлённого `scan` и `extract`; старые записи сохраняют hash для истории и не считаются текущими.

Пример:

```sh
export ASGEND_MEMORY_DECISION_BASE_URL=http://127.0.0.1:8102/v1
export ASGEND_MEMORY_DECISION_MODEL=decision-model
export ASGEND_MEMORY_GENERATION_BASE_URL=http://127.0.0.1:8101/v1
export ASGEND_MEMORY_GENERATION_MODEL=generation-model
./build/asgend-memory enrich /path/to/project --workers 4 --timeout-seconds 120
```

Current limitation: оба профиля должны предоставлять OpenAI-compatible `chat/completions`; типизированный System One endpoint сам по себе не является генеративным Chat Completions API. Embeddings и подключение памяти к orchestrator ещё не реализованы в этом релизе.

## 6. Граф файлов

- Markdown: parse links, relative links, references and existing frontmatter source fields без изменения файлов.
- Код: начать с безопасных и детерминированных статических парсеров импортов/includes для приоритетных языков; далее расширять plugin registry.
- Общие отношения: `links_to`, `imports`, `includes`, `references`, `same_content_as`, `generated_from`/`source_of` только при наличии подтверждаемого provenance.
- Каждый edge содержит источник обнаружения, hash исходного файла и confidence; удаление/изменение файла инвалидирует соответствующие рёбра.
- Не позволять модели произвольно создавать подтверждённые dependency edges: модельные предположения хранятся как отдельный низко-доверенный тип edge.

## 7. Search/retrieval pipeline

1. Получить вопрос и исходный request context от orchestrator.
2. По умолчанию выполнить локальный retrieval без дополнительного запроса к decision model. Для явного CLI `query --plan` можно opt-in вызвать OpenAI-compatible planner (`ASGEND_MEMORY_DECISION_BASE_URL`, `ASGEND_MEMORY_DECISION_MODEL`, optional `ASGEND_MEMORY_DECISION_API_KEY`). Планировщик видит только текст запроса и allowlist intents, не excerpts/документы; пользовательский запрос считается недоверенными данными, а не инструкцией.
3. При успешном ответе принять только строгий JSON с фиксированными ключами, allowlisted intent, не более чем тремя короткими rewrite-фразами и опциональными типом/tag/root-relative path без `..`. Неизвестные ключи, markdown, управление/лишний текст и небезопасные фильтры отклоняются целиком. User-supplied filters имеют приоритет. Timeout, invalid URL/transport, HTTP и schema error не проваливают поиск: используется локальный путь, причина fallback присутствует в JSON response.
4. Построить кандидатов через BM25/FTS5, exact matches, metadata filters и semantic similarity по description embeddings.
5. Расширить/уменьшить кандидатный набор по графовым рёбрам на ограниченную глубину с decay и защитой от популярного hub-node.
6. Объединить нормализованные оценки BM25, семантической близости, metadata, path/type match, свежести и graph proximity; веса должны быть конфигурируемы и тестируемы.
7. Только при отдельном CLI `--rerank` отдать top-K кандидатных фрагментов в decision-model relevance qualification/reranker вместе с критериями вопроса. Ответ модели ограничен предоставленным списком file IDs; неизвестные IDs отбрасываются.
8. Собрать evidence bundle: path, snippet, source hash, offset, ranking/relevance и краткое основание. Обеспечить token budget и дедупликацию.
9. Вернуть evidence bundle в исходный execution flow, используя ту же модель/маршрут, куда был отправлен первоначальный запрос. Memory retrieval не подменяет модель и не возвращает непроверенный сгенерированный ответ вместо evidence.

### Opt-in query planner (`query --plan`)

> **Experimental in 1.0.0.** `--plan` and `--rerank` are supported interfaces, but their retrieval quality has only been checked with synthetic/mock fixtures, not an independent human-judged corpus. Default retrieval stays local (BM25/hybrid/graph) and does not call a decision model. Evaluate both on your own corpus with `scripts/evaluate_memory_retrieval.py --compare-plan` before relying on them.

- Sends a JSON envelope containing the raw query and the fixed intent allowlist to a configured decision model. Retrieved source text is not included in this request.
- Accepts strict JSON only: `intent`, `reason`, up to three `rewrites`, and `filters` with nullable `type`, `tag`, `path_prefix`. Additional/missing keys or values outside bounds reject the entire plan.
- Type/tag values use conservative labels; paths must be root-relative, slash-separated and contain no empty, `.` or `..` segment. Explicit command-line filters override planner suggestions.
- Rewrites supplement the original BM25 query; the original query is preserved. The planner intent can select only fixed local graph weights/families. Planner errors return to local retrieval and are visible as `query_plan.fallback_reason`.
- The planner is opt-in on the CLI. Automatic agent retrieval remains local-only unless a future explicitly designed policy changes that behavior.

## 8. Инкрементальное обновление

- Сначала надёжный периодический scan/hash; watcher (`inotify` на Linux и переносимый polling fallback) — оптимизация, а не единственный источник истины.
- При изменении файла переизвлечь только этот файл; invalidate metadata/title/description/embedding только по content hash/prompt/model policy; обновить относящиеся рёбра.
- Для удаления хранить tombstone и удалять файл из активного FTS/vector search, сохраняя историю согласно retention policy.
- Держать scan generation и last complete generation, чтобы незавершённый scan не помечал массово файлы удалёнными.
- Опционально использовать Git object/history если `ROOT` — Git repo, но корректность не должна зависеть от наличия `.git`.

## 9. Тесты и критерии приёмки

- Fixture с несколькими уровнями вложенности, unicode paths, пустыми/бинарными/неподдерживаемыми/большими файлами, symlink loop и путём за пределы корня.
- Первый scan индексирует запись для каждого файла; второй без изменений не вызывает model/embedding API повторно.
- Изменение одного Markdown/code файла инвалидирует только необходимые стадии, FTS и graph edges; rename определяется корректно или консервативно помечается delete+add.
- Проверить existing OKF frontmatter parse; Markdown без frontmatter не изменяется; тест сравнивает bytes до/после.
- Искусственная остановка между стадиями и перезапуск возобновляют очередь без потери результата и без дублирующих записей.
- При timeout/ошибке модели file-level job retryable; остальной каталог продолжает работать.
- BM25 на фиксированном корпусе возвращает известные релевантные документы; graph expansion повышает известную связанную страницу без вытеснения точных совпадений.
- Retrieval тесты проверяют фильтры metadata, ограничение top-K, лимиты токенов, валидацию IDs от reranker и передачу контекста исходной модели.
- Sanitizers (ASan/UBSan), SQLite integrity check, fuzz-тесты парсинга frontmatter/links, performance benchmark и migration tests.


## 5.2 Реализованный M3 Embeddings + BM25 query в версии 0.9.3

- `asgend-memory embed ROOT [--workers N] [--timeout-seconds N] [--retry]` использует OpenAI-compatible `POST /v1/embeddings`. Настройки: `ASGEND_MEMORY_EMBEDDING_BASE_URL`, `ASGEND_MEMORY_EMBEDDING_MODEL`, необязательный `ASGEND_MEMORY_EMBEDDING_API_KEY`.
- Вход embedding формируется как `title + "\n" + description`, а SHA-256 рассчитывается именно от этого текста. `embeddings` сохраняет `description_id`, `file_id`, `input_sha256`, `model_id`, размерность и JSON-вектор. Изменение title/description делает старый вектор неактуальным без удаления аудиторской истории. Поисковая выдача проверяет hash текущего текста и не использует устаревший vector.
- Размерность вектора ограничена 65 536; каждый элемент проверяется на числовой тип и конечное значение, вектор нулевой нормы отклоняется. Для одного `model_id` размерность должна быть постоянна; при изменении поставщиком размерности нужно указать новый model ID, чтобы не смешивать пространства векторов.
- `pipeline_jobs.stage='embedding'` хранит состояние, попытки и exponential backoff. Межпроцессная блокировка `.embed.lock` не позволяет параллельно запустить две embed-команды для одного project root. Workers ограничены 1..16; каждый использует отдельное SQLite connection.
- Ключи API берутся из environment, не помещаются в SQLite. Если ключ настроен, разрешены HTTPS endpoint или loopback HTTP; remote plain HTTP запрещён. Redirects выключены, API responses имеют предел 4 MiB, protocol ограничен HTTP/HTTPS.
- Миграция schema v3→v4 создаёт `embeddings`, `memory_search_docs` и FTS5 `memory_fts`. SQLite triggers обновляют документ поискового индекса при изменении `files`, `file_contents`, `file_metadata` и `descriptions`. BM25 веса выше отданы title/description, чем пути и сырому тексту; FTS-операторы из пользовательского запроса безопасно экранируются кавычками.
- `asgend-memory query ROOT QUERY [--limit N] [--timeout-seconds N] [--lexical-only] [--plan] [--rerank [--rerank-candidates N]]` возвращает JSON с `path`, `title`, `description`, excerpt, related graph edges, BM25 score/rank, cosine similarity/rank и fused score. При настройке embedding endpoint/model query также отправляется embedding API, а кандидаты объединяются Reciprocal Rank Fusion (`1/(60+rank)`). Без endpoint поиск остаётся локальным BM25 и не вызывает сеть. Query text не записывается в журнал/базу. Для перестроения связей отдельно вызывается `asgend-memory graph ROOT`.
- Cosine search версии 0.9.3 перебирает совместимые сохранённые vectors, поддерживая top-100 кандидатов; это точный linear scan, не ANN/vector database. В 0.9.4 добавлен file graph и automatic retrieved evidence injection; query classification, chunk-level offsets и advanced metadata filters ещё не реализованы.

**Проверки M3:** локальный OpenAI-compatible mock проверяет `/v1/embeddings`, multi-worker concurrency, сохранение векторов, idempotent rerun, invalidation по изменению description, hybrid ranking, BM25-only query, API-key non-persistence, запрет удалённого plain HTTP с ключом, FTS trigger sync и отсутствие модификации исходных файлов.


## 5.3 Реализованный M4 Graph + agent retrieval в версии 0.9.4

- `asgend-memory graph ROOT` транзакционно перестраивает `file_edges` из локальных Markdown ссылок `](...)`, wiki links `[[...]]`, C/C++ quoted `#include` и относительных imports в JS/TS/Python-like source. Внешние URL и пути за пределы root игнорируются; мёртвые цели не создают рёбра. Текущий extractor text должен быть выполнен до graph pass.
- `query` graph-expands до шести соседних файлов для первых двенадцати direct candidates. Результаты содержат `excerpt`, `related`, `retrieved_via` и `graph_edge_type`; graph score слабее direct BM25/semantic RRF score, чтобы точное совпадение оставалось выше случайного соседа.
- `agent_execute` автоматически выполняет локальный `query`, только если `.asgend_mem/index.sqlite3` существует как обычный файл внутри корня проекта. Retrieval ограничен пятью результатами, не вызывает embedding/network endpoint, передаёт JSON evidence той же выбранной модели и маркирует содержимое как недоверенные источники, а не инструкции. Нет индекса или нет совпадений — задача продолжается без memory context.
- Schema v5 добавляет `file_edges` с внешними ключами, типом ссылки, сырым reference и source hash. `graph` идемпотентен и заменяет актуальный граф транзакционно.
- Остаётся реализовать incremental edge invalidation/rebuild, chunk-level source offsets, query classification, field-based filters, token-budget-aware evidence pack, observability и окончательные security tests на prompt-injection corpus.


## 5.4 Реализованный M6 query intent + chunk embeddings в версии 0.9.6

- Schema v8 создаёт `chunk_embeddings`: `chunk_id`, `file_id`, `source_sha256`, `input_sha256`, `model_id`, `dimensions`, `vector_json`. Input для хеша — `title + newline + description + newline + chunk_text`. Вектора сохраняются отдельно от document embeddings; размерность должна быть постоянной внутри `model_id`.
- `asgend-memory embed ROOT --chunks [--chunk-tokens N]` сначала выполняет существующую document embedding стадию, затем синхронизирует source-aware chunks и генерирует chunk vectors ограниченным worker pool. Текущие vectors пропускаются; ошибки остаются повторяемыми при следующем запуске. Без `--chunks` поведение backward-compatible.
- Hybrid `query` вычисляет embedding исходного запроса один раз, ранжирует document и chunk vectors, дедуплицирует semantic candidates по файлу и предпочитает matched chunk excerpt для semantic-only result. Перед использованием проверяются source SHA-256, input SHA-256, model ID и dimension. Lexical BM25 и filters работают независимо.
- Если `--chunk-tokens` не передан в query, индексатор сохраняет существующий размер чанков; для stale/missing state предпочитается наиболее частый сохранённый размер. Явный `--chunk-tokens` может перестроить chunk index, что также удалит vectors каскадом для старых chunk IDs.
- Локальный classifier возвращает `intent`, `method=local_heuristic`, `reason` и `graph_edge_family`: `dependency`, `debugging`, `configuration`, `code`, `documentation`, `general`. Classifier влияет на предпочтение `source_include/source_import` versus `markdown_link/wiki_link` и вес graph-only candidates. Внешняя decision model не вызывается.
- Автоматический agent retrieval добавляет `query_classification` в evidence и продолжает использовать lexical-only local BM25; это сохраняет отсутствие дополнительных model/API calls и не выполняет implicit semantic query embedding.
- Тесты в `test_memory_embedding.py` проверяют миграцию v8, cache hit, chunk vector persistence, hybrid chunk search, cache reuse и secret non-persistence; `test_memory_graph.py` проверяет классификацию документационного и dependency-запросов.

**Ограничения 0.9.6:** local query classifier — эвристика, а не decision-model classifier; chunk semantic ranking использует линейный cosine scan, не ANN. Agent path не вызывает query embedding API, поэтому semantic chunk vectors работают в explicit hybrid CLI query, а не в скрытом online agent path. Окончательная оценка качества на большом real-world корпусе ещё не проведена.

## 5.5 Реализованный M7 — constrained reranking и evaluation в версии 0.9.7

- `query --rerank [--rerank-candidates N]` отправляет только предварительно найденные top 1..20 результатов выбранной decision/reranking модели. Endpoint variables: `ASGEND_MEMORY_RERANK_BASE_URL`, `ASGEND_MEMORY_RERANK_MODEL`, optional `ASGEND_MEMORY_RERANK_API_KEY`. Без флага запрос к decision endpoint не выполняется.
- Каждый документ получает временный alias-ID от 1 до N. Модель возвращает строгий JSON `ranking`; неизвестные/повторные IDs и relevance вне диапазона 0..4 отбрасываются. Reranker не может добавить файл, который не был найден основным retrieval. Excerpts ограничены по размеру и явно обозначены как untrusted data.
- При передаче исходных фрагментов в remote endpoint требуется HTTPS; HTTP допускается только на loopback. API keys не сохраняются в БД; содержимое запросов и документов не логируется.
- `scripts/evaluate_memory_retrieval.py` оценивает размеченный JSON dataset с Precision@K, Recall@K, MRR@K и nDCG@K. По умолчанию используются offline lexical-only запросы для воспроизводимости; hybrid mode включается отдельно. `--plan` оценивает planner-режим; `--compare-plan` запускает paired baseline/planned проходы на одинаковых запросах и возвращает delta planned minus baseline для каждой метрики. Для планового запуска нужно задать `ASGEND_MEMORY_DECISION_BASE_URL` и `ASGEND_MEMORY_DECISION_MODEL`.
- `tests/test_memory_rerank.py` проверяет вызов mock decision API, порядок, запрет unknown IDs, авторизацию и cleartext URL policy. `tests/test_retrieval_eval.py` оценивает маленький lexical corpus. `tests/test_query_plan_eval.py` проверяет paired evaluation на детерминированном mock planner и smoke relevance labels.
- `tests/fixtures/query_plan_eval_dataset.json` — небольшой тестовый smoke-набор, а не независимый human-judged корпус; он проверяет измерительную инфраструктуру, но не доказывает качество реальной модели.

**Ограничения 0.9.7:** reranker применяется только в explicit CLI query, не в автоматическом agent request path. Для agent route сохранён local-only BM25, чтобы избежать скрытых сетевых вызовов/задержек. Semantic vector lookup остаётся точным линейным scan; ANN и performance benchmark больших корпусов — следующий этап.


## 5.6 Persistent ANN и benchmark масштаба в версии 0.9.8

- Schema v9 добавляет nullable `ann_signature` к document/chunk embedding tables и indexes по `(model_id, ann_signature)`. При миграции проверяется `PRAGMA table_info`, чтобы повторные/частично обновлённые БД не завершались ошибкой `duplicate column`.
- `src/memory/ann.c` вычисляет детерминированную 16-bit random-hyperplane signature. В запросе генерируются LSH bucket probes для Hamming distance ≤4 (2,517 signatures) и SQLite использует индекс для отбора кандидатов; cosine similarity остаётся точной на полученном подмножестве.
- Новые векторы подписываются на этапе сохранения. `asgend-memory ann-index ROOT` заполняет signatures для ранее сохранённых document/chunk vectors пакетами по 512 записей и сообщает некорректные векторы. API-key и исходное содержимое в индексную запись не добавляются; сохраняется только derived signature.
- ANN включается автоматически при ≥5,000 актуальных векторов одного `model_id`, полном заполнении signatures и не менее 32 кандидатах в LSH bucket probes. На небольших корпусах/неполном индексе используются прежние точные cosine scans. В query с `--type`, `--tag`, `--path-prefix` или `--metadata` ANN намеренно отключается, чтобы сохранять recall для суженной выборки. `--exact-vectors` — явный эталонный режим.
- Query JSON возвращает `vector_search` (`ann_lsh16`, `exact_cosine` или `lexical_only`), `document_ann_used`, `chunk_ann_used`, counts и candidate counts. Benchmark `scripts/benchmark_memory_vectors.py` сравнивает paired latency и Recall@K path overlap. Время включает embedding API request, а значит зависит от endpoint и сети.
- Синтетический регрессионный тест индексирует 5,200 векторов размерности 128, проверяет schema v9/backfill, сокращение candidate pool и совпадение top-1 с точным cosine режимом.

**Ограничения:** индекс — single-table random-hyperplane LSH, не HNSW/IVF. Радиус 4 — компромисс recall/скорость, который нужно тюнить на реальном размеченном наборе. ANN работает в explicit hybrid CLI query; automatic agent retrieval остаётся local-only BM25.


## 5.7 Реализованный M10 — автоматическая синхронизация памяти и resource retrieval в версии 0.9.9

### Два отдельных индекса

- Project memory: `paths.project_root/.asgend_mem/index.sqlite3` — рекурсивное состояние проекта, extraction, OKF-read-only metadata, descriptions/embeddings/chunks/graph и retrieval по исходным файлам. По обратной совместимости `paths.project_root` по умолчанию равен `root_dir`; если код хранится отдельно от конфигурации, задайте путь явно.
- Resource memory: `ROOT/.asgend_mem/resources.sqlite3` — separate FTS5/BM25 index for configured `skills`, `system_prompts`, `tools` (including live MCP tool descriptions) and `knowledge`. Для каждого resource сохраняются категория, root-relative path, body, SHA-256 и время синхронизации.
- Resource DB синхронизируется в `resources_prepare_agent` перед подготовкой каждой подзадачи. Изменившийся SHA-256 приводит к UPSERT и обновлению FTS; новые файлы индексируются, исчезнувшие удаляются только после полного прохода соответствующего читаемого каталога; ошибочный/недоступный каталог не очищает старые записи. Dynamic MCP tool descriptions входят в tools category.
- Project memory синхронизируется перед каждым агентским вызовом: `scan` + `extract` локально, root-level resource folders (`skills`, `knowledge`, `tools`, `system_prompts`, `instructions`) исключены из project corpus. На scan delta обновляется граф связей; source-aware chunks обновляются query pipeline. Metadata/FTS/vector outputs остаются только в `.asgend_mem`, никакие исходники и YAML frontmatter не записываются. Каждые 300 секунд scan принудительно проверяет хеши всех файлов, чтобы обнаруживать случаи с сохранёнными size/mtime.
- Локальный retrieval добавляется в system context выбранной agent-модели с метками источника и запретом трактовать извлечённый контент как инструкции. Default — BM25-only. При явно настроенных переменных `ASGEND_MEMORY_DECISION_BASE_URL` + `ASGEND_MEMORY_DECISION_MODEL` + `ASGEND_MEMORY_GENERATION_BASE_URL` + `ASGEND_MEMORY_GENERATION_MODEL` на основе durable jobs возобновляется enrich pipeline. При настроенных `ASGEND_MEMORY_EMBEDDING_BASE_URL` + `ASGEND_MEMORY_EMBEDDING_MODEL` автоматизируются document/chunk embeddings и query становится hybrid. API keys берутся только из env. Без настройки provider model stage не запускается и не блокирует задачу внешними запросами.
- Model stages автоматически проверяются на старте первого sync, при любом изменении проектных источников и с минутным интервалом для resumable failed/deferred jobs. Pipeline state/hash позволяет пропускать уже обработанные версии источников.
- `resources_status_json()` возвращает `selection_mode=local_bm25`, `resource_index_auto_sync=true`, `classification_parallel=false` и путь `.asgend_mem/resources.sqlite3`. Формальный результат task resource selection показывает `decision_model_calls=0`; task planning/routing по-прежнему может вызвать отдельную decision-модель. Ресурсный retrieval никогда не расширяет tool allow-list агента.

### Ограничения и эксплуатационные примечания

- Полная scan требует O(number of files) обхода директории на каждом agent invocation; content hashing чаще запускается каждые 300 секунд, чтобы оставить основной fast path лёгким. Для сверхбольших monorepo нужен watcher/change queue или file-system notifications.
- Auto model stages запускаются синхронно при наличии настроенных endpoint/model env vars и могут добавить latency, когда есть новые/изменённые документы; успешные jobs кешируются, ошибки/deferred jobs уважают сохраняемый retry backoff.
- Отсутствие model env vars намеренно не мешает локальному индексированию и BM25. Семантический retrieval/embeddings включается только при явной конфигурации embedding provider.


## 5.11 M5 lifecycle: `stats` и `gc`

- `asgend-memory stats ROOT` выводит JSON для автоматического мониторинга: schema version, размер основной SQLite БД и WAL/SHM, количества активных и tombstone-файлов, актуального извлечённого текста, текущих/устаревших chunks и vectors, актуальных/устаревших графовых рёбер, распределение `pipeline_jobs` по stage/state и последние метрики scan.
- `asgend-memory gc ROOT` в `BEGIN IMMEDIATE` удаляет только производные строки, которые не относятся к активному исходнику: tombstone search documents и FTS postings, chunks/chunk states со stale source hash, document/chunk vectors неактуального source, pipeline jobs старого source и устаревшие graph edges. Сохраняет `files` как identity/tombstones, `scan_runs`, `file_snapshots`, `file_events` и исторические `descriptions`.
- `asgend-memory gc ROOT --vacuum` дополнительно запускает FTS5 `optimize`, checkpoint WAL и `VACUUM` после транзакции очистки. Без `--vacuum` физическое уплотнение файла БД не обещается.
- Гарантии проверяются `tests/test_memory_gc.py`: тест создаёт stale chunk/vector и job, удаляет исходный файл, убеждается в удалении производных строк, сохранении истории, корректном JSON `stats` и успешном `PRAGMA integrity_check`.

Пример:

```sh
asgend-memory stats /path/to/project | jq .
asgend-memory gc /path/to/project
asgend-memory gc /path/to/project --vacuum
```
