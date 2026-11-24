# ASGEND — продолжение реализации, итерация 2 (21 октября 2026)

## Область работ

Продолжена реализация по `TODO.md`, следующий фокус WP2 — Control Plane deployment/recovery и acceptance tests. Уже имеющиеся обработчики Control Plane не переписывались; добавлена упаковка для запуска и регрессионные тесты конкретных инвариантов.

## Реализовано

- `server/Dockerfile`: Python 3.12 slim image, UID/GID 10001 без shell/login, только серверный пакет, lazy PostgreSQL dependency `psycopg[binary]`, entrypoint `python -m asgend_cp`.
- `server/deploy/compose.yaml`: PostgreSQL, одноразовый migration service, Control Plane и NGINX TLS reverse proxy; секрет пароля PostgreSQL монтируется как Docker secret; `control-plane` работает с `read_only: true`, tmpfs `/tmp`, dropped capabilities, `no-new-privileges`, PID limit, health/readiness check. PostgreSQL не публикуется на host port, API опубликован только через proxy.
- `server/deploy/nginx.conf`: TLS 1.2/1.3, redirect HTTP→HTTPS, security headers, ограничение upload body, timeouts для long-poll.
- `server/DEPLOYMENT.md`, `.env.example`, `.dockerignore`, `.gitignore`: шаги развёртывания, настройки host allow-list, сеть trusted proxy, operations, backup и restore drill.
- `server/scripts/backup.sh`, `server/scripts/restore.sh`: сценарии quiesce/backup/restore для Compose + PostgreSQL, checksum validation до изменения state, staging во временном контейнере вместо writable host bind mount, интерактивное подтверждение destructive restore, итоговая проверка artifact hashes.
- `server/tests/test_storage_backup.py`: migration sequencing/idempotency, rejection of future schema, qmark-to-psycopg translator lexical cases, SQLite backup/restore с реальным blob-артефактом, checksum tamper и отказ перезаписи БД без `--force`.
- `server/tests/test_api_security.py`: viewer RBAC, tenant/workspace isolation, submit idempotency/key-reuse conflict, immediate node-token revoke, artifact checksum mismatch rejection.
- Исправлен `translate_pg()` в `server/asgend_cp/db.py`: он больше не подменяет символ `?` в строковых литералах, comments и dollar-quoted SQL на параметры PostgreSQL; `%` корректно экранируется для psycopg parameter binding.
- Обновлены `TODO.md`, `README.md` и `docs/REMOTE_CONTROL_PLANE.md` с указанием состояния развертывания и оставшихся проверок.

## Проверки в этой среде

- `python3 -m unittest discover -s server/tests -p 'test_*.py' -v`: **10/10 passed**.
- `python3 tests/contracts/test_contracts.py`: **31 passed, 1 skipped** (необязательный `openapi-spec-validator` отсутствует).
- `python3 -m unittest discover -s node_agent/tests -p 'test_*.py' -v`: **37/37 passed** при повторном полном прогоне. Первый прогон после добавления Control Plane tests имел единственный сбой `test_server_checksum_mismatch_reuploads`; изолированный повтор и повтор всего набора прошли.
- `python3 -m compileall -q server/asgend_cp server/tests`: passed.
- `cmake -S . -B build-iteration-2 -DCMAKE_BUILD_TYPE=Release` and build: all C targets compiled and linked successfully.
- `ctest --test-dir build-iteration-2 --output-on-failure -E '^integration$'`: **11/11 passed**; the long integration test is not included in this run.
- `sh -n server/scripts/backup.sh server/scripts/restore.sh`: passed.
- Compose YAML загружается `PyYAML`; статическая проверка обнаруживает все 4 ожидаемых сервиса, `read_only` и `no-new-privileges` настройки.

## Не заявляется выполненным

В этой среде команда Docker отсутствует. Поэтому Docker image build, `docker compose config/up`, подключение к реальному PostgreSQL, TLS handshake в NGINX, полный PostgreSQL backup/restore drill, restore после рестарта и end-to-end CP↔node-agent↔mock core не исполнялись. Эти пункты в TODO помечены как добавленные, но не runtime-verified. Целевой deployment должен пройти их до production.

WP3-WP6, Qt/Android packaging/build, PDF/Office extraction, decision-model query planning, memory GC/stats/benchmark и финальный ASGEND 1.0.0 acceptance остаются отдельными незавершёнными работами.
