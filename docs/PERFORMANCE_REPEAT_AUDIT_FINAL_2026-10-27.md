# ASGEND — итоговый повторный аудит производительности

**Дата:** 27 октября 2026 г.  
**Циклы последовательного аудита:** 3  
**Входной архив:** `asgend-performance-audit-final-2026-10-27.tar.gz` (SHA-256 проверен).  
**Тип доказательств:** свежая чистая Release-сборка, повторный полный CTest, синтетический ANN benchmark с тремя независимыми запусками, статистические unit-тесты, структурная инвентаризация.

## Executive summary

Аудит не подтвердил критическую (P0) регрессию производительности в проверяемом локальном окружении. Свежее полное тестирование подтвердило корректность Release-сборки и прохождение CTest. Самая полезная находка повторного аудита — улучшение достоверности измерений: прежде малые выборки отображали максимум как p95/p99, компонентная инвентаризация смешивала source/config/JSON data, а Recall@K мог использовать знаменатель с количеством дубликатов.

Никакой production-алгоритм не менялся без доказанной выгоды. Исправлены средства измерений и инвентаризации. Главные незакрытые риски — отсутствие реальных нагрузочных профилей Core/SQLite/Control Plane, крупных scan/extraction корпусов, provider saturation и профилирования UI на устройствах. Эти пробелы перечислены в TODO как самостоятельные задачи с критериями приёмки.

## Итоговые локальные измерения

| Метрика | Итерация 1 | Итерация 2 | Итерация 3 |
|---|---:|---:|---:|
| Release configure | 4,98 с | 5,18 с | 4,59 с |
| Release build, parallelism 3 | 9,10 с | 10,15 с | 9,02 с |
| Полный CTest | 48/48, 252,91 с | 48/48, 250,73 с | 48/48, 256,35 с |
| Peak RSS configure | 95 400 KiB | 95 396 KiB | 95 444 KiB |
| Peak RSS build | 81 396 KiB | 81 612 KiB | 81 368 KiB |

Одного clean-build результата на каждый этап недостаточно, чтобы заявить об ускорении компиляции: это разные моменты запуска в общей среде. Полные исходные logs/timing сохранены в `PERFORMANCE_REPEAT_*` файлах.

## ANN: три независимых повтора финальной методики

Тест использует 5 200 синтетических документов, векторы 128 dimensions и локальный mock embedding HTTP endpoint. Каждая строка — медиана пяти exact и пяти ANN измерений; стенд включает startup CLI и localhost HTTP round trip.

| Запуск | Exact median | ANN median | Парное ускорение | Кандидаты ANN | Path Recall@5 |
|---|---:|---:|---:|---:|---:|
| 1 | 256,753 мс | 50,984 мс | 5,036× | 251 | 0,60 |
| 2 | 275,494 мс | 62,174 мс | 4,253× | 251 | 0,60 |
| 3 | 226,233 мс | 49,019 мс | 4,692× | 251 | 0,60 |

Медиана трёх ANN-медиан — 50,984 мс; run-to-run spread — 13,155 мс или 25,80%. Для exact медиана — 256,753 мс; spread — 49,261 мс или 19,19%. Парное ускорение имеет медиану 4,692× и диапазон 4,253×–5,036×. Число кандидатов и Recall@5 одинаковы во всех запусках, но latency достаточно шумная. Поэтому жёсткий performance threshold в CI не добавлялся.

p95/p99 в каждом run равны `null`, потому что на режим имеется только пять измеренных samples. В прежнем отчёте p95 выводился как максимум выборки; это поле сохранено только с caveat в историческом JSON, чтобы не выдавать эти значения за устойчивую хвостовую оценку. Текущий общий helper публикует p95 только начиная с 20 samples, p99 — с 100; даже это не является SLA-гарантией.

Recall@5 = 0,60 против exact указывает на неполное совпадение ANN top-5. До использования ANN tuning в production необходим размеченный relevance dataset и sweep размера корпуса/candidate budget с Recall@K/MRR/nDCG.

## Компонентный охват

Итоговая инвентаризация разделяет три класса, чтобы метрики не смешивали код и конфигурационные данные:

- **Исходники и тесты:** 231 файл, 71 018 строк по явно включённым extensions. Это structural LOC, тесты включены; это не production LOC.
- **Конфигурация и build/platform scripts:** 46 файлов, 3 919 строк, включая XML, Gradle DSL, YAML workflow, shell scripts и CMake.
- **JSON structured data/fixtures:** 140 файлов, 3 713 строк. Машиночитаемые отчёты аудита в `docs/` исключены из инвентаря, чтобы не влиять на результат следующих запусков.

Инвентаризация повторно сгенерирована дважды и совпала побайтно. CTest зарегистрировал 48 тестов; в generated metadata у всех 48 имеется конечный положительный timeout. Статические счётчики workflow jobs: control-plane-node-e2e — 5; Flutter — 1; Native Android — 1; Qt — 1; release-gate — 6. Это метрики структуры, не доказательство длительности hosted workflow.

## Компонентные выводы

- **Core/RPC/task lifecycle:** функционально покрыт интеграционными и RPC state tests; нет методовых p95/submit-to-terminal latency и конкурентной saturation curve.
- **SQLite/artifacts:** atomicity/fault-injection tests проходят; нет representative row-count/lock wait/query plan benchmarks.
- **Memory scan/extractors/PDF/OOXML:** smoke tests проходят, но throughput (files/s, MiB/s), peak RSS и холодный/инкрементальный scan не измерены.
- **Embeddings:** mock-backed tests проверяют workflow; нет worker-count saturation, rate-limit/retry и live-provider latency.
- **ANN/vector retrieval:** synthetic end-to-end benchmark есть; quality Recall@5 0,60 не доказывает достаточную релевантность.
- **MCP/sandbox/LSP:** launcher lifecycle tests проходят; spawn/handshake/discovery/invoke/teardown не разделены по метрикам.
- **LLM/SSE:** watchdog корректен как test scenario; remote first-token/inter-token latency не измерялась.
- **Control Plane/PostgreSQL:** load probe и его метрики/редакция токена проверяются на localhost mock; это не реальная throughput/capacity оценка сервиса и БД.
- **Node Agent:** E2E и контрактные тесты проходят; multi-node fleet/reconnect storms не измерялись.
- **Android/Flutter/Qt:** статический и CI inventory есть; device/emulator/profile-mode benchmarks недоступны в этой среде.
- **CLI/TUI/Web:** функциональные state/stream tests есть; нет sustained streaming throughput, redraw latency или slow-consumer curve.
- **Build/CI/package:** чистая Release-сборка, полный CTest, inventory и install smoke валидированы локально; hosted run IDs/artifacts не подтверждены.

## Итерации

1. **Итерация 1 — baseline:** проверен исходный архив, выполнены чистая Release-сборка и полный CTest 48/48 за 252,91 с, сохранены raw logs и инвентаризация.
2. **Итерация 2 — методика:** исправлена ложная интерпретация p95/p99 для малых выборок, унифицирован helper latency, проверен установленный benchmark, проведено три повторения ANN; полный CTest 48/48 за 250,73 с.
3. **Итерация 3 — критический проход:** разделены source/config/JSON data, учтён `.gradle.kts`, отфильтрованы generated dirs и audit artifacts, Recall@K считается по уникальным exact paths, samples валидируются на finite/non-negative. Чистая Release-сборка завершилась за 9,02 с, полный CTest — **48/48 PASS за 256,35 с**, exit code 0; integration 134,13 с, SSE watchdog 39,78 с, MCP launcher 21,08 с, ANN smoke 4,62 с. Максимальный RSS процесса CTest — 130 188 KiB.

## Ограничения доказательств

Нет live Control Plane/PostgreSQL теста, внешнего embedding/LLM provider measurement, Android/Flutter/Qt device профиля, CPU flamegraph или allocator profile. Mock и CTest timing не заменяют эти измерения. Не изменять `find_hit`/dedup, SQL индексы или polling intervals по предположению: сначала зафиксировать workload, profile и equivalence/quality criterion.


## Финальная приёмка Stage 25

- Чистый Release configure: 4,59 с wall, exit 0; максимальный RSS 95 444 KiB.
- Чистый Release build (`--parallel 3`): 9,02 с wall, exit 0; максимальный RSS 81 368 KiB.
- Полный CTest: **100% tests passed, 0 tests failed out of 48**, 256,35 с; `ctest` exit 0; максимальный RSS 130 188 KiB. Прогон выполнен от непривилегированного пользователя.
- Все 48 зарегистрированных CTest тестов имеют положительный конечный `TIMEOUT` в сгенерированных metadata.
- `cmake --install` завершился успешно; установленная команда `asgend-memory-benchmark-vectors --help` работает, общий `performance_metrics.py` установлен в `bin/`.
- Три отдельных финальных ANN-повтора и все raw JSON/timing logs включены в `docs/`. ANN latency отличается между прогонами на 25,80% относительно диапазона run medians; результат остаётся диагностическим синтетическим замером, не production SLA.
- Повторный adversarial pass включал проверку source/config/JSON классов, generated directories, исключение собственных audit отчётов из инвентаря, unique-path Recall@K, валидность времени и install-пути общего helper. Найденные неточности исправлены, целевые тесты после исправлений прошли.

Отдельный инструмент запуска внешнего сабагента в среде не доступен. Независимую внешнюю оценку/числовой балл не заявляем; вместо этого проведены три документированных критических прохода, каждый с исправлениями и повторными проверками.
