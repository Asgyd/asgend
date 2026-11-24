# Agent Orchestrator C

> **Статус выпуска:** ASGEND `1.0.0` — стабильный релиз, утверждён 2026-10-22; все обязательные гейты пройдены (сборка/тесты ядра включая долгий `integration`, live PostgreSQL 16, Docker Compose backup/restore drill, Qt 6 TGZ/DEB/RPM, Flutter analyze/test/APK, ревью безопасности и нагрузки). Все компоненты имеют версию `1.0.0` (Flutter build `1.0.0+2`). `query --plan`/`--rerank` в 1.0.0 — experimental; поддерживается одна реплика Control Plane. См. [release notes](docs/RELEASE_NOTES_1.0.0.md), [release readiness](docs/RELEASE_STATUS.md), [Definition of Done](docs/RELEASE_CHECKLIST_1.0.md) и [`TODO.md`](TODO.md).

Модульный сервер на C с TCP/JSON API, защищённым Unix-domain socket, локальной веб-панелью, приоритетной устойчивой очередью, трёхэтапной декомпозицией и ограниченной средой выполнения MCP-процессов.

## Что есть в текущей версии (1.0.0)

- TCP и Unix-domain socket API: `ping`, `health`, `list_agents`, `list_tasks`, `dead_letters`, `retry_task`, `submit`, `status`, `cancel`. Unix-сокет работает с режимом `0600`, проверяет UID peer и размещается только в каталоге владельца без group/world write.
- SQLite/WAL durable queue: задачи фиксируются перед подтверждением, атомарно выдаются worker-потокам, переживают рестарт, поддерживают exponential retry backoff (1/2/4/8/16/30 секунд), идемпотентность, приоритеты `0..9` и aging для предотвращения голодания низкоприоритетных задач. Failed dead letters доступны через `dead_letters` и вручную возвращаются в очередь через `retry_task`.
- Кооперативная отмена: queued-задача отменяется сразу; running-задача получает `cancel_requested`. Активные HTTP-запросы поддерживаемых LLM-адаптеров (OpenAI-compatible streaming и System One JSON), а также MCP Streamable HTTP вызовы прерываются через libcurl progress callback. MCP stdio проверяет отмену при ожидании блокировки, записи в pipe и чтении ответа (проверка ожидания — до 100 мс); task ID передаётся во все активные сетевые и MCP-вызовы. Сама отмена MCP-вызова не инициирует перезапуск stdio child-процесса.
- Декларативные агенты в `config/agents/*.json`; у каждого агента собственный system prompt, allow-list tools и список skills.
- Оркестратор: выбор специалиста, три этапа декомпозиции, формализация с критериями приёмки, запуск этапов и сбор результатов.
- Реестр нескольких LLM, отдельная decision-модель и Nodes в `config/models.json`. Шаблон по умолчанию называет decision-модель `liquid-d1-3b` (Liquid AI D1 3B), но профиль выключен, пока не указаны реальные executable/веса либо действующий удалённый endpoint. Обычные модели закрепляются за агентами через `model_id`; кнопки UI отдельно включают/выключают профиль и запускают/останавливают управляемый процесс.
- Автоматический локальный выбор ресурсов (с версии 0.9.9): `skills/`, `system_prompts/`, `tools/` и `knowledge/` автоматически синхронизируются в отдельную базу `.asgend_mem/resources.sqlite3`. При подготовке каждой подзадачи выполняется инкрементальный hash-sync, обновление FTS5/BM25 и выбор релевантных документов локально; четыре отдельных вызова decision-модели для выбора категорий больше не выполняются. Вызов основной decision-модели для планирования/маршрутизации задачи остаётся. Доступные инструменты всегда ограничены allow-list агента: retrieval не может расширить полномочия.
- Nodes обозначают локальное устройство, узел LAN и удалённый endpoint. Модели могут использовать управляемый локальный процесс либо OpenAI-совместимый API через `base_url`. Internet nodes обязаны использовать HTTPS; ключи для удалённых API рекомендуется брать из переменных окружения через `api_key_env`.
- Инструменты `echo`, `current_time`, `knowledge_search`; доступ к ним ограничивается allow-list агента.
- MCP stdio: `initialize`, `tools/list`, `tools/call`, allow-list aliases и persistent supervisor: после успешного handshake падение дочернего процесса или transport-level ошибка чтения/записи инициирует перезапуск с backoff 1, 2, 4, 8, 16 и максимум 30 секунд. Восстановление не требует перезапуска ASGEND; initial handshake по-прежнему обязателен, а Streamable HTTP endpoint не является дочерним процессом. Изоляция процесса: лимиты CPU/RAM/file size/descriptors, сброс root-привилегий до nobody (если сервер работает от root), `no_new_privs`, seccomp deny-network (включая `io_uring`) и отдельный рабочий каталог. Landlock в строгом режиме ограничивает чтение системными runtime-путями и явно заданными путями, а запись — временным каталогом; если Landlock недоступен, MCP не запускается.
- Опциональная web UI на `127.0.0.1`, с отдельным токеном, проверкой Host против DNS rebinding и CSP. Помимо dashboard UI показывает состояние всех моделей, позволяет запускать/останавливать управляемые модели и редактировать главный конфиг, реестр моделей/decision-моделей и профили агентов. Все изменения записываются в реальные файлы каталога `config/`; UI явно показывает необходимость перезапуска, пока активная конфигурация сервера не перечитана. По умолчанию UI отключена.
- Skills из `skills/<skill-id>/SKILL.md`; справочные файлы `.md`/`.txt` подаются модели как данные, а не как инструкции более высокого приоритета.

Это расширяемый MVP, а не готовая multi-tenant production-платформа. TLS в бинарник не встроен, нет пользовательских квот и изоляции через отдельные container/user/network namespaces. Web UI привязана только к loopback и выключена по умолчанию. Редактор конфигов позволяет запускать команды, указанные в реестре моделей, поэтому UI-токен является административным секретом: включайте панель только локально и выдавайте доступ доверенным пользователям. До сетевого развертывания добавьте TLS-прокси/VPN, отдельную учётную запись ОС для MCP-процессов и эксплуатационный мониторинг.

## Требования

Linux, GCC/Clang, CMake, `pkg-config`, `json-c`, `libcurl`, SQLite3, OpenSSL libcrypto, pthreads и Python 3 для интеграционных тестов.

Debian/Ubuntu:

```bash
sudo apt-get install build-essential cmake pkg-config libjson-c-dev libcurl4-openssl-dev libsqlite3-dev libssl-dev python3
```

Сборка и тестирование:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/agent-orchestrator --check-config -c config/config.json
./build/agent-orchestrator -c config/config.json
```

Альтернативные команды через Makefile: `make`, `make test`, `make run`.

### Индекс проектной памяти (M1–M5, текущая реализация)

Отдельный C-модуль `asgend_memory` и CLI `asgend-memory` рекурсивно индексируют файлы в SQLite, рассчитывают SHA-256, сохраняют snapshots/события и обнаруживают add/modify/delete/rename. Команда `extract` извлекает UTF-8 текст с лимитом 4 MiB по умолчанию, отделяет существующий OKF frontmatter у Markdown и сохраняет поля метаданных в SQLite. Производная база размещается в `<ROOT>/.asgend_mem/index.sqlite3`; исходные документы не меняются, symlink и переходы на другие файловые системы по умолчанию не обходятся. Ограничение размера исходного файла — 256 MiB.

```bash
# Сборка создаёт build/asgend-memory
./build/asgend-memory scan /path/to/project
./build/asgend-memory status /path/to/project
# Детальные счётчики стадий, валидных/устаревших векторов и размер БД (JSON)
./build/asgend-memory stats /path/to/project
# Удалить устаревшие производные записи; --vacuum дополнительно уплотняет SQLite
./build/asgend-memory gc /path/to/project --vacuum
./build/asgend-memory events /path/to/project 50
# Принудительно пересчитать SHA-256, включая файлы с прежним size/mtime
./build/asgend-memory verify /path/to/project
# извлечь текст и существующий OKF frontmatter в SQLite без изменения источников
./build/asgend-memory extract /path/to/project

# Настроить OpenAI-compatible API профили decision и description моделей
export ASGEND_MEMORY_DECISION_BASE_URL="http://127.0.0.1:8102/v1"
export ASGEND_MEMORY_DECISION_MODEL="decision-model-name"
export ASGEND_MEMORY_GENERATION_BASE_URL="http://127.0.0.1:8101/v1"
export ASGEND_MEMORY_GENERATION_MODEL="generation-model-name"
# API keys необязательны для локального endpoint; если нужны — передавать только через env:
# export ASGEND_MEMORY_DECISION_API_KEY="..."
# export ASGEND_MEMORY_GENERATION_API_KEY="..."
./build/asgend-memory enrich /path/to/project --workers 4 --max-input-chars 24000

# Настроить embedding-модель; endpoint может быть OpenAI-compatible /v1
export ASGEND_MEMORY_EMBEDDING_BASE_URL="http://127.0.0.1:8103/v1"
export ASGEND_MEMORY_EMBEDDING_MODEL="embedding-model-name"
# export ASGEND_MEMORY_EMBEDDING_API_KEY="..."
./build/asgend-memory embed /path/to/project --workers 4
# Опционально: дополнительно создать embeddings для source-aware чанков
./build/asgend-memory embed /path/to/project --workers 4 --chunks --chunk-tokens 400

# Гибридный поиск (embedding query + BM25 + cosine similarity; chunk vectors включаются, если были созданы)
./build/asgend-memory query /path/to/project "where is task cancellation implemented" --limit 10
# Локальный BM25 без запроса к embedding API
./build/asgend-memory query /path/to/project "task cancellation" --lexical-only
./build/asgend-memory graph /path/to/project
# построить чанки с координатами исходника (переиспользуются при повторных запусках)
./build/asgend-memory chunk /path/to/project --max-chunk-tokens 400
# BM25 по фрагментам, с ограничением контекста и metadata-фильтрами
./build/asgend-memory query /path/to/project "task cancellation" --lexical-only \
  --context-tokens 1800 --chunk-tokens 400 --path-prefix src/ --tag orchestration
# фильтр по OKF/model metadata
./build/asgend-memory query /path/to/project "token budget" --type documentation --metadata audience=internal

# Построить approximate nearest-neighbour signatures для существующих document/chunk vectors
./build/asgend-memory ann-index /path/to/project
# Точный полный cosine scan для сверки с ANN
./build/asgend-memory query /path/to/project "cancellation internals" --exact-vectors

# Парный benchmark latency и Recall@K (embedding env выше должен быть настроен)
cat > /tmp/asgend-queries.json <<'EOF'
["task cancellation", "MCP process lifecycle", "SQLite retry backoff"]
EOF
./scripts/benchmark_memory_vectors.py --cli ./build/asgend-memory --root /path/to/project --queries /tmp/asgend-queries.json --k 10 --repetitions 3

# Опциональное планирование запроса до retrieval. Передаётся только текст запроса,
# а не найденные документы; ошибки модели/схемы безопасно возвращают локальный поиск.
# Используются ASGEND_MEMORY_DECISION_BASE_URL / ASGEND_MEMORY_DECISION_MODEL выше.
# В 1.0.0 --plan и --rerank — EXPERIMENTAL: качество не оценено на human-judged корпусе.
./build/asgend-memory query /path/to/project "where is task cancellation implemented" --lexical-only --plan

# Явный decision-model rerank только для уже найденных top-K кандидатов
export ASGEND_MEMORY_RERANK_BASE_URL="https://your-endpoint/v1"
export ASGEND_MEMORY_RERANK_MODEL="your-decision-model"
export ASGEND_MEMORY_RERANK_API_KEY="..." # необязательно, если endpoint без авторизации
./build/asgend-memory query /path/to/project "task cancellation" --rerank --rerank-candidates 12

# Оценить retrieval на размеченном датасете; offline BM25 по умолчанию
python3 scripts/evaluate_memory_retrieval.py --cli ./build/asgend-memory \
  --root /path/to/project --dataset judged-queries.json

# Парное сравнение без planner и с query --plan (настроить ASGEND_MEMORY_DECISION_* выше)
python3 scripts/evaluate_memory_retrieval.py --cli ./build/asgend-memory \
  --root /path/to/project --dataset judged-queries.json --compare-plan
```

Обычный `scan` использует size + nanosecond mtime как быстрый hint и переиспользует сохранённый hash только при совпадении обеих метрик; `verify` всегда читает содержимое заново. Неполный проход по каталогам не помечает непосещённые файлы удалёнными. В 0.9.0–0.9.1 реализованы scan/state и UTF-8/OKF extraction; 0.9.2 добавляет возобновляемый decision/title/description pipeline; 0.9.3 — embeddings и FTS5 BM25/hybrid query; 0.9.4 — граф локальных зависимостей и автоматическое подключение retrieval evidence в контекст исходной agent-модели; 0.9.5 — source-aware chunks с точными byte/line offsets, chunk-level FTS, metadata-фильтры и ограничение контекста. В 0.9.6 добавлены embeddings для фрагментов и local query-intent классификатор. В 0.9.7 добавлены constrained decision-model reranking и benchmark качества BM25/hybrid retrieval. В 0.9.8 реализован persistent random-hyperplane LSH16 radius-4 ANN: старые векторы индексируются командой `ann-index`, новые получают signatures автоматически. Для активного model ID ANN включается при наличии не менее 5000 актуальных векторов и достаточного числа пробных кандидатов; на маленьких корпусах, при неполном индексе, с metadata-фильтрами и с `--exact-vectors` используется точный cosine scan. Добавлен парный benchmark ANN против exact с Recall@K и медианным ускорением. Автоматический agent retrieval по умолчанию local-only BM25; начиная с 0.9.9 настроенный `ASGEND_MEMORY_EMBEDDING_BASE_URL`/`ASGEND_MEMORY_EMBEDDING_MODEL` включает автоматический hybrid retrieval после инкрементального embed pass. Встроенные PDF и Office OOXML extractors подключены к `asgend-memory extract`: PDF использует bounded zlib FlateDecode, OOXML поддерживает DOCX/XLSX/PPTX через in-memory ZIP/Deflate и XML text extraction с лимитами против zip-bomb; `parsed_partial` участвует в retrieval. `query --plan` теперь предоставляет явное query-only планирование с ограниченной JSON-схемой, валидацией intent/type/tag/relative path, не передаёт модели извлечённые документы и при ошибке безопасно возвращается к обычному поиску. `scripts/evaluate_memory_retrieval.py --compare-plan` парно сравнивает baseline и planned Precision/Recall/MRR/nDCG с metric deltas; тесты используют только детерминированный mock. Large independent human-judged query-plan quality review, Qt/Linux, Android и production acceptance Docker Control Plane остаются открытыми задачами.

Сервер слушает `127.0.0.1:8765` и дополнительно создаёт Unix-сокет `data/agent-orchestrator.sock` с правами `0600`; примерные модели в `config/models.json` выключены, SQLite-файл создаётся в `data/tasks.sqlite3`. Web UI выключена по умолчанию.

## Защищённый Unix-сокет и веб-панель

Unix-сокет включён по умолчанию. Путь: `server.unix_socket.path`; владелец должен совпадать с эффективным UID службы, каталог не должен быть доступен на запись группе или остальным пользователям. Даже при совпадающем UID продолжает действовать `server.auth_token`, если он задан. Пример вызова через Python:

```python
import json, socket
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
    s.connect("data/agent-orchestrator.sock")
    s.sendall((json.dumps({"op":"ping"}) + "\n").encode())
    print(s.recv(4096).decode())
```

Чтобы включить панель, задайте отдельный секрет (не используйте токен LLM или секрет сервера повторно):

```sh
openssl rand -hex 32
```

Установите `ui.enabled` в `true`, скопируйте полученные 64 hex-символа в `ui.auth_token`, проверьте `ui.directory: "ui"` и перезапустите сервис. Откройте `http://127.0.0.1:8766/`; панель попросит токен и хранит его только в `sessionStorage` вкладки. API принимает `X-AO-Token`, а не query-параметр/куки. Сервер привязан только к loopback и отклоняет неожиданный `Host`.

При повторном запуске `install.sh` три принадлежащих приложению UI-файла (`index.html`, `app.js`, `style.css`) обновляются даже в существующей установке; пользовательские дополнительные файлы в каталоге UI сохраняются. После обновления перезапустите сервис и принудительно обновите вкладку браузера. Статические UI-ресурсы отправляются с `Cache-Control: no-store`, а ссылки на JS/CSS получают версию для обхода старого browser cache. Рабочее пространство чата с тремя панелями открывается по умолчанию; прежняя таблица доступна во вкладке «Все задачи».

Панель предоставляет просмотр состояния очереди, списка задач/агентов, приоритета и попыток, просмотр JSON результата, отправку задачи (0–9) и кооперативную отмену. Заголовок задачи генерируется ответом активной LLM на этапе первичного анализа и сохраняется; если модель не вернула заголовок, используется безопасный краткий вариант из запроса. Экран задачи включает список задач, чат и редактор текстовых файлов рабочей папки. В той же области доступны списки To-do и Goals, хранящиеся в ядре и доступные через `/api/plan`; CLI и WebUI используют общие элементы плана. `POST /api/tasks/<id>/files` принимает `{"action":"list","path":""}`, `{"action":"read","path":"notes.txt"}` или `{"action":"write","path":"notes.txt","content":"..."}`. Операции требуют `X-AO-Token`, ограничены рабочим каталогом выбранной задачи, запрещают обход через `..` и символьные ссылки и принимают только текстовые файлы размером до 1 МиБ. В серверном RPC также есть `workspace_read_chunk` (`path`, `offset`, `limit` до 65536 байт): он потоково просматривает большие UTF-8 файлы порциями, старается сохранять границы строк и не загружает целиком файл в память. Редактирование через внешний редактор по-прежнему ограничено 1 МиБ. Дополнительно `GET /api/configs` читает файлы с диска, `PUT /api/configs` валидирует и сохраняет главный конфиг, модели и профили агентов, `GET /api/models` показывает runtime-статусы, а `POST /api/models/<llm|decision>/<id>/<start|stop|enable|disable>` управляет процессом и состоянием профиля. Изменение профилей требует перезапуска, UI показывает `restart_required`; файлы и UI редактируют одну и ту же конфигурацию, а не отдельную копию в браузере. Endpoint API: `GET /api/health`, `GET /api/agents`, `GET /api/tasks`, `GET /api/tasks/<id>`, `POST /api/tasks`, `POST /api/tasks/<id>/cancel`, а также указанные config/model endpoints.

## Голосовой режим и Cowork (2026-11-12)

Веб-панель включает адаптивный режим совместной работы **Cowork**: текстовый ввод, запись аудио через браузерный микрофон, вложения audio/image/video, выделение текста или кода и правой рабочей области проекта. Выбранные фрагменты передаются с меткой файла/объекта; объекты, которые нельзя выделить, можно описать голосом или текстом.

Голосовой и мультимодальный ввод обрабатывает отдельный loopback-сервис `voice_gateway/voice_gateway.py` на базе `LiquidAI/LFM3-Omni-2B`. Посредник получает состояние проекта и накапливает полный структурированный отчёт. Каждый новый ход обновляет номер ревизии; только явное подтверждение последней версии создаёт обычную задачу в очереди ASGEND. Голосовая модель не назначается исполняющим агентом и не вызывает инструменты основной системы.

Профиль LFM3 доступен в `config/models.json` и форме добавления модели как `voice_interpreter`; он выключен по умолчанию. Запуск `llama-server` (GGUF), gateway, переменные окружения, API и ограничения описаны в [docs/COWORK_VOICE_MODE.md](docs/COWORK_VOICE_MODE.md). Итоговые заметки по текущей поставке: [docs/VOICE_COWORK_FLUTTER_RELEASE.md](docs/VOICE_COWORK_FLUTTER_RELEASE.md).

WebUI/PWA остаётся адаптивной и кеширует только оболочку интерфейса. В Flutter local и remote task workspace теперь используют TUI-подобную двухпанельную компоновку на больших экранах: чат/детали задачи слева, файлы агента и визуальный preview справа. На телефоне список файлов остаётся компактным, а выбранный документ/сайт/код открывается в выдвижной нижней панели с масштабированием изображения и переключением Preview/Source. Отрисовка выполняется MCP-инструментами агента внутри изолированной рабочей папки: агент может создавать Word/Excel/PowerPoint/PDF, HTML/CSS/SVG, код и responsive-макеты, запрашивать снимки desktop/tablet/mobile или нужной страницы/слайда и получать PNG обратно в контекст vision-модели для проверки и повторной правки. Flutter показывает сохранённый снимок, но не исполняет проектный HTML/код и не является встроенным Office-редактором. Нативное редактирование бинарных Office-документов в Flutter не заявляется.

## Полноэкранный C CLI и библиотека TUI

При запуске `asgend-cli` внутри интерактивного терминала по умолчанию используется полноэкранный TUI на отдельной статической C-библиотеке `libasgend_tui.a` (`include/asgend_tui.h`). Библиотека отвечает за терминал, раскладку панелей, потоковые плитки, выбор/сворачивание и режим отображения; она не зависит от JSON или сетевого слоя. `install.sh` устанавливает библиотеку и публичный заголовок в `$HOME/.local/lib` и `$HOME/.local/include` (или в переопределённые `XDG_LIB_HOME`/`XDG_INCLUDE_HOME`). Для скриптов и совместимого построчного режима используйте `asgend-cli --no-tui` либо запускайте CLI без TTY.

Экран включает список задач и To-do слева, ленту чата/событий по центру, а справа — файлы текущей рабочей папки и Goals. Текстовые дельты модели отображаются по мере поступления; thinking показывает только индикатор и объём потока, но не раскрывает содержимое reasoning. Формирование tool call показывается плиткой инструмента с скрытыми промежуточными аргументами, затем плитка сворачивается в завершённый вызов. События файлов/рабочей папки создают отдельные плитки, а список файлов обновляется после файловых операций/завершения инструментов.

Клавиши: стрелки или `j`/`k` — перемещение; `Tab` — смена панели; `Enter` — свернуть/развернуть выбранную плитку, открыть задачу, каталог либо текстовый файл в режиме просмотра; `b`/Backspace — родительский каталог; `p` в правой панели — переключение выбора между файлами и Goals; `Esc` закрывает предпросмотр; стрелки, `PgUp`/`PgDn` и вертикальное колесо мыши прокручивают открытый файл. Просмотр больших файлов потоковый, редактирование внешним редактором поддерживает до 1 МиБ; горизонтальное колесо включает режим без переноса и сдвигает вид по горизонтали. Удерживая левую кнопку мыши, можно протянуть выделение по видимым строкам; колёсико в ленте событий прокручивает визуальные строки. В предпросмотре `e` запускает внешний редактор (`$VISUAL`, затем `$EDITOR`, по умолчанию `vi`) для текущего файла; после успешного выхода изменения валидируются как UTF-8 текст до 1 MiB и безопасно сохраняются через `workspace_write` API ядра. Команда редактора разбирается без shell: обычные аргументы, кавычки и экранирование разрешены, но shell-подстановки и конвейеры не исполняются. Колесо над лентой сдвигает окно событий; над Files/Goals перемещает выделение и при необходимости прокручивает список. Левая кнопка мыши выбирает видимую строку. При изменении размера терминала раскладка перестраивается через `SIGWINCH`. `1` — все плитки раскрыты; `2` — все свёрнуты; `3` — динамический режим; `n` — новая задача; `o` — открыть задачу; `t` — добавить To-do; `g` — добавить Goal; `d` — изменить состояние выбранного пункта; `r` — обновить данные; `c` — отменить текущую задачу; `q` — вернуться/выйти. To-do и Goals хранятся в SQLite через API ядра; отдельные TUI-клиенты получают уведомления об изменениях через `plan_wait` и обновляют список без ручного `r`.

## Повторы, dead letters и штатное завершение

После неудачного выполнения очередь сохраняет `next_attempt_at`, и worker не забирает задачу до наступления этого времени. Задержка растёт как 1, 2, 4, 8, 16 секунд, затем ограничивается 30 секундами. Поле `next_attempt_at` доступно через `status` и `list_tasks`; оно отсутствует, когда задержки нет.

`{"op":"dead_letters","limit":50}` перечисляет терминально упавшие задачи с последней ошибкой и количеством попыток. `{"op":"retry_task","task_id":"..."}` возвращает failed-задачу в очередь и выдаёт ровно одну дополнительную попытку; предшествующие события и счётчик попыток сохраняются, а `task_redriven` фиксирует ручной запуск.

При `SIGINT`/`SIGTERM` сервер прекращает принимать новые запросы и не запускает новую работу после начала shutdown. Уже начатые задачи завершаются; полученный worker-ом, но ещё не начатый claim возвращается в очередь без списания попытки. Необработанные queued-задачи остаются в SQLite для следующего старта. Поле `health.draining` и сообщение журнала показывают режим drain.

## Приоритетная очередь

В `runtime.priority_aging_seconds` задаётся интервал повышения effective priority для ожидающих задач (по умолчанию 300 секунд). Приоритет передаётся в JSON как integer от 0 до 9; по умолчанию 5. Внутри одного effective priority первой берётся более старая задача. Aging не меняет сохранённый пользовательский приоритет, он влияет только на порядок выбора.

## Sandbox дочерних MCP-процессов

Пример полей `sandbox` в `config.json`:

```json
{
  "sandbox": {
    "enabled": true,
    "require_landlock": true,
    "allow_network": false,
    "cpu_seconds": 120,
    "memory_mb": 512,
    "file_size_mb": 64,
    "open_files": 64
  }
}
```

MCP-процессы запускаются без shell, с закрытием унаследованных файловых дескрипторов, `RLIMIT_CPU`, `RLIMIT_AS`, `RLIMIT_FSIZE`, `RLIMIT_NOFILE`, `PR_SET_NO_NEW_PRIVS`, запретом сети и опасных системных вызовов seccomp. Если родитель работает от root, child сбрасывает UID/GID до nobody. Landlock в строгом режиме ограничивает запись отдельным каталогом процесса и разрешает чтение системных runtime-путей, абсолютной команды/аргументов и `read_only_paths`, заданных администратором. Чтение system directories вроде `/usr` разрешено для библиотек и исполняемых файлов; секреты, размещённые внутри этих системных путей, следует считать доступными sandbox-процессу. При `require_landlock: true` startup MCP завершится ошибкой, если Landlock недоступен/заблокирован; не отключайте этот режим для недоверенных инструментов. В некоторых контейнерах syscall Landlock фильтруется внешним seccomp-профилем — запускайте сервис в поддерживающем его окружении. Это дополнительная защита, а не замена контейнеру/VM и отдельному OS-пользователю для произвольного недоверенного кода.

## Управление локальными LLM и decision-моделями

Все файлы конфигурации находятся в каталоге `config/`:

- `config/config.json` — сервер, очередь, транспорт, MCP, sandbox, настройки UI и пути.
- `config/models.json` — реестр обычных LLM и отдельно `decision_models`, плюс значения по умолчанию.
- `config/agents/*.json` — инструкции агентов, инструменты, skills и привязка к модели.

См. подробную схему и примеры в [`docs/MODELS.md`](docs/MODELS.md). Пример модели из реестра:

```json
{
  "id": "qwen-coder",
  "name": "Qwen Coder local",
  "enabled": true,
  "auto_start": true,
  "start_at_boot": false,
  "executable": "/usr/local/bin/llama-server",
  "model_path": "/srv/models/qwen-coder.gguf",
  "working_directory": ".",
  "args": ["-m", "{model_path}", "--host", "127.0.0.1", "--port", "{port}"],
  "port": 8101,
  "base_url": "http://127.0.0.1:8101/v1",
  "health_url": "http://127.0.0.1:8101/health",
  "model_name": "local-model",
  "startup_timeout_seconds": 180,
  "shutdown_timeout_seconds": 10,
  "request_timeout_seconds": 180,
  "temperature": 0.2,
  "api_key": ""
}
```

`auto_start: true` означает ленивый запуск при первом обращении к назначенному агенту; `start_at_boot: true` запускает модель при старте сервера. Нужен абсолютный путь к исполняемому файлу, исполняемые права и существующий `model_path`, если он задан. `args` передаётся напрямую в `execv` без shell. Поддерживаемые подстановки: `{port}`, `{model_path}`, `{model_name}`, `{base_url}`, `{root_dir}`. Укажите уникальные порты для разных экземпляров моделей.

Закрепите модель за гибким агентом через поле `model_id` в `config/agents/generalist.json`:

```json
"model_id": "qwen-coder"
```

Если у агента `model_id` пуст, применяется `defaults.model_id` из `config/models.json`. Decision-модель — отдельная сущность из массива `decision_models`, назначается через `defaults.decision_model_id`. Она отвечает за маршрутизацию задачи и model-assisted планирование. Обычный выбор Skills/System prompts/Tools/Knowledge выполняется локально через отдельный BM25 resource index (см. раздел 0.9.9 ниже), а не через decision-модель. Анализ, декомпозиция и финальное ревью используют `defaults.analysis_model_id` или активную модель `tier: "large"`; лёгкие уровни предназначены для исполнения подзадач.

Управлять этими же файлами можно через панель конфигурации: включите `ui.enabled`, задайте отдельный `ui.auth_token` длиной не менее 32 символов, перезапустите сервер и откройте `http://127.0.0.1:8766/`. Панель загружает конфиги с диска и записывает их в `config/`; она проверяет ссылки агентов на модели и показывает требование перезапуска. Поскольку изменения реестра могут запускать произвольные административно заданные команды от пользователя сервиса, не публикуйте панель в сеть и не выдавайте токен недоверенным пользователям.

Старая настройка `local_llm` в `config/config.json` продолжает работать для обратной совместимости, если `paths.models_config` не указывает реестр. Для новых установок рекомендуется `config/models.json`.

Структурированные ответы декомпозиции проверяются и нормализуются. При невалидном JSON этап использует резервный путь и отражает это через `model_assisted=false`; модельный ответ не считается валидной декомпозицией только потому, что HTTP-запрос был успешен.


## Автоматическая память проекта и выбор ресурсов (0.9.9)

ASGEND использует два независимых индекса в корнях, указанных конфигурацией:

- `.asgend_mem/resources.sqlite3` — `skills`, `system_prompts`, `tools` (включая live MCP tool descriptions) и `knowledge`. На каждой подготовке подзадачи ресурсы сверяются с диском, изменившиеся файлы обновляются по SHA-256, новый контент сразу попадает в FTS5/BM25, а записи исчезнувших файлов удаляются только после успешного полного прохода. Поиск по категориям локальный и не делает четыре дополнительных запроса к decision-модели.
- `.asgend_mem/index.sqlite3` — файлы самого проекта. Перед обращением к выбранной агентской модели автоматически выполняются scan и extraction; новые/изменённые документы становятся доступны через BM25. Из project corpus исключаются корневые `skills/`, `knowledge/`, `tools/`, `system_prompts/` и `instructions/`, чтобы не дублировать resource memory. Граф зависимостей обновляется при обнаруженных изменениях файлов. На каждой задаче используется metadata fast path, а каждые пять минут — полная проверка хешей для обнаружения изменений с сохранёнными размером и временем.

Без дополнительных настроек автоматическая индексация остаётся локальной: scan/extract/FTS/BM25 не требуют LLM и не отправляют содержимое наружу. По умолчанию найденный проектный контекст выбирается local BM25 и передаётся в system context той же агентской модели, которой назначена задача. Это контекст-доказательства: документы обозначены как недоверенные данные, не как инструкции более высокого приоритета.

**Корень проектной памяти.** По обратной совместимости `paths.project_root` по умолчанию равен корню конфигурации `root_dir`. Если конфигурация хранится отдельно от исходников, явно укажите настоящий каталог проекта в `config/config.json`:

```json
{
  "paths": {
    "project_root": "/home/user/src/my-project"
  }
}
```

Каталог должен существовать и не быть symlink. Ресурсная БД при этом остаётся в `root_dir/.asgend_mem/resources.sqlite3`, а проектная — в `project_root/.asgend_mem/index.sqlite3`.

Модельные стадии памяти можно включить отдельно переменными окружения. Если настроены обе пары `ASGEND_MEMORY_DECISION_BASE_URL`/`ASGEND_MEMORY_DECISION_MODEL` и `ASGEND_MEMORY_GENERATION_BASE_URL`/`ASGEND_MEMORY_GENERATION_MODEL`, новые/изменённые файлы автоматически проходят decision metadata и title/description pipeline; API-ключи задаются через `ASGEND_MEMORY_DECISION_API_KEY` и `ASGEND_MEMORY_GENERATION_API_KEY`. Если настроены `ASGEND_MEMORY_EMBEDDING_BASE_URL` и `ASGEND_MEMORY_EMBEDDING_MODEL`, автоматизируются document/chunk embeddings, а retrieval использует hybrid search. Эти model stages хеш-ключевые и возобновляемые: неизменённые документы пропускаются, ошибка/отложенная попытка сохраняется. Удалённые endpoints с ключами должны быть HTTPS; без endpoints нет скрытых модельных вызовов.

`/api/resources` отражает режим через `selection_mode: "local_bm25"`, `resource_index_auto_sync: true` и `classification_parallel: false`. `instructions/select_*.md` и существующие настройки совместимости сохраняются, но не используются в обычном выборе ресурсов через decision-модель. `formalization[].resource_selection` показывает выбранные пути, `model_assisted: false`, `decision_model_calls: 0` и путь индекса.

## MCP

По умолчанию подключение MCP отключено:

```json
"mcp": {
  "servers": []
}
```

Чтобы подключить доверенный MCP-сервер, добавьте запись (пример команды условный; используйте свой исполняемый файл и абсолютный путь к скрипту):

```json
"mcp": {
  "servers": [
    {
      "name": "example",
      "command": "python3",
      "args": ["/opt/my-mcp/server.py"],
      "read_only_paths": ["/opt/my-mcp/resources"],
      "env": {"SERVICE_MODE": "readonly"},
      "timeout_seconds": 15
    }
  ]
}
```

После handshake инструмент с именем `lookup` публикуется как `mcp__example__lookup`. Добавьте **точный алиас** в `tools` только тем агентам, которым разрешено его использовать. Процессы MCP stdio запускаются с правами сервера; не подключайте неизвестные скрипты и бинарники. Дочерний процесс всегда получает очищенное окружение, даже если `sandbox.enabled` отключён: унаследованные секреты службы не передаются. Только явно перечисленные строковые пары в `mcp.servers[].env` добавляются в окружение конкретного stdio-сервера (не более 16 значений). `PATH`, `HOME`, `TMPDIR` и `LD_`/`DYLD_` переменные защищены и не могут переопределяться через эту настройку.

После успешного начального handshake ASGEND следит за дочерним stdio-процессом. При завершении процесса или транспортной ошибке pipe/response запускается восстановление с exponential backoff: 1, 2, 4, 8, 16, затем 30 секунд (максимальная задержка 30 секунд); повторные неудачные старты не завершают оркестратор. При штатном shutdown supervisor останавливается и присоединяется перед очисткой процесса. Каталог MCP alias/schema остаётся неизменным для работающих workers; изменения набора инструментов после рестарта сервера MCP будут применены при следующем рестарте ASGEND. Ошибка первоначального handshake всё ещё прерывает запуск, а Streamable HTTP MCP не управляется процессным supervisor.

### Streamable HTTP

Можно подключить MCP-сервер по современному Streamable HTTP (JSON или request-scoped SSE). Для remote endpoint используется HTTPS и проверка TLS; plain HTTP разрешён для loopback либо при явном `allow_insecure_http: true`:

```json
{
  "mcp": { "servers": [
    {
      "name": "remote_docs",
      "transport": "streamable_http",
      "url": "https://mcp.example.com/mcp",
      "bearer_token_env": "ASGEND_REMOTE_MCP_TOKEN",
      "timeout_seconds": 15
    }
  ] }
}
```

Секрет должен быть задан в окружении службы, например `ASGEND_REMOTE_MCP_TOKEN`. Если задано `bearer_token_env`, отсутствие или пустое значение секрета приводит к явной ошибке, а не к анонимному запросу. Для локального mock/development endpoint допустим `http://127.0.0.1:<port>/mcp`. Redirects не разрешаются, чтобы не передавать Authorization на другой адрес.

Оба транспорта поддерживают discovery, tools, resources/list/read/resource templates, prompts/list/get, completion и пагинацию `nextCursor`. HTTP транспорт проверяет `MCP-Protocol-Version`/`Mcp-Method`, использует `Mcp-Name` и schema annotation `x-mcp-header` для передачи отдельных аргументов через `Mcp-Param-*`. Значения, помеченные `x-mcp-header`, не включаются в JSON `arguments`. Это не является реализацией всех расширений/согласительных потоков текущего MCP: MRTR `input_required` retry, `subscriptions/listen` и OAuth discovery/token refresh пока не реализованы.

### LSP: диагностика после изменения файла

Добавьте language server для каждого набора расширений в `lsp.servers`; команда запускается напрямую, shell не используется. Пример для C/C++ (нужно установить `clangd`, заменить `root_dir` на существующую папку проекта):

```json
{
  "lsp": { "servers": [
    {
      "language": "c",
      "command": "clangd",
      "args": ["--background-index"],
      "root_dir": "/home/user/src/my-project",
      "extensions": [".c", ".h", ".cpp", ".hpp"],
      "timeout_seconds": 10
    }
  ] }
}
```

Для Python, Rust, TypeScript и других языков используйте соответствующий установленный LSP executable, его stdio-аргументы и непересекающийся список `extensions`. После MCP-инструмента, записавшего/изменившего файл, ASGEND запрашивает диагностику для изменённых файлов и добавляет сообщения/строки/диапазоны в результат инструмента для следующего хода модели. `lsp_diagnostics` позволяет агенту запросить диагностику явно. Автоматически исполнять исправления от LSP сервер не пытается.

## Планирование памяти и параллелизма моделей

Для управляемых локальных моделей контекст вычисляется из сериализованных сообщений/схем инструментов и кэшируется по размеру prompt. Дисковый размер весов измеряется рекурсивно и кэшируется до изменения времени модификации/периода проверки; GGUF metadata используется для оценки KV-cache bytes/token, с консервативной оценкой для неподдерживаемых форматов. Планировщик смотрит на `MemAvailable`, лимиты cgroup, PSS/RSS известных inference-процессов, свободную/общую VRAM и shared/UMA-режим. На UMA веса и KV cache считаются в общем системном пуле — отдельная VRAM не вычитается повторно.

Планировщик оставляет резерв для ОС/кэша, в первую очередь пытается выбрать максимальный параллелизм tier, затем уменьшает общий контекст до prompt-safe минимума. Текущие caps: `high`/`large`/`analysis`/`decision` — 1, `medium` — 2, `fast` — 3, `ultra-fast` — 4–8 в зависимости от доступных CPU (верхняя граница 8). Если веса и минимальный контекст не могут поместиться даже при освобождении прочих моделей, запуск завершается с диагностикой сразу, без ожидания полного timeout. Для удалённого/внешнего endpoint процессную память провайдера измерить невозможно; локальная оценка к нему не применяется.

## TCP/JSON протокол

Один UTF-8 JSON объект на соединение, заканчивается `\n`; сервер возвращает один JSON-объект и закрывает соединение. Примеры и описания полей — в [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

Проверка:

```bash
printf '{"op":"ping"}\n' | nc 127.0.0.1 8765
```

Постановка задачи:

```json
{"op":"submit","task":"Проверь архитектуру проекта","session_id":"demo","idempotency_key":"demo-001"}
```

Клиент опрашивает `status` по возвращённому `task_id`. Повторная отправка с тем же `idempotency_key` возвращает исходную задачу, а не создаёт дубликат. Если клиент не передал `auth_token` или он неверен, запрос отклоняется, когда токен настроен.

## Безопасность

- По умолчанию сервер слушает только loopback. При привязке к нелокальному интерфейсу `server.auth_token` обязателен.
- TCP-поток не шифруется. Не выставляйте порт напрямую в Интернет; используйте TLS-прокси, VPN или SSH-туннель.
- MCP запускается без shell с ресурсными лимитами, сбросом root-привилегий (если применимо), seccomp и Landlock в строгом режиме. Это снижает риск, но не равно контейнеру/VM; для недоверенного кода используйте отдельный OS-пользователь и внешнюю изоляцию. Добавляйте только необходимые абсолютные пути в `read_only_paths`.
- Модельные tool calls проходят allow-list конкретного агента.
- Размер, таймаут и число соединений ограничены конфигурацией; строки JSON проверяются на корректный UTF-8.
- Встроенное хранилище SQLite рассчитано на один экземпляр сервера, использующий свой файл БД; для нескольких реплик нужна другая схема координации очереди.

## Структура проекта

```text
include/runtime.h            общие структуры runtime/orchestrator
include/asgend_memory.h      публичный API библиотеки проектной памяти
src/server.c                 TCP + Unix socket + optional HTTP UI, connection limits, API и worker-пул
src/db.c                     SQLite priority queue with aging, retries, cancellation, recovery, sessions
src/orchestrator.c           выбор агента, decomposition loop, formalization loop
src/agent.c                  агентский цикл и tool calling
src/llm.c                    HTTP-клиент OpenAI-compatible API с привязкой к профилям моделей
src/tools.c                  встроенные tools и локальный поиск знаний
src/mcp.c                    lifecycle MCP stdio/JSON-RPC, supervisor restart/backoff и Linux sandbox
src/config.c, src/agents.c   загрузка конфигурации и реестра агентов
src/models.c                 управление локальными процессами LLM/decision-моделей и health checks
src/config_editor.c          валидация, синхронизация и безопасная запись файлов конфигурации
src/net.c                    единая инициализация libcurl
src/memory/index.c           рекурсивное сканирование, SHA-256, SQLite state/snapshots/events и миграции
src/memory/extract.c         безопасное UTF-8-извлечение и read-only OKF frontmatter parser
clients/asgend-memory.c      CLI памяти: scan/verify/extract/status/stats/gc/events
config/                      единая папка основной конфигурации, реестра моделей и профилей агентов
skills/                      инструкции для конкретных классов задач
knowledge/                   локальные справочные материалы
tests/integration.py         TCP/Unix/UI, priority scheduling, restart recovery, cancellation, managed models, config editing, LLM/MCP supervision/sandbox/context tests
tests/test_memory_index.py   scan/hash, extract/OKF, миграции, rename/delete, symlink/size guards and DB integrity
```

Архитектура: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). План и границы реализации: [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md).


## Установка на Linux / Arch Linux

Установщик собирает ядро и C memory index, запускает CTest, устанавливает бинарники `asgend-orchestrator`, `asgend-cli`, `asgend-memory` и CLI в `~/.local/bin`, копирует отсутствующие профили/ресурсы в `~/.config/asgend` и проверяет конфигурацию. Существующие конфиги и Markdown-файлы не перезаписываются. В частности, он согласует пути агентов между старыми раскладками `config/agents` и `orchestrator/agents`.

```bash
# Arch Linux: установите зависимости сборки, если они ещё отсутствуют
sudo pacman -S --needed base-devel cmake pkgconf json-c curl sqlite python
# Распакуйте архив, перейдите в каталог проекта и выполните
./install.sh
```

Установщик выводит сгенерированный `ASGEND_CORE_TOKEN`; сохраните его в менеджере секретов. Входящий TCP-порт по умолчанию `127.0.0.1:8765`, а web UI отключён. Не открывайте TCP API напрямую в Интернет: для Android/LAN используйте VPN или SSH-туннель и ограничьте firewall.

## Дорожная карта больших подсистем

План ASGEND 1.0 и незакрытые пункты находятся в [`TODO.md`](TODO.md) и [`docs/ROADMAP.md`](docs/ROADMAP.md). Исходники Control Plane, Execution Node agent, Qt 6 и Flutter-клиентов уже находятся в `server/`, `node_agent/`, `clients/asgend_qt/` и `clients/asgend_flutter/`; наличие исходников не означает, что production packaging или Android SDK-сборка проверены. Нормативный удалённый API опубликован в [`contracts/v1/openapi.yaml`](contracts/v1/openapi.yaml), схемы и примеры — в [`contracts/v1/`](contracts/v1/), поведенческие инварианты и state machine — в [`docs/API_CONTRACT.md`](docs/API_CONTRACT.md) и [`contracts/v1/state_machine.json`](contracts/v1/state_machine.json). Решения по границе Control/Execution Plane, безопасности и хранению записаны в [`docs/adr/`](docs/adr/); deployment-граничные условия — в [`docs/REMOTE_CONTROL_PLANE.md`](docs/REMOTE_CONTROL_PLANE.md). Compose production-пример, TLS reverse-proxy, миграции и процедуры резервного копирования описаны в [`server/DEPLOYMENT.md`](server/DEPLOYMENT.md); Docker runtime и PostgreSQL restore drill требуется выполнить в целевой среде. По памяти: scan/hash state и UTF-8/OKF extraction реализованы в 0.9.0–0.9.1; decision metadata/title/description — в 0.9.2; embeddings, FTS5/BM25 и hybrid query — в 0.9.3; graph expansion и automatic retrieval injection — в 0.9.4; chunk retrieval, metadata filters и token-budget packing — в 0.9.5; chunk embeddings/query intent — в 0.9.6; constrained reranking/evaluation — в 0.9.7; persistent LSH16 ANN — в 0.9.8; resource/project memory auto-sync — в 0.9.9; memory stats/gc с проверкой stale chunks/vectors — в предыдущей итерации; в предыдущей итерации подключён PDF extraction, а в текущей — DOCX/XLSX/PPTX OOXML extraction с ZIP/XML лимитами и smoke-тестами. Query planning теперь имеет явную CLI-реализацию `query --plan`; оценка его качества на большом размеченном корпусе, production packaging/deployment, Android build и final 1.0 end-to-end acceptance остаются открытыми и перечислены в TODO.

## Клиенты: ядро, CLI, Arch Linux и Android

Ядро ASGEND не зависит от веб-интерфейса. Клиенты используют line-delimited JSON TCP API (`ping`, `health`, `submit`, `list_tasks`, `status`, `events`, `answer`, `cancel`, `models`, `config_get`, legacy `config_save`, безопасный allow-listed `config_update_settings` и операции управления моделью). UI — один из клиентов, и его можно не запускать. См. [`docs/CLIENTS.md`](docs/CLIENTS.md).

Интерактивный CLI реализован на C (`clients/asgend_cli.c`) и устанавливается как `asgend-cli`; запуск без аргументов открывает меню задач/чата. Кроссплатформенный Flutter-клиент находится в `clients/asgend_flutter/`: общие исходники для Linux desktop и Android, экран задач/чата, диалог вопросов, события ядра, Nodes/модели и профили гибких агентов. Чтобы подготовить нативные platform scaffolds, установите Flutter SDK и выполните инструкции из `clients/asgend_flutter/README.md`. В текущей среде Flutter SDK не установлен, поэтому исходники UI добавлены, но Linux/Android бинарники не собирались здесь.


### Qt desktop client status (iteration 8)

`clients/asgend_qt/` now contains a Qt Widgets task-workspace MVP over the local core and remote HTTPS Control Plane adapters, Qt core tests, Linux desktop metadata, CPack TGZ/DEB/RPM configuration, and an Ubuntu CI workflow. This is not yet a verified binary release: Qt 6 development packages are not installed in the current environment, so local C++ compilation and GUI runtime tests remain open. Details: [`clients/asgend_qt/README.md`](clients/asgend_qt/README.md).

### Flutter/Android client

Android security scaffold, secure-token migration tests and the Flutter APK CI workflow are in `clients/asgend_flutter/`. The remote Control Plane screen uses `RemoteSession`/`EventFollower` for pairing, workspace switching, task/event operations, role-gated mutations, logout/revoke and SHA-256-verified artifact export. Local and remote task detail pages share the same adaptive Agent workspace: three TUI-inspired panes at widths ≥1200 logical pixels (task navigation, conversation/details, agent workspace), full-width details with an expandable artifact list on tablets and a draggable artifact preview sheet on phones. The local Flutter Cowork tab records mono 16 kHz WAV to the separate LFM3 voice gateway, retains/retries failed clips, accepts the report revision before queue handoff and can play an optional speech reply. Its task preview toolbar captures the actual Flutter-rendered artifact preview as a bounded PNG and opens the native share sheet; it does not capture other apps or the Android system screen. Gateway credentials are stored separately from the core token and Android requires HTTPS. Native Flutter voice/screenshot setup and constraints are documented in [`docs/FLUTTER_NATIVE_COWORK.md`](docs/FLUTTER_NATIVE_COWORK.md). The preview follows the source file SHA-256 so stale screenshots are not shown after a new artifact revision. Office/PDF/HTML/UI artifacts use generated PNG previews; code can be viewed as source, and images can be zoomed. `RemoteHomePage` accepts injectable test dependencies; widget tests cover viewer read-only behavior, pairing validation, offline errors and logout. Flutter SDK is not installed in this implementation environment, so `flutter analyze`, widget tests and APK build remain delegated to the pinned Flutter 3.29.3 CI workflow.

### Сквозная проверка compiled C core + Control Plane + node-agent

После Release-сборки можно выполнить отдельный быстрый acceptance-тест реального
C JSON API вместе с настоящим Python node-agent и HTTP Control Plane:

```sh
ctest --test-dir build --output-on-failure -R '^node_agent_c_core_e2e$'
```

Тест использует изолированную SQLite-базу без LLM-провайдера; он проверяет доставку
команд, состояние/события задачи, чтение артефакта из рабочей директории C core,
запрет публикации symlink и round-trip артефакта с SHA-256. Это не заменяет
PostgreSQL migration/restart и backup/restore acceptance.
### Live PostgreSQL acceptance (CI)

The Control Plane workflow provisions PostgreSQL 16 and runs schema-isolated
acceptance for schema upgrade/idempotency, durable command state across HTTP
server restarts, and expired-lease redelivery. It also runs
`server/tests/test_postgres_backup_restore.py` with PostgreSQL client tools: the
test creates disposable source/target databases, performs a real `pg_dump` and
`pg_restore`, and verifies restored rows plus content-addressed artifact bytes.
Local runs require `psycopg[binary]`, `pg_dump`/`pg_restore`, and a disposable
`ASGEND_TEST_POSTGRES_URL` whose role can create databases. These tests create and
drop their own temporary schema/databases and must not target production. The live
PostgreSQL CI job has not been executed from this environment; the restore test is
skipped locally when its prerequisites are absent.


## Security and operations smoke tests

See [`docs/SECURITY_OPERATIONS.md`](docs/SECURITY_OPERATIONS.md) for automated rate-limit/quota/log-redaction regressions and the staging HTTP probe. Use a dedicated low-privilege token through `ASGEND_CP_LOAD_TEST_TOKEN`; the probe records status and latency statistics but does not claim production capacity. Multi-replica rate-limit strategy and production operations review remain release gates.

## Native Android client plan

A separate Kotlin/Jetpack Compose native client is now scaffolded under `clients/asgend_android`; it does not replace `clients/asgend_flutter`. Audit, decomposition, and staged implementation acceptance criteria are tracked in [`docs/ANDROID_NATIVE_AUDIT.md`](docs/ANDROID_NATIVE_AUDIT.md), [`docs/ANDROID_NATIVE_DECOMPOSITION.md`](docs/ANDROID_NATIVE_DECOMPOSITION.md), and [`docs/ANDROID_NATIVE_TODO.md`](docs/ANDROID_NATIVE_TODO.md). The target is Android 17 / API 37 and Material 3 Expressive. The initial native shell, secure token store and bounded TCP client are source-implemented; APK/device acceptance requires Android SDK 37 and CI/emulator verification.


## Native Android client status

A parallel Kotlin/Jetpack Compose client lives at `clients/asgend_android`, targeting Android 17/API 37 with Material 3 Expressive. Stage 1 audit/spec, Stage 2 Android foundation, Stage 3 local task/chat/plan/files core workflows, Stage 4A runtime/models/agents/resources/MCP controls, and Stage 4B guided safe configuration are implemented in source. HTTPS Control Plane parity and final platform/accessibility acceptance remain tracked in `docs/ANDROID_NATIVE_TODO.md`. The authoring environment lacks Android SDK/Gradle; do not treat this source snapshot as an APK build verified until the Android CI workflow succeeds.


Native Android implementation status (2026-10-25): see `docs/ANDROID_NATIVE_AUDIT_FINAL.md`, `docs/ANDROID_NATIVE_AUDIT_TODO.md`, and `docs/ANDROID_NATIVE_IMPLEMENTATION_STAGE_7.md`. Stage 7 source fixes cover durable remote idempotency, remote state recovery, atomic workspace switching, fail-closed credential cleanup and response API-version validation. Android Gradle/APK/API37 device acceptance is still blocked/not run in the authoring environment.


## Current project audit (2026-10-25)

Current audit evidence and open work are tracked in `docs/PROJECT_AUDIT_ITERATION_1_2026-10-25.md`, `docs/PROJECT_AUDIT_ITERATION_2_2026-10-25.md`, `docs/PROJECT_AUDIT_ITERATION_3_2026-10-25.md`, `docs/PROJECT_AUDIT_STAGE_12_CLIENT_CI_TRIGGERS.md`, `docs/PROJECT_AUDIT_TODO_2026-10-25.md`, and `docs/PROJECT_REVIEW_DECOMPOSITION_2026-10-25.md`. Historical implementation-status files remain historical records; current verification state is described in the audit documents.

## Current cross-project audit (2026-10-26)

The latest audit continuation is documented in [`docs/PROJECT_AUDIT_FINAL_2026-10-26.md`](docs/PROJECT_AUDIT_FINAL_2026-10-26.md), with the Stage 13 API/OpenAPI/client-contract result in [`docs/PROJECT_AUDIT_STAGE_13_API_CONTRACT_PARITY.md`](docs/PROJECT_AUDIT_STAGE_13_API_CONTRACT_PARITY.md), the decomposed review in [`docs/PROJECT_REVIEW_DECOMPOSITION_2026-10-26.md`](docs/PROJECT_REVIEW_DECOMPOSITION_2026-10-26.md), and the prioritized list in [`docs/PROJECT_AUDIT_TODO_2026-10-26.md`](docs/PROJECT_AUDIT_TODO_2026-10-26.md). The report distinguishes source-level passes from hosted CI and platform acceptance that have not been run here.

## Current cross-project audit (2026-10-27, Stage 15)

Stage 15 builds on the 33-operation Core RPC inventory from Stage 14 and adds live request/response contract tests over the TCP JSON-line listener. `docs/PROTOCOL.md` now records required fields and representative success response shapes for all 33 operations; CTest target `core_rpc_runtime_contract` validates representative responses and missing/type-invalid requests. The complete local Release build and CTest passed **42/42 tests** in 222.46 seconds (exit code 0). See [`docs/PROJECT_AUDIT_STAGE_15_CORE_RPC_SEMANTICS.md`](docs/PROJECT_AUDIT_STAGE_15_CORE_RPC_SEMANTICS.md), [`docs/PROJECT_AUDIT_FINAL_2026-10-27.md`](docs/PROJECT_AUDIT_FINAL_2026-10-27.md), [`docs/PROJECT_AUDIT_ITERATION_3_2026-10-27.md`](docs/PROJECT_AUDIT_ITERATION_3_2026-10-27.md), and [`docs/PROJECT_AUDIT_TODO_2026-10-26.md`](docs/PROJECT_AUDIT_TODO_2026-10-26.md). Core TCP/Unix RPC remains separate from browser HTTP and Control Plane HTTPS; hosted CI and platform UI acceptance are not inferred from local test results.

Stage 14 inventory details remain available in [`docs/PROJECT_AUDIT_STAGE_14_CORE_RPC_PARITY.md`](docs/PROJECT_AUDIT_STAGE_14_CORE_RPC_PARITY.md) and [`docs/PROJECT_AUDIT_FINAL_2026-10-26.md`](docs/PROJECT_AUDIT_FINAL_2026-10-26.md).
