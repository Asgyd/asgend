# Повторный аудит производительности — итерация 3/3: критический проход

**Дата:** 27 октября 2026 г.  
**Фокус:** проверить точность подсчёта, повторное использование статистических метрик и достоверность Recall@K; устранить обнаруженные несоответствия.

## Findings и исправления

### F-04 — JSON данные были смешаны с конфигурацией (P2, исправлено)

Первая версия schema v2 объединила JSON fixtures и измерительные отчёты с configuration inventory. Это формально сохраняло охват, но делало название категории неточным.

**Исправление:** отдельные наборы `configuration_inventory` и `structured_data_inventory`; source LOC, config/script LOC и JSON data теперь взаимоисключающие по классификации. Kotlin Gradle DSL `*.gradle.kts` классифицируется как конфигурация, а не app source.

### F-05 — возможный перекос path Recall@K при дубликатах (P1 методология, исправлено)

В benchmark использовалось пересечение множеств уникальных путей, но знаменатель брал длину исходного списка, где теоретически могли быть дубликаты. Это могло занижать/искажать Recall@K при chunk-level результатах с несколькими кандидатами одного файла.

**Исправление:** общий `path_recall_at_k()` считает уникальные непустые exact paths и использует их число в знаменателе; добавлены тесты с дублирующимися путями, пустым exact-набором и невалидным K.

### F-06 — inventory не игнорировал распространённые каталоги среды (P2, исправлено)

Исходная фильтрация игнорировала несколько известных build-каталогов, но не `.venv`, `venv`, `node_modules`, `target`, `out` и `cmake-build-*`. В другом клоне это могло резко увеличить инвентарь и исказить компонентные объёмы.

**Исправление:** централизованный `EXCLUDED_DIRS`; regression-test создаёт фиктивные node_modules/venv-файлы и подтверждает, что они не попадают в first-party inventory.

### F-07 — валидность samples не проверялась (P2, исправлено)

Общий latency helper не отвергал отрицательные и не конечные (`NaN`/`inf`) значения. Такое значение могло разрушить порядок percentile или JSON-сводку.

**Исправление:** `summarize_latency()` отвергает отрицательные и не конечные samples; unit-тесты покрывают `NaN` и отрицательное время.

## Общая реализация метрик

`performance_metrics.py` применяется к standalone ANN benchmark, Control Plane load-probe и `tests/test_memory_ann.py`. Таким образом едины правила:
- median/min/max доступны для короткой выборки;
- p95 не публикуется при менее чем 20 samples;
- p99 не публикуется при менее чем 100 samples;
- в JSON присутствует `tail_percentile_status`;
- Recall@K считается по уникальным путям exact-выдачи.

Эти sample thresholds защищают от наиболее очевидного неправильного прочтения малой выборки, но не делают малые benchmarks статистически надёжным SLA. Для capacity/performance gate нужны отдельные серии на закреплённом runner и существенно больше запросов.

## Проверки этой итерации

- Inventory unit tests: 4/4 PASS.
- ANN benchmark methodology tests: 3/3 PASS.
- Control Plane load-probe tests: 3/3 PASS.
- Python syntax compilation: PASS.
- Чистый Release configure: 4,59 с wall; чистый Release build: 9,02 с wall (parallelism 3), exit 0.
- Полный CTest на финальном дереве: **48/48 PASS за 256,35 с**, exit 0, max RSS 130 188 KiB; запуск от непривилегированного пользователя. Длительные тесты: integration 134,13 с, SSE watchdog 39,78 с, MCP launcher 21,08 с, ANN smoke 4,62 с.
- Все 48 CTest тестов имеют конечный положительный `TIMEOUT` в сгенерированных metadata.
- Install smoke: `cmake --install` успешен; установленный `asgend-memory-benchmark-vectors --help` работает, общий `performance_metrics.py` находится в `bin/`.
- Три финальных ANN-повтора: ANN median 49,019–62,174 мс; exact median 226,233–275,494 мс; paired speed-up 4,253×–5,036×; 251 кандидат; path Recall@5 0,60. Из-за малого числа samples p95/p99 корректно не публикуются; run-to-run разброс подчёркивает, что это синтетический диагностический benchmark. Полные raw logs/JSON находятся в `docs/PERFORMANCE_REPEAT_ANN_*_FINAL*` и `PERFORMANCE_REPEAT_ITERATION_3_*`.
