# Приоритетный TODO — повторный аудит производительности

Критерий закрытия задачи — артефакт с командой запуска, окружением, размерами корпуса/concurrency, повторениями, latency distribution, error rate и критерием качества. Одной зелёной функциональной проверкой performance-задача не закрывается.

## P1 — обязательные измерения до заявления production performance

- [ ] **Core RPC/task lifecycle:** добавить deterministic нагрузочный сценарий на submit/status/list/events/answer/cancel/retry; минимум 1/4/16 конкурирующих клиентов, измерять method p50/p95/p99 (p95 минимум 20, p99 минимум 100 samples), submit-to-terminal, error/timeout rate. Условие приёмки: машиночитаемый JSON + baseline из 5 сопоставимых запусков.
- [ ] **SQLite/artifacts:** корпус 1k/10k/100k rows, read-heavy и write-heavy профили; записать transaction latency, busy/lock waits, error rate и `EXPLAIN QUERY PLAN`; отдельно проверить индексный план и transaction correctness.
- [ ] **Memory scan/index:** измерить cold full scan, unchanged incremental и one-file-delta на 1k/10k файлов; по 3 corpus samples и 5 повторах; вывести files/s, MiB/s, total time и peak RSS.
- [ ] **Extractors:** отдельные Markdown/text/PDF/OOXML группы с малым/типовым/крупным файлом; вывести MiB/s, files/s, peak RSS и timeout/error distribution.
- [ ] **Embedding pipeline:** worker sweep 1/2/4/8 с deterministic mock для CPU/DB isolation и отдельный ограниченный live-provider run только с разрешёнными credentials; вывести throughput, p95, retries, throttling и DB contention.
- [ ] **ANN quality/performance:** создать labelled relevance dataset; sweep corpus sizes и candidate budgets; сравнить Recall@K/MRR/nDCG вместе с latency. Не ставить performance gate, пока Recall@K не достигает согласованного порога.
- [ ] **Control Plane/PostgreSQL:** поднять disposable live stack; нагрузить task list/submit/events/cancel/retry и artifact transfer при concurrency 1/8/32/64; записать p50/p95/p99, response size, pool wait, SQL plans, row locks и error rate. Local mock не засчитывать.

## P2 — client/runtime performance

- [ ] **MCP/LSP:** разложить latency на spawn, handshake, discovery, invoke и teardown; добавить cold/warm, worker contention и cleanup overhead.
- [ ] **LLM/SSE:** deterministic local fixtures для first-token/inter-token/completion, slow reader, cancellation и error case; затем отдельно authorized provider benchmark.
- [ ] **Node Agent:** эмулировать растущий fleet; измерить heartbeat/poll rate, retries, reconnect storm и recovery time.
- [ ] **Native Android:** pinned emulator/device; cold startup, scrolling jank/frame time, memory, network-per-active-minute и lifecycle reconnect.
- [ ] **Flutter:** profile mode для 100/1,000 tasks; build/raster time, poll request rate, inactive behavior, error retry/backoff.
- [ ] **Qt:** `QElapsedTimer`/Qt Test на больших task/artifact lists, repaint, model updates, memory growth.
- [ ] **CLI/TUI/Web:** throughput и latency на long-stream responses, ANSI redraw, slow consumer и reconnect.

## P2 — performance governance

- [ ] Зафиксировать runner, CPU quota, RAM, kernel, compiler, CMake, dependency versions, commit SHA, corpus hash и command line для каждого benchmark.
- [ ] Собирать минимум 5 сравнимых запусков на стабильном runner; хранить все raw samples, median/range и p95/p99 только при достаточном количестве samples.
- [ ] Разделить cold/warm run, endpoint/network time и pure search-kernel time; не объединять их в один график.
- [ ] Добавить CI artefacts + trend comparison, но не hard-fail по абсолютным миллисекундам до измеренной variance и договорённого tolerance.

## P3 — изменения кода только после профилирования

- [ ] Профилировать `src/memory/embedding.c::find_hit` и graph/chunk dedup на растущих candidate sets; переписывать на hash/index только при доказанном ускорении и идентичной выдаче.
- [ ] Оценить SQLite индексы через `EXPLAIN QUERY PLAN` и workload, не по названию запроса.
- [ ] Пересматривать Flutter polling интервалы только после связи request rate с UX/staleness и проверкой reconnect/error scenarios.

## Закрыто этим повторным аудитом

- [x] Свежий clean Release build и полный CTest baseline.
- [x] Повторно сняты длительности основных долгих tests.
- [x] Исправлена статистическая отчётность: хвостовые процентили скрыты для малых выборок.
- [x] Source/config/structured data inventory разделены.
- [x] ANN path Recall@K использует знаменатель уникальных exact paths.
- [x] Inventory пропускает распространённые generated/environment directories.
- [x] Финальный чистый Release configure/build выполнен; build wall time 9,02 с.
- [x] Повторный полный CTest на финальном дереве: 48/48 PASS, 256,35 с, exit 0.
- [x] Generated CTest metadata сохранены; все 48/48 тестов имеют положительные конечные timeout.
- [x] Установочная smoke-проверка подтвердила запуск `asgend-memory-benchmark-vectors --help` и установку `performance_metrics.py`.
- [x] Три финальных ANN запуска сохранены в JSON с raw samples/time logs; p95/p99 не публикуются для n=5.
