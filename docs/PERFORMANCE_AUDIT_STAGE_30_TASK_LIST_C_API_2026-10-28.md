# Stage 30 — Task-list C API A/B benchmark (2026-10-28)

## Итог

Проверен действующий C API `db_list_tasks()` против test-only candidate `db_list_tasks_candidate()` из той же `src/db.c` на отдельных, детерминированных SQLite DB. Измеряется настоящий C runtime path: ожидание `Runtime.db_mutex`, подготовка SQL, `sqlite3_step`, формирование `json-c` объектов и JSON serialization после возврата API.

**Результат: функциональная эквивалентность доказана, но candidate не является универсальной оптимизацией. Индексы остаются только в benchmark fixture и не добавлены в production schema.** На 100k строк candidate в благоприятной `fixture_10pct_running` configuration ускоряется примерно в 65–70×, но при реалистичном `low_running_4` в 1.85–2.06× медленнее из-за обработки большой queued-полосы с динамическим aging priority.

## Проверка и воспроизводимость

- Release build успешен (GCC 14.2.0); свежий полный CTest: **53/53 PASS за 255,72 с**, ошибок 0, процесс `ctest` завершился штатно.
- Все 53 generated CTest metadata entries имеют положительный конечный timeout (10–300 с; integration 300 с; новый C API smoke 60 с).
- 12 конфигурационных ячеек: 1k/10k/100k задач × два распределения статусов × concurrency 1/4.
- На каждую ячейку выполнено 5 парных запусков, 20 samples на worker и 3 warm-up API calls на worker. Получено **60 paired runs и 6 000 raw API samples** (3 000 на baseline, 3 000 на candidate), 0 ошибок.
- Все пять парных сравнений в каждой ячейке подтвердили одинаковый упорядоченный JSON response baseline/candidate с публичным лимитом 200; сумма `ordered_result_json_equal` — 60/60.
- Каждая сторона отдельно использует собственный DB файл с тем же детерминированным task fixture. Порядок baseline/candidate чередуется.
- First API call на worker записывается отдельно, но явно **не называется OS cold-cache**: page cache не сбрасывается, worker стартуют с небольшим рассогласованием.
- Instrumentation компилируется только в test benchmark target под `ASGEND_DB_PERF_BENCHMARK`; production target собирается без instrumentation define. Production partial indexes не добавлены.

Команда повторения (из корня репозитория, после Release configure/build):

```bash
python3 scripts/benchmark_task_list_c_api.py \
  --binary <build>/test-task-list-c-api-benchmark \
  --sizes 1000,10000,100000 \
  --runs 5 --samples-per-worker 20 --concurrencies 1,4 \
  --limit 200 --output docs/PERFORMANCE_TASK_LIST_C_API_BENCHMARK_2026-10-28.json
```

## Результаты C API

В ячейках concurrency=1 имеется 100 samples на mode; concurrency=4 — 400 samples на mode. В таблице приведены медиана и p95 API latency (мс); speedup — baseline median / candidate median. Значение меньше 1× означает, что candidate медленнее. `throughput ratio` сравнивает измеренный wall-throughput candidate/baseline.

| Rows | Distribution (running count) | Concurrency | API median baseline / candidate (ms) | API p95 baseline / candidate (ms) | Median speedup | Throughput ratio |
|---:|---|---:|---:|---:|---:|---:|
| 1,000 | 10% running (103) | 1 | 1.090 / 0.708 | 1.485 / 0.991 | 1.54× | 1.39× |
| 1,000 | 10% running (103) | 4 | 4.092 / 2.643 | 9.178 / 4.632 | 1.55× | 1.56× |
| 1,000 | 4 running (4) | 1 | 1.043 / 0.833 | 1.279 / 1.040 | 1.25× | 1.17× |
| 1,000 | 4 running (4) | 4 | 4.136 / 3.174 | 9.723 / 6.138 | 1.30× | 1.28× |
| 10,000 | 10% running (1,003) | 1 | 3.372 / 0.358 | 4.293 / 0.523 | 9.42× | 5.69× |
| 10,000 | 10% running (1,003) | 4 | 13.491 / 1.034 | 17.005 / 4.917 | 13.05× | 6.62× |
| 10,000 | 4 running (4) | 1 | 3.469 / 2.668 | 3.958 / 3.524 | 1.30× | 1.24× |
| 10,000 | 4 running (4) | 4 | 13.952 / 11.398 | 17.215 / 22.727 | 1.22× | 1.20× |
| 100,000 | 10% running (10,003) | 1 | 25.494 / 0.391 | 28.536 / 0.578 | 65.20× | 37.55× |
| 100,000 | 10% running (10,003) | 4 | 98.153 / 1.406 | 127.976 / 4.433 | 69.81× | 45.99× |
| 100,000 | 4 running (4) | 1 | 26.449 / 54.551 | 29.946 / 65.682 | 0.48× | 0.50× |
| 100,000 | 4 running (4) | 4 | 109.780 / 202.642 | 153.494 / 241.707 | 0.54× | 0.55× |

## Критические сценарии при 100k строк

### Больше 200 running задач — candidate выигрывает

Для `fixture_10pct_running` имеется 10 003 `running` задачи при API limit 200. Так как running-band является первой сортировочной полосой, корректный ответ целиком состоит из первых 200 `running` rows, и candidate не обязан обращаться к queued/other полосам.

- Concurrency 1: API median **25,494 → 0,391 мс** (65,20×); p95 28,536 → 0,578 мс.
- Concurrency 4: API median **98,153 → 1,406 мс** (69,81×); p95 127,976 → 4,433 мс.
- При concurrency 4 median mutex wait **73,097 → 0,748 мс**, SQLite step median **23,909 → 0,115 мс**.

Это valid but skew-specific fast path, не типичный показатель для любого status distribution.

### Только 4 running задачи — candidate регрессирует

Для `low_running_4` candidate должен продолжать чтение queued rows, вычислять dynamic aging и сохранять единый глобальный порядок до лимита. Queued-band по-прежнему использует temp B-tree для сортировки.

- Concurrency 1: API median **26,449 → 54,551 мс**; p95 29,946 → 65,682 мс; throughput ratio 0,50×.
- Concurrency 4: API median **109,780 → 202,642 мс**; p95 153,494 → 241,707 мс; p99 205,536 → 366,415 мс; throughput ratio 0,55×.
- При concurrency 4 median mutex wait вырос **82,362 → 151,353 мс**, SQLite step median — **26,905 → 49,692 мс**. Это удлиняет critical section и повышает задержки конкурентных вызовов.

Этого достаточно, чтобы **не переносить candidate partial indexes в production**. Нужно разработать вариант, который не сортирует крупную queued-полосу целиком, и протестировать его против C reference на фактических статус-распределениях.

## План SQL и storage

Production query plan: `SCAN tasks` + `USE TEMP B-TREE FOR ORDER BY`. Candidate использует `idx_ab_running_order` и `idx_ab_other_order` для соответствующих полос; queued band использует `idx_tasks_queue_priority`, но вынужден делать `USE TEMP B-TREE FOR ORDER BY` из-за computed aging priority. Это объясняет разницу между двумя распределениями.

На 100k rows после checkpoint + `VACUUM`:

| Distribution | Baseline DB bytes | Candidate DB bytes | Extra bytes | Изменение полного файла |
|---|---:|---:|---:|---:|
| 10% running (10 003) | 28 987 392 | 32 882 688 | 3 895 296 | +13,44% |
| 4 running | 28 971 008 | 32 346 112 | 3 375 104 | +11,65% |

`dbstat` доступен в данном SQLite build. Storage цифры являются разностью между двумя DB fixture, каждая была checkpointed/VACUUMed; это размер DB на диске в измеренной fixture, не прогноз WAL size и не долговременный write amplification.

## Разложение времени

Instrumented C API выдаёт отдельно:

- `api_ms`: время `db_list_tasks` с mutex wait и созданием json-c объектов;
- `mutex_wait_ms`: ожидание `Runtime.db_mutex`;
- `prepare_ms`: SQLite prepare, для candidate суммируется между band statements;
- `sqlite_step_ms`: время внутри `sqlite3_step` (без C row mapping между вызовами);
- `json_mapping_ms`: перевод SQLite columns в `json-c` objects;
- `serialization_ms`: компактизация JSON после возврата API, вне API wall time.

Например, в `100k / low_running_4 / concurrency=4`, почти весь рост candidate сосредоточен в mutex wait и SQLite stepping, а не в JSON mapping/serialization: mapping median 0,245 → 0,250 мс, serialization 0,164 → 0,176 мс.

## Среда и hashes

- OS: Linux-6.18.55-x86_64-with-glibc2.41; GCC: gcc (Debian 14.2.0-19) 14.2.0; Python: 3.13.5; SQLite Python module: 3.46.1.
- cgroup CPU quota: `200000 100000` (~2 CPUs); RAM cap: 4 GiB.
- Candidate benchmark binary SHA-256: `cc02168e9bae582ac8d3aa7afa1429ece8d41a009f974f595bd637d0b0db254b`.
- `src/db.c`: `f7d26806a103e13f6069d27b66d635488a8eff83c56dd38db9a0ac68455b4718`.
- C benchmark harness (`tests/task_list_c_api_benchmark.c`): `fb3b89b3d40e13d1381751d30dfe8f6daf59388cd05644f9b93ff6c5bc711123`.
- Timing header (`include/db_perf_timing.h`, supplementary final provenance): `de80ae99533644198b5a9db74efcb27298294abd9585ef32dfac5ae46f288bd9`.
- metrics helper: `222352ee2db0f145628cb45b8dd56fdad7577d34e14cdd177429c7cec171f027`.
- benchmark script: `362a779ecda43a03436d2e86d1b0f3eae98bcc9d3f25fe537f7fd2195574e0f4`.
- Raw samples: `PERFORMANCE_TASK_LIST_C_API_BENCHMARK_2026-10-28.json`.

## Ограничения

1. Это локальный synthetic fixture benchmark, а не production DB trace; реальные пользователи могут иметь другое распределение статусов, размера JSON и конкурентных запросов.
2. Один C runtime process и общая SQLite DB; не измерены production networking, request parse, Node/CP integration, live LLM и remote database contention.
3. OS page cache не очищается, поэтому first-call statistic отдельно помечен как first-call-after-init, не cold-cache.
4. Кандидатный multi-band query семантически совпадает по JSON в испытанных фикстурах, но не имеет гарантии performance для любых workload.
5. Внешний сабагент не запускался: инструмент запуска сабагентов недоступен в текущей среде. Три последовательных критических прохода выполнены и описаны в отдельных iteration reports.

## Решение и следующий пункт

- Production: оставить текущую схему индексов; candidate partial indexes не переносить.
- P1: исследовать alternative queued query/aging-aware approach; добавить correctness proof на tie/aging boundary и realistic low-running distribution.
- P1: сопоставить с профилем Core RPC state transitions и настоящими production-like task status snapshots.
- После улучшения повторить C API benchmark, все 53 CTest и write/index-cost matrix Stage 29.
