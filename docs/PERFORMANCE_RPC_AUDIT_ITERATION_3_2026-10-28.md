# Итерация 3 — финальная проверка метаданных и результатов (2026-10-28)

## Исправления и контроль

- Время начала каждого server/database run теперь записывается до создания fixtures; время завершения записывается отдельно. Проверка подтвердила `completed >= started` для всех 15 runs.
- Отчёт включает kernel/platform, Python, GCC и CMake identities, binary SHA-256, harness SHA-256, logical CPU count, cgroup CPU quota и memory limit.
- Финальный набор повторно снят на точной версии harness, включённой в архив: 5 независимых runs × 20 samples на concurrency 1/4/16.
- Проверено 2 700 raw samples, 15 run details, 9 методов на каждом concurrency level, 0 ошибок; p95/p99 присутствуют только по 100 samples.
- Измерительная выборка и raw JSON проверены на отсутствие токена и RPC response bodies.
- Новый smoke target прошёл, полный CTest финального дерева: 49/49 PASS за 250,11 с; все 49 тестов имеют положительный конечный timeout.

## Заключение

Core RPC lifecycle P1 закрыт в границах локальной diagnostic baseline. Данные показывают latency amplification и почти плоский submit-to-terminal throughput между concurrency 4 и 16. Причина пока не локализована; следующий P1 — SQLite/artifact workload с lock-wait и `EXPLAIN QUERY PLAN`, а не переписывание кода по одному benchmark-результату.
