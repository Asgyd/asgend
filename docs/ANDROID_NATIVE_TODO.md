# ASGEND Native Android — implementation TODO

Статусные значения: `[x]` подтверждено в репозитории/реализовано; `[ ]` ожидает реализации; `[~]` частично выполнено или требует внешнего запуска.

## Stage 1 — аудит и спецификация
- [x] Найти существующий Flutter Android runner и отделить его от требования Kotlin-native.
- [x] Инвентаризировать C CLI/TUI: task/To-do, chat/timeline tiles, files/Goals, runtime/model/MCP/config interactions.
- [x] Инвентаризировать дополнительные Flutter-функции: Control Plane, pairing, role-gated commands, artifacts/export.
- [x] Сопоставить функции с JSON-lines RPC и Control Plane OpenAPI.
- [x] Определить Android 17/API 37 и Material 3 Expressive baseline; отметить build-tool limitations среды.
- [x] Определить безопасность raw TCP/VPN, HTTPS origin validation, Keystore, polling/backoff, idempotency.
- [x] Оформить `ANDROID_NATIVE_AUDIT.md` и `ANDROID_NATIVE_DECOMPOSITION.md`.
- [x] Установить измеримые stage exit criteria.

## Stage 2 — native build foundation
- [x] Добавить самостоятельный `clients/asgend_android` с Kotlin/Compose Gradle project; не изменять/не удалять Flutter target.
- [x] Закрепить версии AGP, Gradle, Kotlin/Compose plugin, Compose BOM, Material3 и test deps.
- [x] Сконфигурировать `compileSdk=37`, `targetSdk=37`, `minSdk` с rationale, `INTERNET`, app ID, versioning, backup/data extraction rules.
- [x] Собрать Material 3 Expressive theme: dynamic color Android 12+, fallback light/dark, typography/shape.
- [x] Реализовать компактную bottom navigation и medium/expanded navigation rail.
- [x] Добавить Core settings UI, validation, понятное предупреждение TCP/VPN и явное отклонение распространённых loopback-адресов на Android.
- [x] Добавить Android Keystore-backed auth-token store и безопасное удаление секрета; ротация — после remote/session реализации.
- [x] Реализовать bounded `CoreRpcClient` для JSON-line TCP: absolute response deadline, malformed response, `ok=false`, socket close, no main-thread I/O.
- [x] Добавлены исходники unit-тестов settings/error redaction; чистая CoreSettings-модель дополнительно проверена локальным Kotlin compiler. Android/JUnit execution заблокирован отсутствием Gradle/Android SDK; network-protocol tests ждут injectable socket seam.
- [~] Добавлен CI workflow для API 37, unit tests и Debug/Release APK. В этой среде SDK/Gradle отсутствуют — фактическая сборка APK pending; не PASS. CI config только статически проверен.

## Stage 3 — Workspace + TUI capability parity
- [x] Task dashboard: list/filter/empty/loading/error, create with priority 0–9, stable idempotency key reused for same failed submission.
- [x] Task detail: poll status and events with `after_id`, deduplicate event IDs, coroutine polling cancels with the screen lifecycle, bounded backoff on repeated errors.
- [x] Chat timeline: high-level event cards for status/action/progress/tool/file/result; filter event types that explicitly identify private reasoning or raw model payloads.
- [x] Clarification: render option buttons plus free-text input; disable duplicate answers while a request is pending.
- [x] Follow-up submission with `session_id=task-<id>` matching the existing Flutter local client semantics.
- [x] Cooperative cancel confirmation; failed-task retry goes through server-side `retry_task` eligibility check.
- [x] Plan screen: global/task-scoped To-do and Goals; add/edit/toggle done/open/delete confirmation.
- [x] Files browser: task selection, path navigation and file/directory distinction; full text read falls back to bounded chunk preview for large files.
- [x] File editor: 1 MiB write cap, dirty-state discard confirmation, core-side path confinement and symlink rejection relied on rather than client path assumptions.
- [ ] Restore active task/session and screen after process death; ViewModel survives configuration changes but no durable restoration contract is implemented yet.
- [~] Repository operation contract tests added with injectable transport seam; JUnit execution and Compose UI tests blocked pending Android Gradle/SDK CI.


## Stage 4A — Runtime, models, agents, resources and MCP
- [x] Health dashboard: status counts, workers, active connections, draining, priority-aging interval.
- [x] Runtime registry: LLM + decision models; display status, enabled/running, resource estimates.
- [x] Actions model enable/disable/start/stop with progress and refresh; serialize overlapping actions.
- [x] Resources catalog and MCP connected/stopped/disabled states.
- [x] Agents list and model/profile linkage where reported by core.
- [x] Do not render raw config JSON, API keys, base URLs, last-error payloads, absolute directories or log paths; genericize runtime action errors.
- [x] Repository contract tests for model operation fields, allow-list and long timeout added (execution is pending Android CI).
- [x] Config read/save via guided schema/range-validated form is implemented in Stage 4B; it surfaces `restart_required` when returned by the core. Non-destructive core API remains open (see Stage 4B risk).
- [~] UI behavior tests still pending Android Gradle/SDK CI; no `src/androidTest` acceptance suite is present yet.

## Stage 4B — Guided configuration editor
- [x] Add range-validated fields for operational limits and sandbox policy; do not render secret/raw JSON fields.
- [x] Preserve non-editable values in transient in-memory config bundle and require explicit acknowledgement for sandbox weakening and legacy full-bundle write behavior.
- [ ] Replace legacy destructive `config_save` semantics with a non-destructive schema-validated partial update endpoint and backup/rollback.
- [~] Compose/JUnit/device acceptance pending Android SDK/Gradle.

## Stage 5 — remaining local CLI/TUI parity
- [x] Dead-letter queue uses the core `dead_letters` operation; list is bounded and retries continue through core-side eligibility checks.
- [x] Generated artifacts uses `artifact_list`; UI shows relative path, MIME, size and abbreviated SHA-256.
- [x] Artifact inventory explicitly warns when the core reports a truncated scan or skipped files; binary downloads use bounded `workspace_read_bytes` chunks, write to private cache and verify byte count + SHA-256 before enabling export.
- [x] LSP diagnostics uses the core operation with normalized relative-path checks, a bounded call and explicit confirmation; UI states diagnostics run on the core host and within its configured project root.
- [x] Workspace save dirty-state clears only after the core confirms a successful write.
- [x] To-do/Goals uses bounded `plan_wait` revision long-poll; the request is lifecycle-bound and has a client deadline beyond the core wait.
- [x] Binary artifact export is capped at 64 MiB, writes to private cache, verifies byte count and SHA-256 before Sharesheet export, and uses a FileProvider limited to the artifact cache directory.
- [x] Contract tests cover bounded dead-letter requests, task-scoped artifact listing, bounded binary-read response shape, path confinement, LSP bounds, plan revision notifications and invalid wait arguments.
- [x] Local Kotlin/JVM contract harness (API-compatible stubs) executed: 14/14 repository contract tests passed; Android/Compose/JUnit runtime is still pending CI.

## Stage 6A — secure Control Plane foundation and pairing
- [x] Separate HTTPS client (does not reuse local TCP RPC); strict HTTPS URL, no user-info/query/fragment, safe base-path normalization, no redirects, default platform TLS verification.
- [x] Bounded JSON response body, request IDs, API version check and generic HTTP/network errors; bearer is only sent in Authorization header.
- [x] Pairing redeem sends the one-time code only in the POST body, checks response against `ucp_` token schema and allowed role values, and never logs/stores the code.
- [x] Remote user token uses a distinct Android Keystore alias and encrypted preference slot; local core token remains independent. Server revoke is attempted on sign-out and local remote credentials are cleared even when remote access is offline.
- [x] `/v1/me` workspace membership/role and workspace nodes are rendered in the Settings → Remote Control Plane section; unauthorized sessions clear the local remote credential.
- [x] API contract tests: HTTPS/base-path validation, no-token-before-network, pairing payload, bearer-header behavior, response version/redirect policy, node path encoding, malformed token rejection. API-compatible local harness: 7/7 PASS.

## Stage 6B — remote workspace/task parity
- [x] Workspace switching based on refreshed `/v1/me` membership; persist the selected workspace ID/role without re-encrypting or replacing the token; clear workspace-bound task/event state when switching.
- [x] Remote task list pagination/filtering uses the canonical `state`, `node_id`, `limit`, opaque `cursor` fields and includes the task states declared by the v1 schema.
- [x] Remote task creation uses the exact OpenAPI request shape and a stable idempotency key for retries of the same unconfirmed request. A confirmed 200/201 acceptance is distinguished from a later list refresh failure.
- [x] Task-detail/event long polling uses `after_event_id`, bounded `wait_ms`, an extra client deadline, lifecycle cancellation, deduplication, monotonic cursor and server cursor-reset handling.
- [x] Unknown event types advance the cursor without rendering unknown data; UI event summaries are allow-listed and raw event payloads are not retained in the view model.
- [x] Answer/cancel/retry use exact v1 routes and idempotency request bodies; membership is refreshed before each mutation; only `owner`/`operator` can mutate; cancellation/retry require confirmation; no offline command queue.
- [x] Session expiry/401 clears the remote credential without touching local-core credentials; 403 is distinguished from an offline state.
- [x] API-compatible Kotlin/JVM harness (local stubs, not Android JUnit): 14/14 contract tests PASS; source verifier and six XML parse checks PASS.
- [ ] Persist event cursor/history and selected task/workspace through process death with a bounded privacy-reviewed cache. Also persist pending idempotency keys/signature hashes before remote mutation to avoid duplicates after ambiguous response + process death.
- [~] Android Gradle/JUnit/Compose compile and device acceptance remain unverified; local authoring environment lacks Gradle/wrapper JAR and Android SDK/platform 37.

## Stage 6C — remote artifact delivery
- [ ] Artifact inventory/state; ticket request; same-origin download URL policy and no token on ticket stream.
- [ ] Bounded streaming with declared size, SHA-256 and state checks; expiry/resume/error behavior based on OpenAPI schema.
- [ ] Private-cache export/share and executable/active content confirmation; cleanup incomplete downloads and cap local export size.
- [ ] Schema contract tests from `contracts/v1/openapi.yaml` and the associated JSON schemas.

## Stage 7 — quality, platform and release
- [ ] UI state matrix: offline/connecting/unauthorized/server error/empty/loading/running/waiting_input/failed/completed.
- [ ] Accessibility: semantics, TalkBack labels, touch targets, scalable text, contrast, keyboard navigation.
- [ ] Responsive layouts: phone portrait/landscape, small window, tablet/split-screen, navigation rail/list-detail.
- [ ] Android 17/API37 behavior checks: shortcuts, IME rotation, edge-to-edge, back/navigation changes.
- [ ] Compatibility smoke on representative previous supported Android releases.
- [ ] Compose UI tests and instrumented smoke; avoid exact pixel assumptions when dynamic color is enabled.
- [ ] Performance: no blocking I/O on main thread, poll cancels correctly, large workspace files are chunked.
- [ ] Security checks: APK manifest, cleartext policy, secret storage, network origin validation, no secrets in logcat.
- [ ] CI workflow: lint/static analysis, unit tests, API 37 build, APK artifact + SHA-256; release signing externalized.
- [ ] Final 3-pass local review (architecture/security, UX/accessibility, test/package regressions); no claims of external subagent review unless available.
- [ ] Final cumulative archive, per-stage archives and checksum verification.

## Audit outcome (2026-10-25)

- [x] Three sequential local audit passes are documented in `ANDROID_NATIVE_AUDIT_ITERATION_1.md`, `_ITERATION_2.md` and `_ITERATION_3.md`; these are not an independent external subagent review.
- [ ] Stage 6C Control Plane artifact ticket/stream/export is not implemented.
- [ ] Durable idempotency keys and task/event cursor restoration after process death are not implemented.
- [ ] Native Android Gradle/JUnit/Compose/APK/device acceptance remains blocked/unverified in the authoring environment.
- [ ] Compose UI/accessibility tests and backend-contract CI triggers need strengthening.

## Current stage status

- Stage 1 (audit/spec): COMPLETE.
- Stage 2 (native build foundation): IMPLEMENTED IN SOURCE; API 37 APK/Compose compilation and tests remain PENDING external Android CI because this environment has no SDK/Gradle.
- Stage 3 (local CLI/TUI capability parity): CORE WORKFLOWS IMPLEMENTED IN SOURCE; process-death restoration and execution of Android unit/UI tests remain pending.
- Stage 4A (runtime/models/agents/resources/MCP): IMPLEMENTED IN SOURCE; Gradle/Compose/JUnit/device validation pending.
- Stage 4B (safe guided config editor): IMPLEMENTED IN SOURCE; Gradle/JUnit/Compose UI validation pending. Full-bundle save semantics still need a narrower/non-destructive core endpoint.
- Stage 5 (remaining local CLI/TUI operations): IMPLEMENTED IN SOURCE; repository contract harness 14/14 PASS and source/XML checks PASS; Gradle/JUnit/Compose UI validation pending.
- Stage 6A (HTTPS foundation/pairing) is implemented in source; API-compatible client harness 7/7 PASS.
- Stage 6B (remote workspace/task parity) is implemented in source; API-compatible HTTPS client harness 14/14 PASS; Android Gradle/Compose/JUnit/device validation remains pending.
- Stage 6C (remote artifact delivery) and Stage 7 (platform/accessibility/release) remain pending.

## Acceptance rule

A task is complete only when code and its targeted tests exist, observed commands/results are recorded, docs updated, and the stage archive has been extracted and integrity checked. If Android SDK/platform 37 or emulator is unavailable, report the blocked external validation specifically and do not mark APK/device build as passed.
