# Итерация 1 — Core RPC load baseline (2026-10-28)

## Что сделано

- По performance TODO выбран следующий P1: измерение Core RPC/task lifecycle (`submit/status/list_tasks/events/answer/cancel/retry` и submit-to-terminal).
- Реализован `scripts/benchmark_core_rpc_lifecycle.py`: disposable Core TCP сервер, локальный deterministic mock LLM, отдельная SQLite база, concurrency 1/4/16, повторяемые входы и JSON с raw latency samples.
- Добавлен smoke-тест `tests/test_benchmark_core_rpc_lifecycle.py` и CTest регистрация `performance_core_rpc_lifecycle_smoke`.
- Smoke прошёл 1/1 за 4,17 с: все 9 операций, ноль ошибок, p95/p99 скрыты для малой выборки, токен и RPC response bodies не попадают в отчёт.

## Найденные проблемы методики

Первый полный запуск превысил командный лимит на раннем уровне concurrency и выдал ожидаемые `ConnectionResetError` tracebacks от mock handler после cancel. Незавершённый запуск не использовался как benchmark result. Анализ показал, что последовательное ожидание terminal state могло искусственно умножать latency нескольких задач, а стандартный HTTP handler шумел на штатных reset при отмене.

## Что оставлено открытым

Полные 5x20x3 измерения не объявлялись готовыми до устранения поведения harness и повторного запуска. Все ошибки setup/cancellation должны приводить к явному отказу benchmark, а не молча попадать в latency выборку.
