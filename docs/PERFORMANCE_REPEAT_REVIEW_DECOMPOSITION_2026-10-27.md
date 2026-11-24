# Декомпозиция повторного ревью производительности ASGEND

## Принцип интерпретации

Каждая область помечается одной из категорий: **измерено локально**, **проверена функционально/статически**, **не измерено в соответствующей среде**. Время CTest является длительностью тестового сценария, а не latency пользователя. Mock load-probe характеризует проверочный инструмент и localhost transport, а не производительность реального сервиса.

## Покрытие компонентов

| Подсистема | Что измерено/проверено | Нерешённый performance-вопрос | Статус / приоритет |
|---|---|---|---|
| Core C runtime / RPC | Release build, integration, RPC runtime/state tests, полный CTest | Нет p50/p95 по RPC методу, submit-to-terminal latency и конкурентной saturation curve | Частично измерено; P1 |
| SQLite / artifacts / durability | Atomicity, fault injection, artifact tests, shutdown сценарии | Нет representative database scale, lock-wait, transaction/query latency и EXPLAIN QUERY PLAN | Только функциональные доказательства; P1 |
| Memory scan / indexing | Smoke и pipeline tests | Нет холодного полного scan vs unchanged incremental scan и files/s/MiB/s на 1k/10k файлов | Не измерено; P1 |
| Text/PDF/OOXML extraction | PDF/OOXML тесты | Нет throughput и peak RSS по формату/размеру, крупных/повреждённых документов | Не измерено; P1 |
| Embedding generation | Mock-backed functional tests, worker option exists | Нет throughput/saturation 1/2/4/8 workers, retries, throttling и реальной provider latency | Mock/functional only; P1 |
| Vector search / ANN | 3 повторных synthetic runs: exact median 226,233–275,494 мс; ANN 49,019–62,174 мс; paired speedup 4,253×–5,036×; 251 candidates, path Recall@5 0,60 | 5 samples/mode per run, end-to-end startup+localhost mock, synthetic corpus; низкая релевантность не обосновывает порог ANN | Synthetic measured; P1 dataset quality |
| Graph/chunk/rerank | Functional graph/chunk/rerank tests; static candidate merge inspection | Нет масштабирования `find_hit`/merge, duplicate hit distribution и затрат reranker | Static risk; профилировать перед rewrite; P1 |
| MCP / sandbox / LSP | Lifecycle/restart tests, CTest duration | Нет decomposition spawn/handshake/tool discovery/invoke/teardown, нет multi-worker latency curve | Частично измерено; P2 |
| LLM / SSE | SSE watchdog ~40 с как тестовый сценарий; timeout/cancel checks | Нет настоящего first-token/inter-token latency под реальным provider | Не provider-measured; P1/P2 |
| Control Plane / HTTP | Load-probe harness: status/error/latency/redaction; local mock 200 запросов | Не запускался против живого сервиса и PostgreSQL; нет pool wait, SQL plan/lock timings | Harness validated; live unmeasured; P1 |
| PostgreSQL | Контракты/acceptance из проекта | Нет реального DB service с representative schema/data и EXPLAIN ANALYZE BUFFERS | Не измерено; P1 |
| Node Agent | Node Agent E2E/contract scenarios | Нет multi-node fleet scale, heartbeats/poll/reconnect/retry amplification | Функционально проверено; P2 |
| Flutter | Static source inventory and test/workflow checks | Нет profile build, 100/1,000 task view frame/raster timing и polling cost | Статический анализ; P2 |
| Native Android | Source/CI inventory | Нет emulator/device startup, jank/frame, memory/network-per-active-minute | Не измерено; P2, нужен SDK/device |
| Qt client | Source/CI inventory | Нет Qt Test/QElapsedTimer painting/model update/memory benchmarks | Не измерено; P2, нужен Qt SDK |
| CLI/TUI/Web UI | Functional/state/stream tests | Нет sustained streaming throughput/redraw latency и slow-consumer curve | Функционально проверено; P2 |
| Build/CI/packaging | Clean Release configure 4,59 с/build 9,02 с; final CTest 48/48 за 256,35 с; 48/48 finite timeouts; install smoke | Нет five-run clean-build variance, hosted artifacts/run IDs и platform performance runners | Local gate validated; P2 |

## Приоритеты работ

- **P1 — сначала:** live Core RPC/SQLite workload, memory scan/extraction throughput, embedding worker scaling, labelled retrieval-quality set и production-like Control Plane/PostgreSQL load.
- **P2 — следующий пакет:** разложение MCP/SSE latency, Node Agent fleet curve, UI profile benchmarks на pinned SDK/device, CLI/TUI/Web stream load.
- **P3 — только по профилю:** заменить линейную дедупликацию `find_hit`/graph/chunk merge, менять polling intervals или индексы SQL только после воспроизводимого профиля и функционального equivalence test.
