# Core RPC / Task Lifecycle — нагрузочный профиль

Дата измерений: 28 октября 2026. Это локальный диагностический baseline, не production SLA.

## Методика

- 5 независимых запусков на каждом уровне concurrency: 1, 4 и 16 TCP-клиентов.
- 20 samples на метод и запуск; суммарно 100 samples на метод/уровень и 2 700 raw samples.
- Свежий процесс Core и отдельная временная SQLite-база для каждого запуска (всего 15 процессов/баз).
- Локальный deterministic mock LLM, только loopback. Длительность пользовательского провайдера, WAN, Control Plane или PostgreSQL не измерялась.
- На измеряемый RPC включены TCP connect, JSON encode/decode, dispatch, сериализация ответа и закрытие соединения. `submit_to_terminal` включает submit плюс опрос статуса до terminal state.
- `answer` предварительно подготовлен в `waiting_input`; `cancel` включает активные и queued задачи и проверяет active cancellation fixture; `retry_task` использует засеянные dead-letter записи. Setup операций в latency не входит.
- p50/p95/p99 взяты из объединённых raw samples пяти запусков. Все 100 samples валидны для p95/p99 согласно общей статистической политике.

## Окружение

| Параметр | Значение |
|---|---|
| ОС / kernel | `Linux-6.18.55-x86_64-with-glibc2.41` |
| Архитектура | `x86_64` |
| Python | `3.13.5` |
| C compiler | `cc (Debian 14.2.0-19) 14.2.0` |
| CMake | `cmake version 3.31.6` |
| Конфигурация | `Release`, `-O3 -DNDEBUG` |
| CPU | 3 logical CPUs; cgroup quota `200000 100000` (~2 CPU cores) |
| Ограничение памяти cgroup | `4294967296` bytes (4 GiB) |
| Max connections / queue | 24 / 100 |
| Core binary SHA-256 | `0a3987d7b86dedadbfb760a4929f4e73b4c429286c417eb366259936807e7fdc` |
| Benchmark script SHA-256 | `756a9f4ae3059277c6c9d36c69124ec203ea8bcb62e2b0e6079606b44b5564b8` |

## Latency по операциям

Время — миллисекунды; RPS — агрегированная скорость метода в рамках повторов. `submit_to_terminal` отражает полный lifecycle с polling и показан отдельно, шкала сильно отличается от отдельных RPC-вызовов.

### Concurrency 1

| Метод | Samples | p50 ms | p95 ms | p99 ms | max ms | Aggregate req/s | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| `ping` | 100 | 0.115 | 0.289 | 0.334 | 2.813 | 5244.664 | 0 |
| `submit` | 100 | 1.823 | 3.256 | 4.847 | 5.259 | 467.495 | 0 |
| `status` | 100 | 0.717 | 2.037 | 2.975 | 3.727 | 1178.981 | 0 |
| `list_tasks` | 100 | 0.825 | 3.140 | 4.800 | 4.995 | 880.964 | 0 |
| `events` | 100 | 0.267 | 1.354 | 2.211 | 2.467 | 2126.302 | 0 |
| `answer` | 100 | 1.334 | 2.700 | 3.781 | 16.006 | 614.621 | 0 |
| `cancel` | 100 | 0.761 | 1.232 | 1.684 | 2.021 | 1203.514 | 0 |
| `retry_task` | 100 | 1.123 | 2.442 | 2.920 | 3.837 | 767.430 | 0 |
| `submit_to_terminal` | 100 | 154.164 | 208.629 | 2943.807 | 2999.363 | 3.393 | 0 |

### Concurrency 4

| Метод | Samples | p50 ms | p95 ms | p99 ms | max ms | Aggregate req/s | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| `ping` | 100 | 0.452 | 1.127 | 2.016 | 2.019 | 5550.930 | 0 |
| `submit` | 100 | 6.982 | 16.088 | 24.072 | 24.688 | 453.190 | 0 |
| `status` | 100 | 3.273 | 8.438 | 13.525 | 14.114 | 841.893 | 0 |
| `list_tasks` | 100 | 3.475 | 8.280 | 9.060 | 9.412 | 811.425 | 0 |
| `events` | 100 | 4.638 | 13.834 | 18.810 | 26.291 | 643.849 | 0 |
| `answer` | 100 | 4.185 | 10.742 | 14.418 | 17.894 | 664.099 | 0 |
| `cancel` | 100 | 1.978 | 4.262 | 7.121 | 11.834 | 1461.198 | 0 |
| `retry_task` | 100 | 3.491 | 9.015 | 12.744 | 14.416 | 875.925 | 0 |
| `submit_to_terminal` | 100 | 310.259 | 1639.328 | 1741.658 | 1745.720 | 7.243 | 0 |

### Concurrency 16

| Метод | Samples | p50 ms | p95 ms | p99 ms | max ms | Aggregate req/s | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| `ping` | 100 | 1.062 | 2.366 | 3.069 | 4.099 | 4371.776 | 0 |
| `submit` | 100 | 18.968 | 48.459 | 61.539 | 74.830 | 319.039 | 0 |
| `status` | 100 | 9.523 | 30.693 | 35.169 | 49.555 | 553.149 | 0 |
| `list_tasks` | 100 | 10.662 | 28.928 | 32.110 | 46.872 | 485.873 | 0 |
| `events` | 100 | 8.000 | 34.238 | 42.141 | 43.519 | 518.371 | 0 |
| `answer` | 100 | 11.034 | 35.276 | 45.878 | 47.218 | 455.593 | 0 |
| `cancel` | 100 | 11.007 | 25.225 | 36.058 | 39.179 | 618.540 | 0 |
| `retry_task` | 100 | 9.767 | 31.528 | 34.099 | 34.456 | 553.073 | 0 |
| `submit_to_terminal` | 100 | 2389.547 | 2759.491 | 2797.657 | 2839.570 | 7.107 | 0 |

## Ключевые выводы

1. **На Core RPC request path есть нагрузочная деградация при 16 клиентах.** `submit` p99 вырос с 4,847 ms (concurrency 1) до 61,539 ms (concurrency 16), примерно в 12,7 раза. `events` p99 вырос с 2,211 до 42,141 ms (~19,1 раза), `answer` — с 3,781 до 45,878 ms (~12,1 раза).
2. **Throughput submit-to-terminal насыщается около concurrency 4 на этом окружении.** Агрегированная скорость выросла с 3,393 задач/с при concurrency 1 до 7,243 при concurrency 4, но при concurrency 16 была 7,107 задач/с; при этом median lifecycle latency увеличилась с 310 ms до 2,390 ms между concurrency 4 и 16.
3. **Ошибки не зарегистрированы:** 0 из 2 700 измеренных samples; все подготовленные answer/cancel/retry переходы дошли до ожидаемого состояния. Это не исключает ошибок за пределами данного сценария и mock.
4. Измерения не локализуют первопричину: возможны contention SQLite mutex/transactions, RPC connection/thread overhead, worker scheduling и синхронизация task queue. Нужен следующий профиль SQLite/DB wait/query plans и CPU profiling на том же workload.

## Ограничения

- Тестовая среда имеет 3 logical CPUs, но cgroup CPU quota `200000 100000` ограничивает процесс примерно двумя CPU cores; память cgroup ограничена 4 GiB.
- Mock LLM не воспроизводит latency, throttling или backpressure внешнего провайдера. Нет live PostgreSQL/Control Plane и реальных клиентских SDK.
- `submit_to_terminal` включает polling с шагом из общего helper; это end-to-end диагностическая латентность, не чистое CPU time и не ровно пользовательский first-token latency.
- Эти результаты подходят как воспроизводимый baseline для сравнения на таком же runner, но не как production capacity certification или абсолютный CI latency gate.

## Повторение

```bash
python3 scripts/benchmark_core_rpc_lifecycle.py build/agent-orchestrator "$PWD" \
  --runs 5 --samples-per-run 20 --concurrencies 1,4,16 \
  --terminal-timeout 25 \
  --output docs/PERFORMANCE_CORE_RPC_LIFECYCLE_BENCHMARK_2026-10-28.json
```

Полные индивидуальные измерения находятся в [`PERFORMANCE_CORE_RPC_LIFECYCLE_BENCHMARK_2026-10-28.json`](PERFORMANCE_CORE_RPC_LIFECYCLE_BENCHMARK_2026-10-28.json); тест нагрузки добавлен в CTest под именем `performance_core_rpc_lifecycle_smoke`.
