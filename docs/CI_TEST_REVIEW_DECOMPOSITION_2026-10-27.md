# Декомпозиция итогового ревью CI-тестов

## Цель

Добиться, чтобы изменение общего контракта или runtime Core запускало не только подходящий workflow, но и тесты, которые подтверждают изменённое поведение; а релизный тег проходил свежие автоматические проверки на собственном commit SHA.

## Области ревью

### A. Триггеры и владение тестами

1. Сопоставить изменения в каждом path filter с реально выполняемыми командами job.
2. Убедиться, что общие Core RPC checks выполняются в трех клиентах, поскольку их workflows намеренно реагируют на shared Core изменения.
3. Проверить регрессионный guard на присутствие путей и на присутствие исполняемых команд.
4. Проверить негативные случаи: удаление filter, удаление команды, исчезновение одного теста из regex должно проваливать guard.

### B. Корневой Core CTest

1. Сверить `add_test` CMake inventory с workflow-командой fast suite и long integration.
2. Проверить, что у потенциально долгих шагов есть тестовые и job-level тайм-ауты.
3. Не скрывать `integration` посредством одного исключения без отдельного job: здесь отдельный `core-integration` уже существует.
4. Не считать локальный Stage 19 результат заменой hosted runner с доступным Landlock.

### C. Control Plane / Node Agent / PostgreSQL / Compose

1. Проверить зависимостям Python, PostgreSQL health checks и live-test env vars соответствие тестов.
2. Проверить, что PostgreSQL acceptance и backup/restore реально выделены в отдельные jobs/service.
3. Сохранять факт, что обычный unittest discovery может пропускать сервисные тесты; не объявлять их покрытыми без отдельного job.
4. Убедиться, что compose drill запускается отдельной командой и имеет достаточный timeout.

### D. Native clients

1. Native Android: shared contracts, source guard, Java/Gradle build, unit tests и APK artifacts.
2. Flutter Android: SDK pin, analyze, unit/widget tests и unsigned APK artifact.
3. Qt: dependency install, configure/build, offscreen CTest, packaging и install smoke.
4. Отдельно перечислить отсутствующие emulator/instrumentation tests и реальный device acceptance.

### E. Release gate

1. Проверять metadata и статические gate manifest.
2. Выполнять smoke tests на самом теге: core build + CTest, OpenAPI contracts, release-gate regression tests.
3. Держать заявления о live PostgreSQL/Compose/client packages пропорциональными реально выполняемым jobs.
4. Не менять исторический `verified_at` и не переписывать старое evidence задним числом.

### F. Надёжность и безопасность CI

1. Явные job-level timeouts и CTest timeouts.
2. Минимальные `permissions` сохранены.
3. Third-party action tags остаются тегами; переход на SHA pinning — отдельная supply-chain задача с плановым обновлением, а не случайной правкой в этом патче.
4. Любой skip/NOT RUN указывается отдельно от PASS.

## Условие завершения

- Все workflows parse-ятся как YAML.
- Guard подтверждает path-filter и command coverage.
- Contract/RPC tests проходят локально, а новая негативная матрица guard доказывает, что он обнаруживает потерю тестового шага.
- Release workflow выполняет свежие проверки кода, а не только читает статический gate manifest.
- Все оставшиеся ограничения отражены в отдельном CI TODO и итоговом аудите.

## Итог декомпозиции после Stage 22

- A. Триггеры/владение тестами: закрыто локально — guard проверяет path filters и команды, а все три клиентских workflow исполняют Core RPC test suites.
- B. Core CTest: полный набор 45/45 PASS (245,54 с, exit code 0) при запуске от непривилегированного пользователя.
- C. Control Plane/PostgreSQL/Compose: workflows переиспользуются release gate; live service checks требуют hosted job execution.
- D. Clients: release gate вызывает все три client workflows; job-level timeouts установлены. Android emulator/device instrumentation остаётся отдельным открытым пунктом.
- E. Release gate: metadata preflight, четыре reusable acceptance workflows и final summary с жёсткой проверкой каждого `needs.*.result`.
- F. Надёжность: YAML parsed; негативные guard tests PASS; исторические статусы не переписаны, hosted evidence не объявлено выполненным.
