# Stage 28 — Task-list SQL A/B и эквивалентность `db_list_tasks()`

**Дата:** 2026-10-28  
**Статус:** локальный A/B baseline завершён; candidate indexes не перенесены в production.

## Итог

- Создан `scripts/benchmark_task_list_ab.py`: сравнивает текущий query shape и изолированный трёхполосный вариант (`running` → `queued` → остальные статусы), использующий candidate partial indexes только во временной БД.
- Добавлен C reference harness `tests/task_list_reference.c`. Он вызывает настоящую `db_list_tasks()` из `src/db.c` на той же SQLite-базе; benchmark сверяет и baseline-модель, и candidate с C-ответами, а не только друг с другом.
- Для размеров 1k, 10k и 100k выполнены проверки всех 7 status filters плюс общий список, при limits 1/10/50/200 (для size 1k — 1/10/50/200). Получено 96 комбинаций filter/limit (32 на размер). 5 694 возвращённые строки были сопоставлены с реальным C reference; baseline и candidate совпали по порядку и публичным полям.
- Явно добавлен `id ASC` в оба production SQL ordering (`sql_all`, `sql_filter`) как третичный tie-break после priority/effective priority и `created_at`. До этого точный порядок строк с одинаковыми ключами был не определён SQL-контрактом и зависел от выбранного плана/индекса. Status rank, priority aging, `created_at ASC`, limit и формат ответа сохранены.
- Производительная часть candidate остаётся экспериментом. Два partial indexes (`running`, `other`) не добавлены в production-схему; их стоимость на writes, миграции, место на диске и cold-start ещё не измерены.

## Дизайн проверки

### Контроль корректности

- Схема и текущие индексы сверяются с `src/db.c`.
- Фикстуры включают все статусы, повторяющиеся значения priority/created_at/effective priority, недавние и старые queued tasks, а также aging saturation.
- На каждом размере сравниваются результаты `baseline SQL` → C `db_list_tasks()` и `candidate SQL` → C `db_list_tasks()` для каждой комбинации фильтра/limit.
- C reference harness строится из `tests/task_list_reference.c`, `src/db.c` и `src/config.c`; временная база disposable.
- Временной anchor для Python SQL зафиксирован на запуск бенчмарка, а created_at отстоит от границ aging bucket на 120–180 секунд. Это предотвращает переход на соседний bucket между Python и C вызовами.

### Измерения

- Размеры: 1 000 / 10 000 / 100 000 записей.
- 5 независимых runs для каждого размера и concurrency.
- Concurrency: 1 и 4; по 10 операций на worker в каждом run, один warm-up query на worker вне замера.
- 50 raw samples на метод при concurrency=1 и 200 при concurrency=4; p95 публикуется при ≥20 samples, p99 при ≥100.
- Чередование порядка baseline/candidate по run; сырые образцы сохранены в `PERFORMANCE_TASK_LIST_SQL_AB_2026-10-28.json`.
- Environment: Linux x86-64, Python 3.13.5, SQLite 3.46.1, 3 видимых CPU, cgroup CPU quota 2 cores, RAM limit 4 GiB.

## Результаты

| Строк | Concurrency | Baseline median / p95 / p99 (мс) | Candidate median / p95 / p99 (мс) | Отношение медиан baseline/candidate |
|---:|---:|---:|---:|---:|
| 1k | 1 | 1,454 / 3,573 / не публикуется | 0,860 / 1,147 / не публикуется | 1,69× |
| 1k | 4 | 7,301 / 23,081 / 28,080 | 8,353 / 22,501 / 31,743 | 0,87× (candidate median медленнее) |
| 10k | 1 | 4,203 / 5,634 / не публикуется | 0,541 / 0,714 / не публикуется | 7,77× |
| 10k | 4 | 9,327 / 27,617 / 36,871 | 10,238 / 29,051 / 37,776 | 0,91× (candidate median медленнее) |
| 100k | 1 | 28,738 / 32,414 / не публикуется | 0,580 / 1,105 / не публикуется | 49,55× |
| 100k | 4 | 77,728 / 101,028 / 124,609 | 8,038 / 27,814 / 36,445 | 9,67× |

**Интерпретация:** на 100k candidate существенно снижает median и tail latency в данном workload; на 1k и 10k при concurrency=4 он не показывает устойчивого выигрыша и иногда медленнее. На больших фикстурах >=200 running tasks заполняют весь LIMIT, позволяя candidate остановиться после индексного чтения running band. Это workload-dependent оптимизация, не универсальное ускорение всех распределений статусов.

## Query plan

- Baseline full list: `SCAN tasks` плюс `USE TEMP B-TREE FOR ORDER BY`.
- Candidate running band: использует `idx_ab_running_order`, без temporary sort.
- Candidate other-status band: использует `idx_ab_other_order`, без temporary sort.
- Candidate queued band всё ещё использует temporary sort, потому что effective priority зависит от `julianday('now')` и priority aging.

Эти `idx_ab_*` индексы созданы только в disposable benchmark DB. Их нельзя считать production-индексами.

## Решение

**Не переносить candidate partial indexes в production на этом этапе.** Причины: отсутствие write-heavy/storage измерений стоимости новых индексов; отсутствие второго реального профиля со skewed status distribution; ухудшение concurrency=4 median на 1k и 10k в этом synthetic profile; измерения кандидата используют Python `sqlite3` и годятся для сравнения SQL-форм, но не заменяют измерение C API latency в live workload.

Production change в этом этапе ограничен стабильностью результата: `id ASC` задан явно после текущих sort keys. Сам candidate — только A/B вариант.

## Артефакты и верификация

- Benchmark: `scripts/benchmark_task_list_ab.py`.
- C reference: `tests/task_list_reference.c`.
- Regression test: `tests/test_task_list_sql_ab.py`.
- Raw data: `docs/PERFORMANCE_TASK_LIST_SQL_AB_2026-10-28.json`.
- Release configure/build прошли; полный CTest Stage 28 завершился **51/51 PASS за 277,28 с, exit code 0**. Все 51 тест имеют положительный конечный timeout по generated metadata.
- Журнал: `PERFORMANCE_STAGE_28_CTEST_LOG_2026-10-28.txt`; timeout metadata: `PERFORMANCE_STAGE_28_CTEST_TIMEOUTS_2026-10-28.json`. Новый `performance_task_list_sql_ab_smoke` прошёл за 2,66 с.
