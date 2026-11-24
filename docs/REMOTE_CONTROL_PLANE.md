# ASGEND Remote Control Plane — серверная спецификация

Статус: **реализация Control Plane и node channel присутствует в исходниках версии 0.9.9; production hardening и упаковка остаются незавершёнными пунктами ASGEND 1.0**. Нормативные контракты: [API_CONTRACT.md](API_CONTRACT.md), [OpenAPI 3.1](../contracts/v1/openapi.yaml), [JSON Schema и примеры](../contracts/v1/). Исторически эта страница описывала целевой дизайн до реализации; приведённые ниже инварианты продолжают действовать.

## 1. Цель

Отдельный Docker-развёртываемый сервер обеспечивает удалённое управление и передачу данных между desktop/mobile клиентами и зарегистрированными ASGEND Execution Nodes. Он хранит и передаёт состояния задач, события, команды и артефакты; исполнение остаётся на Execution Node.

## 2. Запреты и граница исполнения

Сервер не выполняет следующие операции:

- не запускает ASGEND orchestrator, decision model, LLM или embedding runtime;
- не декомпозирует задачу, не выбирает tools/agents и не вызывает `tools/call`;
- не выполняет shell/programs, user code, MCP servers или builds;
- не монтирует каталоги исходников и не предоставляет LLM рабочее пространство;
- не считается исполнительной средой задачи.

Execution Node работает у пользователя/оператора и локально делегирует принятый от сервера `CommandEnvelope` существующему core API. События выполнения публикуются узлом обратно. Сервер сохраняет подтверждённое состояние и распространяет его подписанным пользователям workspace.

## 3. Компоненты развёртывания

```text
Qt Linux / Android / Web admin
             | HTTPS + authenticated API
             v
      asgend-control-plane
        |        |        |
     PostgreSQL  |    Artifact API
                 |        |
           persistent node command/event channel
                 |
         registered Execution Node
                 |
         local ASGEND core/orchestrator
         (LLM, decision model, MCP, tools)
```

- `asgend-control-plane` — API, auth, policy, node presence, command/event routing, artifact authorization и audit.
- PostgreSQL — users/workspaces/roles, node registry/sessions, task projections, events/cursors, command outbox, artifact metadata, quotas.
- Object storage/local persistent volume — blob-артефакты. В Postgres хранятся metadata/checksum, не большие файлы.
- Reverse proxy или server TLS endpoint — HTTPS/WSS, лимиты, защитные headers и сертификаты.

Предпочтительный MVP: Docker Compose с отдельными контейнерами для control plane и PostgreSQL. Не включать orchestrator и model images в Compose серверного профиля. При использовании локального object volume — отдельный named volume с backup; S3-compatible storage допускается отдельным deployment профилем.

## 4. Основные сущности

- `User` и `Workspace`: изоляция клиентов, узлов, задач и артефактов.
- `RoleBinding`: права `owner`, `operator`, `viewer` на workspace и node-level constraints.
- `ExecutionNode`: зарегистрированное устройство/хост, capability labels, protocol version, heartbeat, online status, last seen и revocation state.
- `NodeSession`/`Lease`: подтверждённая сессия узла, heartbeat deadline и владелец активной команды.
- `RemoteTask`: task ID core (либо внешний стабильный ID с mapping), workspace/node, mirror state, timestamps, latest event cursor и idempotency key.
- `CommandEnvelope`: command ID/type/payload reference/expiry/required permission/idempotency key; не исполняется сервером.
- `TaskEvent`: event ID, per-task sequence, source node, timestamp, schema version и payload projection.
- `Artifact`: artifact ID, task ID, path/display name, MIME, size, SHA-256, storage reference, visibility and retention policy.
- `AuditEvent`: actor, action, target, result, timestamp, request ID и минимальные безопасные детали.

## 5. API и поток управления

Полный endpoint inventory и схемы фиксируются в [`contracts/v1/openapi.yaml`](../contracts/v1/openapi.yaml); поведенческие и security-инварианты описаны в [`API_CONTRACT.md`](API_CONTRACT.md). Основные группы endpoints:

- `POST /v1/workspaces/{id}/tasks` — создать remote task/command с idempotency key и целевым execution node.
- `GET /v1/tasks` / `GET /v1/tasks/{id}` — список и mirror state, фильтры по состоянию, node и времени.
- `GET /v1/tasks/{id}/events?after_event_id=...` и поток событий — курсорное восстановление после reconnect.
- `POST /v1/tasks/{id}/answer`, `/cancel`, `/retry` — проверка прав и добавление команды в durable outbox.
- `GET /v1/nodes`, enroll/revoke/heartbeat API — lifecycle узлов.
- Artifact init/upload/complete/download — ограниченная multipart/resumable передача; checksum до регистрации готового объекта.
- Node channel: pending commands, command acknowledgement, heartbeat, event batches, state snapshot и artifact registration.

Server endpoint не должен утверждать, что задача `running`, только потому что submit принят. Пока node не подтвердил получение/передачу local core, состояние должно явно отражать `pending_delivery`/`accepted_by_server`; фактические task states синхронизируются из core.

## 6. Доставка и согласованность

- Команды сначала пишутся в persistent outbox в транзакции, затем доставляются онлайн-узлу.
- На каждом command ID действует at-least-once доставка с idempotency; Execution Node обязан дедуплицировать команды. Не обещать exactly-once execution через сеть.
- Команда содержит TTL; после expiry сервер ставит понятный delivery failure, если команда ещё не передана узлу.
- Узел публикует event IDs и sequence; сервер дедуплицирует, обнаруживает пробелы и запрашивает snapshot/resync.
- Длительные отключения не теряют состояние; список задач остаётся доступным с отметкой stale/out-of-date, а команды отображаются pending/expired.
- События упорядочиваются per task; нет обещания глобального total order между разными задачами.

## 7. Защита данных

- TLS обязателен на внешнем интерфейсе. Node enrollment и token rotation/revocation должны быть частью первого рабочего релиза.
- Server user tokens и node credentials раздельные. RBAC проверяется на каждом чтении/изменении/скачивании объекта.
- Артефакт нельзя запросить только по угадываемому ID: проверяются workspace, node/task ownership и visibility; download URL ограничен по сроку и scope.
- Секреты не сохраняются в body логов; payload task может содержать чувствительные данные — по умолчанию редактировать логи и задавать retention.
- Rate/size limits, quota, audit, protection against path traversal, archive bombs и MIME spoofing.
- Container non-root, read-only rootfs по возможности, dropped capabilities, no-new-privileges, seccomp default profile, health/readiness, CPU/RAM/PID limits.

## 8. Первая реализация и тесты

1. Поддерживать OpenAPI/JSON Schema, Postgres migrations, identity/workspace/RBAC; контрактная база размещена в `contracts/v1/`.
2. Secure node enrollment, heartbeat, reconnect and command lease.
3. Durable outbox, local-node adapter к текущему core API, идемпотентность и command ack.
4. State/event mirror, cursor, snapshots и resync.
5. Artifact transfer, checksum, quotas, access control и retention.
6. Dockerfile/Compose, health check, structured safe logs, metrics and audit.
7. Backup/restore, migrations, TLS example, deployment guide — reference tooling added in [`server/DEPLOYMENT.md`](../server/DEPLOYMENT.md); Docker/PostgreSQL runtime and restore drill still need environment-level validation.

Acceptance tests: повторный submit не создаёт второй task; offline/reconnected node получает неистёкшую команду только один раз на semantic уровне; server restart сохраняет state/events/outbox; отмена отправляется node, но сервер не исполняет отмену сам; denied user не читает task/artifact чужого workspace; подмена checksum отклоняется; процесс контейнера работает non-root; образ сервера не содержит model binaries и не монтирует workspace.

## 9. Hermetic acceptance для Control Plane ↔ Execution Node

Реальный HTTP Control Plane и `NodeAgent` проверяются вместе тестом
`server/tests/test_node_agent_e2e.py`. В тесте поднимается loopback HTTP server с SQLite,
после чего настоящий клиент узла отправляет heartbeat, получает durable command через
lease, исполняет submit, подтверждает команду, передаёт события, загружает артефакт частями
и проверяет его SHA-256. Затем пользовательская сторона получает одноразовый download ticket
и скачивает те же байты; также проверяются отсутствие тестового секретного маркера в event
feed и исключение symlink из списка доступных артефактов.

Транспортный протокол локального C core в этом hermetic тесте имитирует `MockCore`, чтобы
тест не зависел от модели, GPU или внешней службы. Запуск:

```sh
python3 -m unittest discover -s server/tests -p 'test_*.py' -v
python3 -m unittest discover -s node_agent/tests -p 'test_*.py' -v
python3 tests/contracts/test_contracts.py
```

Тот же набор запускается workflow `.github/workflows/control-plane-node-e2e.yml` вместе с
Release-сборкой C-ядра и быстрыми CTest. Это покрывает интеграцию Control Plane и node-agent,
но не заменяет отдельный e2e прогон с скомпилированным C core, живым PostgreSQL и Docker Compose.

### Acceptance test with the compiled C core

The `node_agent_c_core_e2e` CTest starts a fresh `agent-orchestrator` process
with an isolated SQLite database and private workspace, then connects the real
Python `NodeAgent` to it and to a live HTTP Control Plane. It verifies command
lease/ack, C-core task status and event projection, workspace artifact listing
and chunk reads, symlink exclusion, upload checksum validation, and download-ticket
round trip. No LLM endpoint is required: the core task is expected to terminate
with `completed_without_model` when no provider is configured, which keeps this
transport/artifact acceptance deterministic.

Run it after building the core:

```sh
ctest --test-dir build --output-on-failure -R '^node_agent_c_core_e2e$'
```

This test uses the real C TCP JSON API but SQLite; it does not replace the
separate PostgreSQL migration/restart/redelivery and backup/restore acceptance.

