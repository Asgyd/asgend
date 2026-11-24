# Итерация 2 — устранение шума harness и повторяемость (2026-10-28)

## Исправления

- `wait_all` переписан на bounded concurrent polling, чтобы завершение независимых задач не измерялось последовательно.
- Cancellation mock handler теперь подавляет только ожидаемые socket reset/broken-pipe ошибки, создаваемые самим сценарием отмены; остальные ошибки benchmark отражаются в счётчиках.
- Добавлены фазовые отметки выполнения, строгие ожидания `answer → completed`, `cancel → cancelled`, `retry_task → completed` и фиксированная граница каждой stateful-фазы.
- Raw samples содержат только operation/concurrency/run/latency/ok; task IDs, request payloads, auth token и RPC response bodies вырезаются.

## Результат

Первый завершённый полный профиль собрал 2 700 samples, 100 на каждую пару method/concurrency, на 15 независимых процессах/SQLite базах. Все операции прошли без RPC ошибок.

## Оставшийся review finding

Критическая сверка JSON выявила неточность: `run_started_utc` записывался при формировании итогового объекта, то есть почти в момент завершения. Это не влияет на latency values, но портит отчётность и воспроизводимость; исправление перенесено в следующую итерацию вместе с инвентаризацией compiler/cgroup metadata.
