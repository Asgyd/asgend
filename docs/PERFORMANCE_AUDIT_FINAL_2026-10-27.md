# ASGEND — итоговый аудит производительности проекта

**Дата:** 27 октября 2026
**Итерации аудита:** 3
**Тип результата:** локальные измерения + структурный аудит + контролируемые синтетические тесты. Это не аудит production-трафика и не device-profile.

## Executive summary

За три итерации создана воспроизводимая база производительности, улучшена методика exact/ANN измерения, добавлены проверки load-probe и исправлена точность инвентаризации workflow/тайм-аутов. Новые инструменты отделяют фактические измерения от статических предположений и от тестов на mock-сервисе.

**Главный вывод:** аудит не подтвердил ни одного P0-дефекта производительности. При этом проект пока не имеет достаточных данных для заявления production SLA: не измерены реальные API/PostgreSQL нагрузки, saturation embeddings, крупные сканы, fleet Node Agent и UI performance на устройствах. Поэтому оптимизация продуктового кода не выполнялась по предположению; найденные потенциальные горячие пути вынесены в P1 TODO для профилирования.

## Область и среда

- Linux x86-64, GCC 14.2.0, CMake 3.31.6, Python 3.13.5.
- 3 доступных CPU threads; около 5.8 GiB видимой RAM.
- CMake/Ninja Release; полный CTest выполняется последовательно и непривилегированным пользователем.
- Отсутствуют Docker/Podman, PostgreSQL service, Flutter/Dart, Gradle/Android adb/emulator, Qt/qmake, `perf` и Valgrind. Поэтому production DB/service, frame-time, CPU flamegraph и allocator-profile значения не выдумывались.

## Ключевые локальные измерения

| Измерение | Результат | Интерпретация |
|---|---:|---|
| Исходный clean Release configure | 5.00 с | Один sample из исходного дерева |
| Исходный clean Release build | 10.33 с, `--parallel 4`, peak RSS 81,608 KiB | Не сравнивать напрямую с финальной сборкой, где parallelism равен 3 |
| Финальный clean Release configure | 5.16 с, peak RSS 95,400 KiB | Одно измерение, без SLA-вывода |
| Финальный clean Release build | 11.36 с, `--parallel 3`, peak RSS 81,404 KiB | Разное число параллельных задач не позволяет утверждать регрессию/ускорение |
| Source inventory | 233 файла, 70,916 строк | Включает исходники и тесты; не является production LOC |
| Full final CTest | 48/48 PASS за 253.62 с, exit 0 | Serial `-j1`, unprivileged `nobody`; max RSS 130,196 KiB от GNU time process/child accounting |
| CTest inventory | 48 тестов | Generated CTest metadata подтверждают `TIMEOUT` у 48/48 тестов |
| ANN exact median end-to-end | 237.904 мс | 5 парных замеров, синтетический корпус 5,200 × 128 |
| ANN median end-to-end | 49.483 мс | Тот же синтетический корпус; включает CLI startup и localhost embedding round trip |
| ANN paired median speed-up | 4.831× | Синтетический микробенчмарк, не production SLO |
| ANN candidates | 251 / 5,200 (4.83%) | Существенное сокращение кандидатов |
| Path Recall@5 против exact | 0.60 | Не проходит в качестве доказательства качества на реальных данных; нужна labelled relevance set |
| Load-probe local mock | 200 запросов, 16 workers, 200/200 HTTP 200, 186.49 req/s | Только тест load-probe harness + Python localhost mock, не производительность ASGEND Control Plane |
| Load-probe mock latency | p50 4.13 мс, p95 37.20 мс, p99 1,063.02 мс | Длинный хвост показывает влияние scheduling/contention; единственный sample нельзя использовать для capacity/SLA |
| Длительные CTest | integration 135.56 с; SSE watchdog 39.59 с; MCP launcher 20.43 с | Это время тестов, не direct user-facing service latency |

Время ANN представляет **end-to-end local CLI query** и включает запуск процесса плюс HTTP round trip к локальному mock embedding endpoint. Пять парных samples дают предварительную оценку, а не надёжный p95/p99 для production. Путь поиска внутри CPU не был профилирован отдельно.

## Компонентный итог

| Часть проекта | Что подтверждено | Что неизвестно / следующий шаг | Оценка доказательств |
|---|---|---|---|
| C core / RPC / task lifecycle | Release build, integration, RPC runtime/state tests; CTest durations | Нужны latency percentiles по методам RPC и submit-to-terminal under concurrency | Частично измерено |
| SQLite / artifacts | Atomicity, fault injection, shutdown/artifact checks | Нет contention curve, lock wait и query plans на больших объёмах | Функционально проверено, performance не измерена |
| Memory vector search / ANN | Синтетический 5,200-vector exact/ANN paired benchmark | Recall@5 только 0.60 на synthetic path; нужна labelled corpus и sweep candidate budgets | Синтетический end-to-end sample |
| Memory scan / extraction | Функциональные тесты scan, PDF, OOXML | Нет files/s, MiB/s, cold/incremental split и per-format memory | Не измерено |
| Embedding pipeline | Текущие functional/mock checks | Нет worker-count saturation, DB contention, retry/error rate и provider latency | Не измерено |
| MCP / sandbox / LSP | Launcher, restart, lifecycle tests; общий launcher test timing | Spawn/handshake/tool discovery/invoke/teardown не разделены по времени; Landlock positive matrix зависит от runner syscall support | Частично измерено |
| LLM / SSE | Local watchdog timing и timeout/cancel/error tests | Нет remote first-token/inter-token distribution | Не измерено на provider |
| Control Plane / HTTP API | Load probe метрики реализованы, success/503, redaction и bounds протестированы на local mock | Нет живого ASGEND endpoint, PostgreSQL, pool wait, DB plans, real latency | Только harness validated |
| PostgreSQL | Acceptance/contract tests из исходного набора | Нет service runner и `EXPLAIN (ANALYZE, BUFFERS)` на representative dataset | Не измерено |
| Node Agent | C core E2E/contract tests | Нет multi-node heartbeat/poll/retry/recovery load curve | Не измерено |
| Flutter | Статический просмотр: task refresh every 3 s remote / 2 s local | Нет profile mode; не проверены frame time, list-scale, polling cost/error backoff | Статический риск |
| Android | Инвентаризация кода и CI workflows | Нет Gradle/adb/emulator для startup, jank, memory, battery/network measurements | Не измерено |
| Qt | Инвентаризация кода и CI workflows | Нет Qt/qmake для repaint/list update/perf tests | Не измерено |
| CLI/TUI/Web UI | Functional/layout/stream tests и inventory размера файлов | Нет long-stream throughput/redraw latency | Функционально проверено |
| Build / CI / test governance | Clean build, 48 tests; конечные timeout подтверждены по CTest JSON | Hosted CI run IDs/artifacts и multi-run benchmark history не доступны | Локально проверено |

## Наиболее важные findings

### P1 — измерения hot path отсутствуют

1. `src/memory/embedding.c`: `find_hit()` линейно ищет hit, а отдельные graph/chunk candidate dedup paths содержат вложенные проходы. Это возможный `O(N²)` риск при росте списка кандидатов, но пока нет профиля, доказывающего влияние. Следующий шаг — scaling benchmark/profile, затем hash-based rewrite только при подтверждённой выгоде и одинаковой выдаче.
2. Core task/RPC/SQLite не публикуют достаточно детальные latency distributions для метода, объёма данных и конкурентности. Добавить локальный deterministic workload с измерениями и query-plan output.
3. Scan/extraction/embedding pipeline не имеет throughput-матрицы по объёму/типу файла и числу workers.
4. Control Plane/PostgreSQL нужны реальные тесты под отдельным тестовым окружением; local mock не заменяет сервер.

### P2 — сервисы и клиенты

5. MCP общий CTest timing не раскладывает spawn, initialization/handshake, tool discovery и teardown.
6. SSE tests проверяют correctness/timeout, но не first-token/inter-token latency.
7. Flutter использует фиксированные интервалы polling; это статический кандидат на измерение, не доказанный дефект. Backoff менять только после измерения нагрузки/UX.
8. Android/Flutter/Qt потребуют соответствующих SDK и pinned device/emulator; функциональные CI-тесты не являются доказательством кадровой производительности.
9. CLI/TUI/Web streaming требуют throughput тестов на больших сообщениях и медленном потребителе.

### P2 — инфраструктура измерения

10. Инвентаризатор исправлен так, чтобы считать фактические workflow job IDs только в секции `jobs:` и различать explicit timeout от общего default. Все 48 tests в сгенерированном CTest JSON имеют timeout.
11. Control Plane load-probe теперь имеет локальные проверки success/failure, percentiles, error rate, input bounds и redaction. Production capacity run остаётся открытым.
12. Один baseline не годится для hard CI latency budgets. Нужны минимум пять сравнимых runs на стабильном runner и сохранение commit/toolchain/corpus/environment metadata.

## Три итерации аудита

1. **Iteration 1 — baseline/inventory:** осмотр компонентов и доступных инструментов; Release build; original 45-test CTest timing; первая декомпозиция; изолированы production gaps.
2. **Iteration 2 — benchmark methodology:** исправлен фиксированный порядок exact/ANN; добавлены warmup/repetitions/percentiles/paired speed-up; ANN performance и Recall@5 выводятся вместе; добавлен inventory regression.
3. **Iteration 3 — adversarial review:** исправлен YAML workflow job count parser; исправлено распознавание динамического CMake timeout; введён и проверен default timeout; добавлен локальный load-probe smoke/error test; обновлены component matrix и TODO.

Отчёты: `PERFORMANCE_AUDIT_ITERATION_1_2026-10-27.md`, `PERFORMANCE_AUDIT_ITERATION_2_2026-10-27.md`, `PERFORMANCE_AUDIT_ITERATION_3_2026-10-27.md`.

## Итог и ограничения

- Никакой production performance optimization не внесён без proof of benefit; изменения ограничены измерительным harness, тестами, CTest timeouts и документами.
- Числа localhost mock не описывают Control Plane performance.
- Synthetic ANN result имеет ограниченный sample size, end-to-end timing и недостаточную доказательность для релевантности.
- Hosted GitHub Actions, настоящий PostgreSQL, внешние LLM/embedding providers и UI device benchmark не запускались.
- Final full CTest log: `PERFORMANCE_CTEST_FINAL_LOG_2026-10-27.txt`; configured timeout evidence: `PERFORMANCE_CTEST_TIMEOUTS_2026-10-27.json`; structural inventory: `PERFORMANCE_INVENTORY_FINAL_2026-10-27.json`; environment/build record: `PERFORMANCE_FINAL_ENVIRONMENT_2026-10-27.txt`.
- Отдельный runtime для сабагента недоступен; отчёт не выдумывает внешнюю оценку 1–10. Вместо этого выполнены три документированные итерации критического ревью и добавлены негативные regression tests.

Следующий приоритет — P1: Core RPC/SQLite concurrency and query plan measurements; representative retrieval profiling/quality set; live Control Plane+PostgreSQL load; then P2 service/client profile work.
