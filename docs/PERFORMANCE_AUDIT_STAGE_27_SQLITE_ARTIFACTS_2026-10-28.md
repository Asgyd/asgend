# Stage 27 — SQLite и artifacts performance audit

**Дата:** 2026-10-28  
**Статус:** локальный профиль завершён; изменение продукционного SQL не выполнялось без A/B доказательства.

## Результат этапа

- Новый воспроизводимый profiler `scripts/benchmark_sqlite_artifacts.py` добавлен в CTest через `performance_sqlite_artifacts_smoke`.
- Полный измерительный проход: **15 000 SQLite операций**, пять повторов на каждый размер/профиль/уровень конкуренции, размеры 1k/10k/100k, 0 ошибок.
- Проверены actual Core TCP RPC `artifact_list` и `workspace_read_bytes`: 0 ошибок, hash/size проверены, корректно работает лимит списка 1 000 объектов.
- Для каждого размера прошли `PRAGMA integrity_check=ok` и транзакционная rollback-проверка. Всего 3/3 integrity checks и 3/3 atomicity checks.
- Отдельный CTest target smoke прошёл **1/1**; полный финальный набор прошёл **50/50 за 255,69 секунды**, код завершения 0.
- Все **50/50** сгенерированных CTest metadata имеют положительный конечный timeout (диапазон 10–300 секунд); см. `PERFORMANCE_STAGE_27_CTEST_TIMEOUTS_2026-10-28.json`.
- Полный журнал: `PERFORMANCE_STAGE_27_CTEST_LOG_2026-10-28.txt`; generated CTest inventory: `PERFORMANCE_STAGE_27_CTEST_SHOW_ONLY_2026-10-28.json`.

## Существенные findings

### P1 — unfiltered task list сканирует таблицу и выполняет временную сортировку

`EXPLAIN QUERY PLAN` для запроса из `src/db.c::db_list_tasks` стабильно показывает `SCAN tasks` и `USE TEMP B-TREE FOR ORDER BY` на всех трёх размерах. На 100 000 строк медиана запроса `task_list_all` составила **21.616 мс** при concurrency=1 и **37.528 мс** при concurrency=4; p95 при concurrency=4 — **96.586 мс**.

### P1 — filtered `queued` list использует индекс, но всё равно сортирует

Запрос использует `idx_tasks_queue_priority (status=?)`, затем `USE TEMP B-TREE FOR ORDER BY`. Порядок учитывает выражение динамического priority aging; нельзя заменить его индексом без проверки эквивалентности сортировки, tie-break и aging semantics. На 100k rows p95 queued list при concurrency=4 составил **43.132 мс**.

### P2 — конкурентный write path показывает длинный хвост, но нет ошибок SQLite

На 100k rows, write-heavy, concurrency=4 `write_transaction` median/p95/p99: **0.325 / 3.704 / 34.264 мс**. p95 elapsed ожидания `BEGIN IMMEDIATE` — **0.249 мс**, p95 `COMMIT` — **1.887 мс**. Эти отдельные процентили нельзя складывать, и они не доказывают первопричину tail latency. Ошибок/`database is locked` не зарегистрировано.

## Следующие изменения

1. Сделать A/B прототип task-list query, который сохраняет effective-priority aging, status ordering, `created_at ASC` tie-break и ограничение `LIMIT`; проверять полное равенство упорядоченной выдачи старой/новой версии на fixture с aging, ties и всеми допустимыми статусами.
2. Сравнить старый/новый запрос на тех же DB files 1k/10k/100k, по 5 runs, concurrency 1/4, собрать per-operation p50/p95/p99 и total CPU. Не менять production SQL, пока корректность не подтверждена.
3. Отдельно профилировать write tail (commit, checkpoints, scheduling) до изменений `synchronous=FULL` или WAL policy. Не ослаблять durability ради цифр.

## Границы измерения

SQLite microbenchmark выполняет Python `sqlite3` calls по схеме, индексам и hot SQL shapes, зеркалирующим `src/db.c`. Он не включает C mutex, JSON serialization или весь TCP RPC путь; Core RPC lifecycle имеет отдельный benchmark предыдущего этапа. Artifact benchmark, напротив, вызывает фактический локальный Core TCP API и `src/artifacts.c`, но на локальной temp FS.

Окружение: Linux-6.18.55-x86_64-with-glibc2.41; GCC/CC `cc (Debian 14.2.0-19) 14.2.0`; CMake cmake version 3.31.6; SQLite 3.46.1; Python 3.13.5; cgroup CPU quota 2.0 cores, RAM limit 4 GiB. Полные hashes окружения и входных файлов находятся в raw JSON.

## Итерации критического ревью

- Итерация 1: сопоставлены schema/indexes/hot SQL с `src/db.c`, добавлены query plan assertions и atomicity/integrity checks.
- Итерация 2: проведён полный профиль 1k/10k/100k и actual Core artifact RPC. Найдены некорректные начальные метрики размера SQLite файла при WAL и aggregate throughput для параллельного чтения; оба расчёта исправлены до финального прогона.
- Итерация 3: добавлены source/binary/script SHA-256, версия CMake/компилятора, контрольные hash/size/MIME для артефактов; все метрики пересняты после последних правок.

Отдельный инструмент внешнего сабагента недоступен в этой среде; независимая внешняя оценка не заявляется. Выполнены три последовательных критических прохода с исправлением выявленных ошибок методики.

## Файлы доказательств

- `PERFORMANCE_SQLITE_ARTIFACTS_BENCHMARK_2026-10-28.md` — результаты и таблицы.
- `PERFORMANCE_SQLITE_ARTIFACTS_BENCHMARK_2026-10-28.json` — raw samples, query plans, hashes и metadata.
- `tests/test_benchmark_sqlite_artifacts.py` — CTest smoke/guard.
- `scripts/benchmark_sqlite_artifacts.py` — повторяемый profiler.
