# TCP/JSON protocol

One UTF-8 JSON object per TCP connection, terminated with `\n`. The server returns one JSON object plus newline and closes the connection. The maximum request size, deadline, connection count and bind address are configurable. When `server.auth_token` is set, every request must include the same token in `auth_token`. TCP is not encrypted. The same JSON protocol is exposed through `server.unix_socket.path` and is intended to remain client-independent (web, CLI, Linux desktop and Android clients); the Unix socket is created with mode `0600` and accepts only a peer with the same effective UID. The optional browser UI is separate and loopback-only.

## Operations

### Complete TCP/Unix RPC operation inventory

The inventory below is machine-checked against the dispatch branches in `server_handle_request()` by `tests/test_core_rpc_contract_inventory.py`. It describes only the newline-delimited JSON RPC exposed on the configured TCP listener and Unix socket. It is **not** the separate loopback HTTP browser API documented below.

<!-- ASGEND_CORE_RPC_INVENTORY_BEGIN -->
```text
answer
artifact_list
cancel
config_get
config_save
config_update_settings
dead_letters
events
health
list_agents
list_tasks
lsp_diagnostics
mcp_status
model_disable
model_enable
model_start
model_stop
models
ping
plan_add
plan_delete
plan_list
plan_update
plan_wait
resources
retry_task
status
submit
workspace_list
workspace_read
workspace_read_bytes
workspace_read_chunk
workspace_write
```
<!-- ASGEND_CORE_RPC_INVENTORY_END -->

### Request and response contracts

`tests/test_core_rpc_runtime_contract.py` starts an isolated core and checks the following request requirements and representative success response shapes over the live TCP JSON-line transport. Every response is a JSON object with boolean `ok`; failures additionally carry a non-empty string `error`. Fields shown as optional have defaults or are validated only when supplied. These checks complement (rather than replace) operation-specific integration/security tests.

| Operation | Required request fields / constraints | Representative success response fields |
|---|---|---|
| `answer` | `task_id`, non-empty `answer` | `task_id`, `status` |
| `artifact_list` | `task_id` | `artifacts` array, `truncated`, `count`, skip counters |
| `cancel` | `task_id` | `task_id`, `status` |
| `config_get` | none | `main` object, `models` object, `agents` array, `restart_required` boolean |
| `config_save` | `main` object, `models` object, `agents` array | `restart_required` boolean, `message` string |
| `config_update_settings` | non-empty `settings` object using the allow-listed keys | `restart_required` boolean, `message` string |
| `dead_letters` | none; optional integer `limit` | `dead_letters` array |
| `events` | `task_id`; optional integer `after_id` and `limit` | `events` array |
| `health` | none | `tasks_by_status` object, integer counters, boolean flags |
| `list_agents` | none | `agents` array; each row has string `id` |
| `list_tasks` | none; optional integer `limit` and string `status` | `tasks` array |
| `lsp_diagnostics` | non-empty `file_path` | `diagnostics` string |
| `mcp_status` | none | `servers` array |
| `model_disable` | non-empty `model_id`; optional `kind` is `llm` or `decision` | `model_status` on success |
| `model_enable` | non-empty `model_id`; optional `kind` is `llm` or `decision` | `model_status` on success |
| `model_start` | non-empty `model_id`; optional `kind` is `llm` or `decision` | `model_status` on success |
| `model_stop` | non-empty `model_id`; optional `kind` is `llm` or `decision` | `model_status` on success |
| `models` | none | `model_status`, `resources` |
| `ping` | none | string `service`, string `version` |
| `plan_add` | string `kind`, string `title`; optional `scope_id` | integer `item_id`, string `scope_id` |
| `plan_delete` | positive integer or numeric-string `item_id`; optional `scope_id` | `item_id`, `scope_id` |
| `plan_list` | none; optional `scope_id`, `kind` (`todo` or `goal`) | `items` array, string `scope_id` |
| `plan_update` | positive integer or numeric-string `item_id`; optional `title`, `description`, `status`, `scope_id` | `item_id`, `scope_id` |
| `plan_wait` | none; optional non-negative integer `revision`, integer `timeout_ms` in `0..30000` | integer `revision`, boolean `changed`, boolean `stopping` |
| `resources` | none | `directories` object, `files` object, `decision_model_id` string, `classification_parallel` boolean |
| `retry_task` | `task_id` | `task_id`, `status`, integer `additional_attempts` |
| `status` | `task_id` | `task_id`, string `status`, integer counters, boolean `cancel_requested` |
| `submit` | non-empty string `task` **or** `prompt`; optional integer `priority` in `0..9` | `accepted`, `task_id`, `status`, `duplicate`, `priority` |
| `workspace_list` | `task_id`; optional safe relative directory `path` | `entries` array, boolean `truncated` |
| `workspace_read` | `task_id`, non-empty safe relative file `path` | `path`, UTF-8 `content`, integer `size` |
| `workspace_read_bytes` | `task_id`, non-empty safe relative file `path`; optional integer `offset`, `limit` | `path`, `data_b64`, `offset`, `next_offset`, `size`, `eof` |
| `workspace_read_chunk` | `task_id`, non-empty safe relative file `path`; optional integer `offset`, `limit` | `path`, UTF-8 `content`, `offset`, `next_offset`, `size`, `eof` |
| `workspace_write` | `task_id`, non-empty safe relative file `path`, UTF-8 `content` without NUL | `path`, integer `size` |

A required task ID is resolved before any follow-on field validation for task-scoped operations. Clients should therefore provide a valid task ID when they need to distinguish a missing `path`/`answer` error from an absent or unknown task. `task` and `prompt` are aliases for submission; other listed required fields are not interchangeable. The table records request requirements and principal response keys, not exhaustive authorization or state-transition semantics.

Additional operations not expanded in the examples below:

- `config_get`: returns the legacy administrative config bundle (`main`, `models`, `agents`) and `restart_required`. Treat it as privileged because configuration can contain executable model/agent commands and credentials.
- `config_save`: legacy full-bundle write of `main`, `models`, and `agents`. It is not the safe guided-settings API; clients editing a small set of runtime settings should prefer `config_update_settings`, which has an allow-list and does not replace model registries or agent profiles.
- `models`: returns the model runtime snapshot, including ordinary LLMs, decision models, and nodes. `model_enable`, `model_disable`, `model_start`, and `model_stop` act on a `model_id` and a `kind` of `llm` or `decision`; start/stop can take substantially longer than a normal RPC.
- `resources`: returns the core's discovered resource catalog grouped by category.
- `mcp_status`: returns the current MCP server status records.
- `lsp_diagnostics`: runs host-side LSP diagnostics for a `file_path` under the configured project root. Diagnostics run on the machine hosting the core, not on the client device; the client path is validated by the core.

- `{"op":"ping"}`: liveness and version.
- `{"op":"list_agents"}`: configured agent IDs, names and descriptions.
- `{"op":"health"}`: counts of jobs by state plus worker/scheduling information.
- `{"op":"list_tasks","limit":100,"status":"queued"}`: up to 200 tasks with priority, retry schedule and timestamps; `status` is optional and validated.
- `{"op":"dead_letters","limit":50}`: inspect terminal `failed` tasks, including the last error, attempts and task title.
- `{"op":"retry_task","task_id":"task-..."}`: manually redrive a failed task. One additional attempt is granted, while the historical attempt count and event history are preserved.
- `{"op":"config_update_settings","settings":{"runtime":{"workers":4}}}`: schema-validated, allow-listed partial update of only `server.max_connections`, `server.request_timeout_seconds`, `runtime.workers`, `runtime.queue_max_pending`, `runtime.max_retries`, `runtime.priority_aging_seconds`, and the documented `sandbox` boolean/resource-limit fields. Unknown sections/keys and type/range mismatches are rejected before a write. The operation merges into the existing main config, validates the complete candidate with `config_load`, writes a private temporary file, retains a temporary rollback copy until the atomic replacement is durable, then cleans up the rollback file. It never writes `models.json` or agent profiles and returns `restart_required: true`; the active process continues using its in-memory settings until restart. Use this operation for guided client settings; `config_save` remains the legacy full-bundle administrative editor contract.
- `{"op":"submit","task":"...","session_id":"optional","agent":"generalist","idempotency_key":"optional-stable-key","priority":5}`: persist and enqueue; returns a `task_id`. Priority is an integer `0..9` (9 is highest, default 5). Queue scheduling uses priority aging so long-waiting low-priority tasks eventually catch up.
- `{"op":"status","task_id":"task-..."}`: lifecycle state, attempts, timestamps (including `next_attempt_at` when delayed) and result, if available.
- `{"op":"cancel","task_id":"task-..."}`: cancels queued work or requests cooperative cancellation for a running task.
- `{"op":"events","task_id":"task-...","after_id":0,"limit":100}`: returns ordered persisted events after the supplied event ID, bounded by `limit`; clients should advance their cursor only after accepting events and should deduplicate by event ID across reconnects.
- `{"op":"answer","task_id":"task-...","answer":"..."}`: submits the user's selected or free-text answer to a task in `waiting_input`; continue polling the same task ID afterward.
- `{"op":"plan_list","scope_id":"global"}`: lists To-do/Goal items for the global scope or a resolved task ID. Optional `kind` filters to `todo` or `goal`.
- `{"op":"plan_add","scope_id":"global","kind":"todo","title":"..."}`, `plan_update`, and `plan_delete`: mutate shared To-do/Goal entries. Successful mutations publish a new in-process plan revision.
- `{"op":"plan_wait","revision":0,"timeout_ms":1000}`: long-polls for a plan revision change and returns `{ "revision": 1, "changed": true, "stopping": false }`. `revision` is the last revision observed by the client; `timeout_ms` is `0..30000` (default 15000). Revision notifications are process-local, so clients should load the current plan on startup/reconnect and use the revision only for live updates.
- `{"op":"workspace_list","task_id":"task-...","path":""}`: lists entries below a task's workspace using relative paths and rejecting symlinks.
- `{"op":"workspace_read","task_id":"task-...","path":"notes.txt"}` and `workspace_write`: full-text editor operations limited to 1 MiB of valid UTF-8 text.
- `{"op":"workspace_read_chunk","task_id":"task-...","path":"large.log","offset":0,"limit":32768}`: reads a bounded UTF-8 text chunk and returns `content`, whole-file `size`, `offset`, `next_offset`, and `eof`. `limit` must be `1..65536`; chunk boundaries are aligned to the last newline when possible and otherwise kept on a UTF-8 code-point boundary. The operation is workspace-confined and supports large files without reading the entire file into memory.
- `{"op":"workspace_read_bytes","task_id":"task-...","path":"out/report.pdf","offset":0,"limit":262144}`: binary-safe bounded read used by the remote Execution Node to upload artifacts. Returns `data_b64` (standard base64 with `=` padding, no line breaks), `path`, `offset`, `next_offset`, whole-file `size`, and `eof`. `offset` defaults to `0` and must be `0..size` (`offset == size` returns empty data with `eof: true`; `offset > size` is an error); `limit` defaults to `262144` and must be `1..1048576`. Only regular files are readable: the path must be a safe relative path (no leading `/`, `.`/`..` or empty components, backslashes, control characters or embedded NUL), every component is opened with `O_NOFOLLOW`, so a symlink anywhere in the path is rejected, and FIFOs/devices/directories fail. Memory use is bounded by `limit`.
- `{"op":"artifact_list","task_id":"task-..."}`: recursively enumerates the task workspace through directory file descriptors (`openat`/`fstatat` with `O_NOFOLLOW`) and returns `artifacts: [{"path","size","sha256","mime"}]` sorted by `path`, plus `truncated`, optional `truncated_reason` (`entry_limit`, `scan_limit` or `hash_budget`), `count`, and skip counters `skipped_too_large`, `skipped_too_deep`, `skipped_special` (symlinks, FIFOs, sockets, devices), `skipped_unsafe_name` (names that are not valid UTF-8 or contain control characters/backslashes, or paths of 1024+ bytes), `skipped_unreadable`, and `skipped_internal` (core-internal `.asgend*` entries such as safe-save temporaries). Limits: 16 nested directory levels, 1000 listed files, 100000 inspected directory entries, regular files up to 256 MiB (larger files are skipped and counted), 4 GiB hashed per request. `sha256` is lowercase hex computed with OpenSSL EVP over exactly `size` bytes; `mime` comes from a file-extension table, then a magic-byte sniff (PNG, JPEG, GIF, WebP, PDF, ZIP, gzip, xz, zstd, SQLite, ELF, ...), then a UTF-8 text check (`text/plain`), defaulting to `application/octet-stream`. Every listed `path` is directly usable with `workspace_read_bytes`. Both artifact operations accept a unique task-ID prefix like `status`.

Automatic retries use a persisted exponential delay of 1, 2, 4, 8, 16, then 30 seconds (30-second cap). During graceful shutdown, active workers finish their current task; not-yet-started claims are returned to the durable queue without consuming an attempt, and queued tasks remain for the next startup.

Successful submission is asynchronous: poll `status` for `queued`, `running`, `waiting_input`, `completed`, `completed_without_model`, `failed` or `cancelled`. On `waiting_input`, render `result.question` and the `question`/`agent_question` event payload; send the selected or free-text answer with `answer`, then continue polling the same task ID. `idempotency_key` is unique in the task database and can be used to make retrying a submission safe.

## Example

```sh
printf '{"op":"ping"}\n' | nc 127.0.0.1 8765
printf '{"op":"submit","task":"Plan a C module","session_id":"demo"}\n' | nc 127.0.0.1 8765
```

## Local web UI API

When `ui.enabled` is true, open `http://127.0.0.1:<ui.port>/` and enter `ui.auth_token`. The HTTP API uses `X-AO-Token` and exposes `GET /api/health`, `GET /api/agents`, `GET /api/tasks`, `GET /api/tasks/<id>`, `POST /api/tasks`, and `POST /api/tasks/<id>/cancel`. The browser listener binds only to loopback and checks the `Host` header; do not reverse-proxy it to a public interface without designing a separate authenticated deployment architecture.

Administrative model/config endpoints (same `X-AO-Token`):

- `GET /api/configs`: read the current `config/config.json`, `config/models.json`, and `config/agents/*.json` from disk plus `restart_required`.
- `PUT /api/configs`: validate and save `{ "main": object, "models": object, "agents": array }`. The configured agent/model paths are pinned to the active config tree. Invalid IDs, duplicate IDs or references to nonexistent model IDs are rejected.
- `GET /api/models`: runtime status of ordinary LLMs, decision models and Nodes.
- `POST /api/models/llm/<id>/start`, `POST /api/models/llm/<id>/stop`: manage a local LLM process.
- `POST /api/models/decision/<id>/start`, `POST /api/models/decision/<id>/stop`: manage a decision-model process.
- `POST /api/models/{llm|decision}/<id>/enable`, `POST /api/models/{llm|decision}/<id>/disable`: change the persisted enabled flag; enabling a model attached to a disabled/missing node is rejected.
- `GET /api/resources`: dynamically scanned `.md` file names grouped by `skills`, `system_prompts`, `tools`, `knowledge`, and `instructions`, plus the active decision-model ID and parallel-classification flag.

The configuration editor is privileged because model profiles contain executable commands. Keep UI on loopback and protect the token like an administrator credential. Saving writes the real JSON files; a running process continues to use its in-memory snapshot until restarted, and `restart_required` makes that difference visible.

## Resource classification

Each task substep can trigger four concurrent decision-model classification calls. Each instruction comes from `~/.config/asgend/instructions/` and is independently expanded with the live file catalog for its category. Tool catalogs include the currently discovered MCP aliases and schemas. For large catalogs, each category uses multiple bounded batches and merges only exact identifiers returned from that batch. Selected resource paths are returned in `result.formalization[].resource_selection`; `model_assisted=false` indicates an explicit fallback rather than successful model classification.
