# Implementation plan

> **Release status (2026-10-22):** all required `1.0.0` gates have passed with reproducible local evidence; the maintainer approved the stable release on 2026-10-22 ([`../release/versions.json`](../release/versions.json)). Evidence: [`RELEASE_CHECKLIST_1.0.md`](RELEASE_CHECKLIST_1.0.md), [`RELEASE_STATUS.md`](RELEASE_STATUS.md); notes: [`RELEASE_NOTES_1.0.0.md`](RELEASE_NOTES_1.0.0.md).

## Delivered in 0.9.0 — M1 memory scan/state foundation

1. **Separate C library and CLI:** `asgend_memory` plus `asgend-memory scan|verify|status|events`, independent from the orchestrator runtime.
2. **Private SQLite state:** `.asgend_mem/index.sqlite3`, schema version 1, WAL, snapshots, scan history, event journal and indexes; no generated metadata is written back to source files.
3. **Recursive and bounded scanner:** regular files are opened relative to directory file descriptors with `O_NOFOLLOW`; symlinks and cross-device mountpoints are skipped; `.asgend_mem` is excluded; each file has a configurable size cap.
4. **Content state:** SHA-256 is calculated through OpenSSL EVP; size/nanosecond mtime optimize normal scans, `verify` forces hashing, and unique-hash rename detection preserves file identity.
5. **Recovery semantics:** traversal errors mark a scan partial and do not mark unseen entries deleted. Per-file errors are saved and do not abort other files.
6. **Verification:** integration tests exercise initial/unchanged scans, modified/deleted/added/renamed files, persistent file identity, symlink guards, size limits, same-size/same-mtime change detected by verify, and SQLite integrity.

Status update (2026-10-21): built-in PDF and DOCX/XLSX/PPTX OOXML extraction are implemented with bounded input/decompression tests. Iteration 7 adds paired baseline-vs-planned retrieval evaluation in `scripts/evaluate_memory_retrieval.py --compare-plan`, including Precision@K, Recall@K, MRR@K, nDCG@K and metric deltas. A deterministic mock-based test validates the evaluation path. Remaining in this plan: a large independent human-judged quality set, client packaging and production deployment; opt-in strict-schema query planning was added in iteration 6. Historical implementation notes below describe the earlier 0.9.x state. Original gaps at the time of this plan were: specialized PDF/Office extraction and chunking, chunk-level retrieval, query classification and advanced metadata filters. Basic UTF-8/OKF extraction shipped in 0.9.1; decision metadata/title/description shipped in 0.9.2; embeddings, FTS5 BM25 and hybrid query shipped in 0.9.3. See `docs/MEMORY_INDEX.md` and `docs/ROADMAP.md`.



## Delivered in 0.8.3

- Every stdio MCP child starts with a clean environment, even when OS sandboxing is explicitly disabled, so API keys and service secrets are not inherited implicitly.
- Added a per-server `mcp.servers[].env` allow-list for explicitly configured string values, with a limit of 16 variables and validation that blocks overrides of `PATH`, `HOME`, `TMPDIR`, `LD_*`, and `DYLD_*`.
- Added end-to-end coverage that launches an MCP server with sandboxing disabled and proves an inherited parent secret is absent while an explicitly configured variable is present.
- Corrected the modern-protocol test setup to pass its mode flag through the server-specific environment allow-list rather than relying on implicit inheritance.

## Delivered in 0.8.2

- Active MCP calls are cooperatively cancellable on both stdio and Streamable HTTP transports. stdio polling and server-lock acquisition check the owning task state; HTTP uses libcurl progress callbacks. Cancellation no longer triggers child-process restart solely because a running task was cancelled.

## Delivered in 0.8.1

1. **Active LLM request cancellation:** supported OpenAI-compatible streaming and System One JSON transports now register libcurl progress callbacks that poll durable task cancellation and abort in-flight requests before their full HTTP timeout.
2. **Parallel classifier context:** each of the four parallel resource-classifier threads receives the owning task ID in thread-local LLM context, so cancellation also reaches active classifier HTTP requests; inline fallback preserves the parent's context.
3. **Verification:** integration coverage delays a System One resource-classifier request, cancels the task while the request is active, and asserts the task reaches `cancelled` within the transport-cancellation deadline.

## Delivered in 0.8.0

1. **Persistent task retry backoff:** automatic retries store `next_attempt_at` in SQLite and become claimable after exponential delays of 1, 2, 4, 8, 16 and 30 seconds (capped); crash recovery applies the same policy.
2. **Dead-letter operations:** `dead_letters` inspects terminal failed tasks and `retry_task` redrives one task for exactly one extra attempt while preserving its attempt count and event history.
3. **Graceful worker drain:** shutdown workers stop claiming new tasks, return unstarted claims without consuming attempts, and allow in-flight tasks to finish; queued jobs remain durable for the next start.
4. **Verification:** integration coverage exercises persisted recovery backoff, dead-letter inspection/manual redrive, task event history and additive SQLite schema migration.

## Delivered in 0.7.0

1. **Persistent MCP stdio supervision:** monitor child process exits and transport-level pipe/response failures after the initial handshake; restart the child in place without restarting ASGEND.
2. **Bounded exponential backoff:** retries at 1, 2, 4, 8, 16, then 30 seconds (30-second cap), interruptible during shutdown. Failed restart handshakes and `tools/list` checks are cleaned up before the next attempt.
3. **Stable tool registry during recovery:** cached aliases and schemas remain immutable while workers are running; a restarted peer repeats handshake and `tools/list`, while newly changed tool schemas are picked up after an ASGEND restart. Streamable HTTP MCP endpoints remain request-based and are not child-process supervised.
4. **Verification:** integration test crashes an MCP process immediately after successful discovery, checks supervised restart/handshake, then runs a real tool call through the restarted server.

## Delivered in 0.6.0

1. **Separate dynamic resource classifiers:** four independent decision-model calls (skills, system prompts, tools/MCP, knowledge) run concurrently per agent subtask, using editable templates from `~/.config/asgend/instructions/` and fresh Markdown catalogs split into bounded batches. Exact file IDs are merged; tool selection remains constrained by the agent allow-list.
2. **Liquid AI D1 3B decision profile:** distinct `liquid-d1-3b` entry is configured in `decision_models` as the orchestrator decision model; example path remains disabled until a real model binary/weights or remote endpoint is configured.
3. **Nodes and UI navigation:** local/LAN/Internet node registry, profile enable/disable buttons, remote HTTPS enforcement, disabled-node guards, and tabbed topbar for Dashboard / Nodes / Agents / Resources.
4. **Verification:** classification output is checked per agent, 140+ `.md` files are discovered, a file added after startup appears without restart, model toggles persist, and invalid Internet endpoints/disabled-node enable attempts are rejected.

## Delivered in 0.5.0

1. **Multi-model supervisor:** registry for local LLM and distinct decision models, command/argument templates, model path and working directory, health checks, process logs, lazy `auto_start`, and `start_at_boot`.
2. **Role binding:** `agent.model_id` and registry defaults bind task-execution agents to LLMs while `defaults.decision_model_id` selects the orchestrator decision profile. Legacy single `local_llm` config remains supported when no registry is configured.
3. **Unified configuration editing:** main settings, model registry, and all agent profiles live under `config/`; authenticated UI loads/saves the same on-disk JSON files, validates model references, and flags restart-required state using a content fingerprint.
4. **Model management UI/API:** model states, launch/stop operations, separate UI editors for main/models/agent configs, add/remove helpers and validation failures before saving.
5. **Verification:** on-demand process launch for separate decision and agent model profiles, actual routing to separate endpoints, process shutdown, UI save/persist/reject invalid references, and restart-required state.

## Delivered in 0.4.0

1. **Sandbox hardening:** Landlock now limits file reads to runtime paths and administrator-configured `read_only_paths`, while restricting writes to the private workspace; seccomp validates syscall architecture and blocks `io_uring` when network access is disabled; cleanup repairs child-modified directory modes before deleting the sandbox.
2. **Verification:** release build, integration suite, ASan/UBSan and GCC `-fanalyzer` all pass in the development environment.

## Delivered in 0.3.0

9. **Secure local access:** same-UID Unix-domain socket (`0600`), strict loopback web UI with dedicated token and Host-header validation, security headers/CSP, dashboard task submit/status/cancel.
10. **Queue scheduler:** task priorities `0..9`, default 5, aging against starvation, `list_tasks` API, schema migration for previously created SQLite databases.
11. **MCP process sandbox:** CPU/address-space/file-size/fd limits, root-to-nobody UID/GID drop, private per-process workspace, `no_new_privs`, seccomp denial of network and high-risk syscalls, fail-closed Landlock filesystem policy: runtime read paths plus explicit `read_only_paths`, workspace-only writes.

## Delivered in 0.2.0

1. **Runtime foundation:** configuration validation, JSON agent profiles, skills and local knowledge.
2. **TCP/JSON API:** bounded request body, absolute request deadline, connection cap, UTF-8 checks, optional auth token and loopback-safe defaults. The transport token is stripped from the persisted task payload.
3. **Durable queue:** SQLite/WAL, atomic worker claims, idempotency keys, retry budget, status, cooperative cancellation, stale-running recovery, and session context.
4. **Three-stage decomposition:** clarification/assumptions, step generation, independent quality review and correction. Model-generated structured outputs are parsed strictly and normalized before execution; fallbacks are explicitly marked.
5. **Formalization/execution:** each step receives a concrete instruction and acceptance criterion; decision-driven routing to flexible agent profiles; results merged into machine-readable JSON.
6. **Local LLM:** OpenAI-compatible Chat Completions adapter, bounded tool-call loop and session-history injection.
7. **Tools and MCP:** built-in allow-listed tools, MCP stdio lifecycle, initialize handshake, `tools/list`, schema exposure only to agents that allow the exact alias, and `tools/call`.
8. **Verification:** integration coverage for auth, strict JSON boundaries, token redaction, queued and running cancellation, idempotency, restart recovery, model-assisted three-stage outputs, MCP tool calls and cross-task session history.

## Acceptance criteria

- A task is committed to SQLite before the API returns an accepted `task_id`.
- Repeating an `idempotency_key` returns the original task.
- Worker claims are atomic and bounded by the configured queue limit.
- A stale running task is requeued if attempts remain or terminated after the retry budget.
- Queued cancellation is immediate; running cancellation is cooperative at model/tool/orchestration boundaries and is reflected as `cancelled`.
- Malformed JSON, trailing garbage, raw NUL bytes and invalid UTF-8 do not become accepted requests.
- Transport credentials are not stored in the queued payload.
- If the model is disabled, the response is `completed_without_model` and never claims model execution.
- Integration tests pass from a fresh CMake build; the same suite also passes under ASan/UBSan.

## Remaining roadmap

Подробный формализованный roadmap с приоритетами, зависимостями и критериями приёмки ведётся в [`docs/ROADMAP.md`](ROADMAP.md). Спецификации двух крупных компонентов вынесены в [`docs/MEMORY_INDEX.md`](MEMORY_INDEX.md) и [`docs/REMOTE_CONTROL_PLANE.md`](REMOTE_CONTROL_PLANE.md).

### Текущий статус — 2026-10-21, итерация 8

- Qt 6 desktop MVP: заполнен `clients/asgend_qt/src/main.cpp` и `src/ui/mainwindow.*`, CMake target больше не ссылается на отсутствующие view/task-event-stream файлы; подключены существующие Core/Control Plane adapters для профилей, списка/фильтра задач, submit, событий, answer/cancel/retry, dead letters, artifacts, nodes/agents/models. Добавлены Qt Test utility tests, desktop/AppStream metadata, CPack TGZ/DEB/RPM и Ubuntu workflow.
- Qt verification: не выполнена локальная C++ сборка. `cmake -S clients/asgend_qt ...` остановился на `find_package(Qt6)` потому что Qt6Config.cmake и SDK отсутствуют; apt repository update timed out. Поэтому нельзя считать UI unit tests или native runtime проверенными. Отдельный Qt-capable CI должен подтвердить compile/test.
- Existing core validation at this iteration: quick CTest 16/16 passed; API contracts 10/10; Control Plane 37/37; node-agent 30 passed / 1 skipped. The long integration test remains excluded from the quick CTest run.

Ближайшие направления:

1. **A0 — API/архитектурные контракты:** task/event/artifact schema, идентичность Execution Node, auth/RBAC и граница Control Plane / Execution Plane.
2. **Продолжить M1 и реализовать M2–M4 — память на C:** базовое UTF-8/OKF extraction добавлено в 0.9.1, decision metadata + title/description pipeline в 0.9.2, embedding stage + FTS5/BM25 + hybrid query в 0.9.3, граф + agent retrieval integration в 0.9.4; остаются специализированные extractors, chunk retrieval, query classification и metadata filters.
3. **S1–S2 — Docker Control Plane:** удалённые команды и состояния, доставка на внешний Execution Node, artifacts, durable storage, TLS/RBAC/audit и безопасная эксплуатация; без orchestration/LLM/model runtime на сервере.
4. **Q1 — Qt 6 Linux client:** отдельное приложение для основных Linux-дистрибутивов, локальный и защищённый удалённый режимы, task/event/artifact workflows и packaging.
5. **A1 — Android client:** доведение существующего Flutter source tree до воспроизводимой Android-сборки, защищённого pairing, удалённой работы с задачами и артефактами.
6. **S2/M4 — production hardening:** quotas, audit/metrics, backup/restore, incremental indexing performance и проверка масштабирования.

Остальные функции перечислены как будущая работа и не являются частью версии 0.9.4. Порядок и Definition of Done см. в `ROADMAP.md`.


## Delivered in 0.9.1 — M1 text extraction and read-only OKF metadata

1. Added `src/memory/extract.c` and public `asgend_memory_extract()` API; CLI supports `asgend-memory extract ROOT` with source/text size limits and `--retry`.
2. Migrated SQLite schema v1→v2 by adding `file_contents` and `file_metadata` while preserving all existing scan/hash/event records.
3. Supported UTF-8 text/code/config extraction with a 4 MiB default persisted-text cap, 64 MiB hard extraction cap, binary detection, and explicit `unsupported`, `binary`, `truncated`, `read_error`, `stale_source` statuses.
4. Markdown reader extracts body separately and parses existing OKF YAML frontmatter into JSON metadata rows (including flow/block scalar lists and nested `sources` list maps) while preserving raw frontmatter and never writing to sources. Full YAML 1.2 features outside the implemented safe subset are marked `parsed_partial`; raw header is preserved.
5. Per-file results are transactional, keyed to source SHA-256, and skipped when a successful result for the same hash exists. A changed path/content detected between scan and extract is not indexed until rescan.
6. Expanded `tests/test_memory_index.py` and CTest coverage for OKF parsing, metadata updates, source-byte preservation, unknown-extension text sniffing, binary/PDF handling, UTF-8-safe truncation, post-scan symlink replacement and legacy schema migration.

Not included in 0.9.1: PDF/Office specialized extractors, decision-model metadata classification, generated title/description, embeddings, graph, BM25/semantic retrieval and orchestrator integration.


## Delivered in 0.9.2 — M2 decision metadata and title/description

1. Added `src/memory/pipeline.c` and `asgend_memory_enrich()` API plus CLI `asgend-memory enrich ROOT`; configuration uses OpenAI-compatible Chat Completions URLs/models, with optional API keys from environment only.
2. Migrated SQLite schema v2→v3 while preserving file IDs and existing extraction/frontmatter data. Added `pipeline_jobs` for durable stage status/retry/backoff, `model_metadata` for decision-model JSON, and `descriptions` for generated titles/descriptions.
3. Implemented versioned prompts, response JSON parsing/normalization, strict bounded metadata fields, source-hash/model/prompt provenance, dependency-aware description keys, and immutable input-file handling.
4. Added bounded file-level parallelism (1–16 workers), separate SQLite connection per worker, request timeout and 4 MiB response cap, no HTTP redirects, HTTPS/HTTP protocol allowlist, exponential retry backoff, and `--retry` override.
5. Added local mock OpenAI-compatible server tests covering two-stage output, persisted model metadata, idempotent rerun (zero extra API calls), model-profile invalidation, concurrency, API-key non-persistence, request failure/backoff and retry.

Not included in 0.9.2: embeddings, PDF/Office-specific extractors, graph indexing, FTS5/BM25, semantic retrieval and orchestrator context injection. The endpoint profiles used by this standalone CLI must implement OpenAI-compatible `/chat/completions`; a typed System One endpoint alone is not currently compatible with this generic pipeline.


## Delivered in 0.9.3 — M3 embeddings and hybrid query

1. Added a bounded parallel embedding stage using the OpenAI-compatible `/v1/embeddings` endpoint. Vectors are keyed by description ID, exact input SHA-256, and embedding model ID; model jobs keep retry/backoff state.
2. Migrated SQLite schema v3→v4 transactionally, adding `embeddings`, external-content FTS5 `memory_fts`, `memory_search_docs` and triggers to keep search documents in sync with source content, OKF metadata, file paths and generated descriptions.
3. Added `asgend-memory query`: local FTS5 BM25 works without a model endpoint; optional query embeddings add cosine-similarity candidates and fuse ranks with Reciprocal Rank Fusion. Query text is not persisted.
4. Added `tests/test_memory_embedding.py`: mock API contract, parallel work, idempotency, re-embedding changed descriptions, hybrid semantic ranking, BM25-only retrieval, auth transport checks, and source no-mutation regression coverage.

Remaining: file dependency graph, chunk-level offsets, query classifier/metadata-filter UI, approximate-nearest-neighbor indexing and passing the retrieved context into the original orchestrator/model route.


## Delivered in 0.9.4 — graph and automatic retrieval

- Schema v5 / `file_edges` migration with source/target foreign keys and edge types.
- `asgend-memory graph ROOT` indexes local Markdown/wiki links, C/C++ quoted includes, and relative JS/TS imports.
- Query results now expose excerpt and related graph edges, and graph-expand candidate documents while down-weighting graph-only matches.
- Agent execution automatically adds local-only BM25 evidence to the selected model's system context when a project memory DB already exists; no alternate model route and no embedding HTTP calls are used by this implicit retrieval.
- Regression checks cover migrations, graph idempotence/stale edge removal, graph expansion and end-to-end evidence appearing in the selected model request.


## Delivered in 0.9.5 — source-aware chunk retrieval

1. Added SQLite schema v6/v7 migrations for `file_chunks`, `file_chunk_state`, FTS5 fragment index, and original source byte/line offsets after OKF frontmatter; legacy databases keep existing file IDs and are migrated transactionally.
2. Added resumable `asgend-memory chunk` with UTF-8-safe boundaries, paragraph/newline-aware split points, overlap, per-file/version state and stale chunk cleanup.
3. Switched lexical BM25 query to chunk-level FTS and added exact source coordinates to each result. The chunk FTS title/description/path/metadata columns are updated through triggers after document/model metadata changes.
4. Added `--type`, `--tag`, `--path-prefix`, and `--metadata KEY=VALUE` query filters; graph-expanded neighbors are subject to the same filters.
5. Added `--context-tokens`/`--chunk-tokens` and a conservative output-packing estimate. The agent path uses local-only retrieval and caps memory evidence at 1200 estimated tokens. The byte/3 estimate is not tokenizer-exact.
6. Added `tests/test_memory_chunk_query.py` and a pipeline regression proving generated titles are searchable even when chunks were built before the model enrichment pass.

Remaining memory items: query classification (optionally using decision model without turning retrieved content into instructions), specialized PDF/Office extraction, and chunk-level semantic vectors.


## Delivered in 0.9.6 — chunk embeddings and query intent

1. Schema v8 adds `chunk_embeddings` keyed by source-aware chunk, input hash and embedding model, with consistent dimension checks shared with document vectors.
2. `asgend-memory embed --chunks [--chunk-tokens N]` can generate vectors for title + description + chunk text with concurrent workers. The default `embed` behavior remains backward-compatible and creates only document vectors unless `--chunks` is supplied.
3. Hybrid query now searches both document embeddings and current chunk embeddings; semantic chunk hits expose the matched excerpt and source coordinates. Source/input hash validation rejects stale vectors.
4. Added local query-intent heuristics and intent-aware graph candidate ordering/weighting. JSON query response explains its `intent`, `method`, `reason`, and graph edge family. Retrieved documents remain untrusted evidence; this local classifier does not call a model.
5. Query no longer overwrites an existing chunk size when `--chunk-tokens` is omitted; it infers a stored chunk size if stale sources need rebuilding. This preserves chunk vector identity during automatic agent retrieval.
6. The main agent evidence pack includes query classification, graph provenance and rank/source metadata, while retaining local-only BM25 to avoid hidden model/API calls during task execution.

Remaining: optional decision-model query classification/reranking, PDF/Office extractors, benchmarked graph ranking, ANN/vector index for very large corpora, Qt/Linux client, Android, and Docker Control Plane.


## Delivered in 0.9.7 — constrained reranking and quality metrics

1. Added explicit `query --rerank` for bounded pre-retrieved top-K candidates through an OpenAI-compatible Chat Completions API. The reranker only sees aliases 1..N and cannot add arbitrary documents; unknown/duplicate IDs are ignored.
2. Added URL transport policy (HTTPS remotely; HTTP loopback only), environment-only API credentials, response-size caps, strict JSON parsing and opt-in network behavior. Default query and automatic agent retrieval do not invoke reranking.
3. Added a reusable judged-dataset evaluation CLI reporting Precision@K, Recall@K, MRR@K and nDCG@K per query and macro aggregate, with deterministic offline BM25 defaults.
4. Added regression tests against a local rerank mock and a synthetic judged corpus, including model output ID validation and source-file immutability.

Remaining after 0.9.7: ANN/vector index and scale benchmarks; PDF/Office extraction, Qt/Linux, Android and Docker Control Plane.


## Delivered in 0.9.8 — vector ANN and paired benchmark

1. Added schema v9 `ann_signature` columns and `(model_id,ann_signature)` indexes for document and chunk vectors. The migration is idempotent with column detection, preserving vectors and IDs.
2. Added deterministic 16-bit random-hyperplane LSH and radius-4 probe collection. The query switches to ANN only for >=5,000 valid indexed vectors and at least 32 approximate candidates; incomplete/small indexes and metadata-filtered queries use exact cosine. `--exact-vectors` can force reference search.
3. New embed results store ANN signatures transactionally; `asgend-memory ann-index ROOT` backfills legacy rows in bounded batches. JSON query output reports mode/counts to support observability.
4. Added `scripts/benchmark_memory_vectors.py` for paired exact-vs-ANN timing and Recall@K, and `tests/test_memory_ann.py` using 5,200 synthetic vectors to validate backfill, candidate reduction and top-1 consistency.

Remaining: production corpus benchmark and LSH tuning; HNSW/IVF remains a future option if measured results show insufficient recall/latency. Query planning and PDF/Office extraction have initial implementations; Qt/Linux, Android and Docker Control Plane acceptance remain roadmap items.


## Delivered in 0.9.9 — automatic resource/project memory lifecycle

- Replaced ordinary four-way parallel decision-model resource classification with local FTS5/BM25 over a separate `.asgend_mem/resources.sqlite3`. Categories `skills`, `system_prompts`, `tools` (including live MCP descriptions) and `knowledge` auto-sync by source hash before each subtask. Tool choices are still checked against the agent allow-list; relevance cannot grant permissions.
- Project memory `.asgend_mem/index.sqlite3` automatically scan/extracts before agent invocation, excludes root-level resource directories, refreshes the dependency graph on observed source changes and passes top-K snippets to the exact same configured agent model. No manual `scan`/`extract` needed during normal task use. Full hash scans run every five minutes to detect preserved size/mtime.
- Optional automatic decision-metadata/title-description and document/chunk embeddings are activated only when the corresponding model endpoint+model env pairs are configured; resumable hash-keyed jobs skip unchanged files. A configured embedding endpoint also enables hybrid project retrieval. Default is local BM25 with no hidden model/network call.
- The full integration acceptance path mutates both a resource and a project file after the server starts, then verifies that the next task context contains both new markers without restart or a manual CLI call. It also asserts there are no `purpose=resource_classifier` calls.


## Delivered — opt-in decision-model query planning

- Added `query --plan` and `src/memory/plan.c`. Only the query and the fixed intent allowlist are sent to the configured OpenAI-compatible decision endpoint; retrieved document content is never sent to the planner.
- The planner accepts a strict fixed JSON shape, caps rewrites and response size, validates intent against an allowlist, validates label filters and root-relative paths, rejects extra keys/code fences/trailing content, and refuses remote plain HTTP (loopback HTTP is allowed).
- Original query remains in BM25 input; validated rewrites supplement it. Explicit user filters override planner hints. Every endpoint, transport, response, or schema failure falls back to the local search and is reported in `query_plan.fallback_reason`. The feature is opt-in; automatic agent retrieval remains local-only.
- `tests/test_memory_query_plan.py` validates API payload, prompt-injection boundaries, rewrite/filter behavior, malicious path rejection, strict schema fallback, missing configuration fallback, and the fact that document content is not transmitted.
- Iteration 7 extends `scripts/evaluate_memory_retrieval.py` with `--plan` and paired `--compare-plan`. The paired report includes baseline and planned aggregate/query metrics plus `delta_planned_minus_baseline`. `tests/test_query_plan_eval.py` uses a deterministic mock planner to exercise the harness. Its synthetic labels validate plumbing only; a larger independent human-judged relevance evaluation is still required before recommending `--plan` broadly.
