# ASGEND Native Android — декомпозиция реализации

**Базовая дата:** 2026-10-25. **Целевая сборка:** отдельное Kotlin/Jetpack Compose приложение под API 37.

## Архитектура

```text
Compose UI (Material 3 Expressive)
  ├── Workspace / Tasks / Chat / Timeline
  ├── Plan (To-do + Goals)
  ├── Files / Preview / Editor
  ├── Runtime (health / models / resources / MCP / agents)
  ├── Control Plane (workspaces / pairing / remote tasks / artifacts)
  └── Settings / Connection / Appearance
           │
           ▼
  ViewModels + StateFlow (loading/error/empty/content; no overlapping polling)
           │
           ▼
  Domain models + repositories
      ├── CoreRpcClient: JSON-lines TCP, bounded response, timeouts
      ├── ControlPlaneClient: HTTPS API v1, endpoint guard, auth/roles
      ├── SecretStore: Android Keystore-backed encryption
      ├── SettingsStore: nonsecret preferences
      └── ArtifactRepository: bounded streaming + SHA-256 + safe export
           │
           ▼
  Existing ASGEND C core / existing HTTPS Control Plane
```

## Слой UI

### 1. Основная навигация

- `Workspace` — tasks, create task, search/filter/status, active task/session.
- `Plan` — global/task-scoped To-do and Goals; add/edit/done/delete.
- `Runtime` — health, queue statuses, workers/draining, model status/resources, MCP status, agents.
- `Files` — workspace browser/read/edit/write (enabled when task selected).
- `Settings` — connection, token rotation/deletion, appearance and security explanations.
- `Remote` — optional/independent Control Plane connection, pairing, workspace switch and logout.

Compact mobile should use bottom navigation with a maximum of five primary destinations. Remote/Settings can be secondary destinations within a profile/app-bar menu if screen width forces a five-destination limit. Expanded windows should use a rail/drawer, and for task detail support list + conversation + files/plan panes.

### 2. Task detail and TUI tiles

Map event kinds to UI timeline variants rather than terminal output:

- user/assistant text → chat bubble/markdown-like selectable text;
- orchestration/progress → compact progress cards;
- question → prominent question card with typed options and free text;
- tool call → tool badge + status + safe summary, never raw intermediate arguments;
- file/workspace events → file tile with open action;
- action/system notices → inline notice;
- thinking → activity indicator only, never reasoning text;
- terminal statuses → state badge and contextual retry/cancel affordances.

Polling: `/status` plus `/events(after_id, limit)` using monotonic event ID, no duplicate events, cancellation on lifecycle disposal, backoff on network failures. Follow-up uses the protocol's `submit` session ID semantics from the existing client; explicit question answers use `answer` with `task_id`, `answer`, and optional `option_id`.

### 3. Data/API layer

#### Local core TCP operations confirmed in `src/server.c`

`ping`, `health`, `list_agents`, `submit`, `status`, `list_tasks`, `events`, `answer`, `cancel`, `retry_task`, `dead_letters`, `workspace_list`, `workspace_read`, `workspace_read_chunk`, `workspace_read_bytes`, `workspace_write`, `artifact_list`, `plan_list`, `plan_add`, `plan_update`, `plan_delete`, `plan_wait`, `config_get`, `config_save`, `models`, `model_enable`, `model_disable`, `model_start`, `model_stop`, `resources`, `mcp_status`, `lsp_diagnostics`.

Every request must include `op` and include `auth_token` when required. Input/output boundaries must be bounded; socket connect/read timeout and malformed JSON are explicit errors. TCP auth does not provide transport encryption.

#### Control Plane HTTPS

Use `contracts/v1/openapi.yaml` plus schemas as canonical; do not derive request shapes from UI assumptions. Port existing Flutter API behaviors: pairing create/redeem, session/workspaces, tasks/events with cursor, role-gated commands/cancel/retry/answer, artifacts/tickets/streaming/checksum/export, logout/revoke. This is a distinct API; do not send local RPC payloads to remote API.

### 4. Android platform layer

- `MainActivity` + Compose; no Flutter embedding in native module.
- `compileSdk 37`, `targetSdk 37`, baseline `minSdk 24` unless plugin/platform validation reveals a necessary change.
- Gradle/AGP/Kotlin/Compose versions pinned to stable APIs compatible with API 37. Do not use floating dependency versions.
- Material 3 stable 1.4.0 as at 2026-10-25, Compose BOM pinned, Android 12+ dynamic colors with fallback theme.
- Android Keystore AES/GCM for persisted auth token; nonsecret endpoint settings in private app storage.
- `INTERNET` permission, cleartext HTTP disabled; raw TCP still requires explicit VPN/SSH acknowledgement since it does not use HTTP cleartext policy.
- Lifecycle-aware flows and cancellation; process death/restoration for current navigation/task ID, not secrets in saved state.
- Edge-to-edge bars, back handling, keyboard/IME, landscape, large fonts, TalkBack, touch target and adaptive width classes.

## Stage boundaries and exit criteria

### Stage 1 — Audit / spec (documentation)
Exit: audit maps source, UI/design surfaces, API ops, security requirements, dependencies, Android 17 target behavior and acceptance criteria; archive is reproducible.

### Stage 2 — Native foundation
Exit: standalone Gradle project, API37 build settings, Material 3 Expressive theme, adaptive navigation shell, secure settings, CoreRpcClient; tests for serialization, error mapping, credential encryption round-trip and endpoint validation. APK build required when SDK is available.

### Stage 3 — Core workspace workflows
Exit: task create/list/open, event timeline, question answer, follow-up, cancel/retry; plan To-do/Goals; workspace files list/read/write with server-side safety intact; unit/UI tests.

### Stage 4 — Runtime administration
Exit: health, models/decision models, enable/disable/start/stop, resources, MCP status, agents/config view/save/restart-required banner; no unsafe raw JSON success fallback; tests for role and state.

**Implementation note (2026-10-25, Stage 9):** Runtime cards/actions and the allow-listed guided config form are implemented in source. Android sends only a `settings` object to the new `config_update_settings` core RPC. The endpoint rejects unknown sections/keys, invalid JSON types and out-of-range values, validates the merged full candidate with `config_load`, stages a private rollback copy and performs a single-file atomic replacement with directory durability check. The legacy `config_save` path explicitly preserves hidden fixed-role agent profiles. Runtime regression evidence exists; Android Gradle/Compose/JUnit/device acceptance remains pending Android SDK CI.

### Stage 5 — remaining local CLI/TUI parity
Close the last local-core operations not yet reachable from native screens:
- Dead-letter inspection and server-validated retry.
- Generated artifact inventory with SHA-256/MIME/size, scan truncation and skipped-entry counters.
- Bounded binary artifact export using `workspace_read_bytes`, stable size/hash verification, private temporary cache and FileProvider/ Sharesheet only after successful validation (64 MiB client limit).
- LSP diagnostics only for a normalized relative file path inside the core-configured project root; explicit confirmation and bounded output.
- `plan_wait` long-poll keyed by the shared core revision so plan changes from CLI/TUI become visible without tight polling.
- Ensure edit dirty-state is cleared only after confirmed writes.

Exit criteria: operation request contract tests, traversal/absolute path rejection, artifact metadata/truncation surfaced, verified bounded binary export, shared plan revision notification, static security source check and clean stage archive. Current source/API-compatible repository harness: 14/14 pass; all six Android XML files parse. Android Gradle/Compose/JUnit and device testing remain external when the SDK is unavailable.

### Stage 6A — Control Plane transport and pairing
Exit: independent HTTPS transport, endpoint policy, no redirects, token validation and distinct Keystore slot, pairing redeem, `/v1/me`, membership/role and node list, sign-out/revoke. Contract tests and source security checks required.

### Stage 6B — remote workspace/task parity
**Implemented in source:** workspace selector from refreshed `/v1/me` membership, cursor/filter task listing, stable idempotent task submission, remote task detail, `after_event_id` event long-poll with lifecycle cancellation, deduplication, cursor reset support, safe typed event summaries, answer/cancel/retry using canonical command bodies, fresh role check before every mutation, confirmation for cancel/retry, and distinct 401/403/offline handling. Local API-compatible contract harness 14/14 PASS; this is not Android Gradle/JUnit/device acceptance.

Exit: feature operations match `contracts/v1/openapi.yaml`; Android API37 Compose compile and UI tests pass in CI; role-change, process death, network interruption, cursor reset, viewer read-only and command idempotency are tested. Remote cursor persistence across process death remains open.

### Stage 6C — remote artifact delivery
Exit: artifact list/state, download ticket and same-origin policy, streaming size/SHA-256 validation, private cache, safe export/share, temporary-file cleanup and schema tests.

### Stage 7 — quality / release
Exit: local unit tests, Compose semantics/UI tests, Android API 37 emulator/device smoke, Android 14/15+ compatibility smoke, dynamic colors/light-dark, rotation/IME, offline/slow network, strict-mode/no-main-thread I/O, accessibility check, reproducible unsigned APK and checksum; no signing secrets committed.

## Design review rubric

1. Fidelity of capability, not terminal pixel imitation.
2. One-hand reachability and readable mobile hierarchy.
3. Adaptive pane use at expanded widths.
4. Expressive theme without status-only color coding.
5. Predictable motion and system reduced-motion behavior.
6. Screen-reader semantics and keyboard/switch navigation.
7. Explicit network trust boundary and credential storage.
8. Robust state restoration, no duplicate polls/duplicate submissions.
9. Distinct task states and no accidental cancellation or config-save.
10. Architecture separates UI, local RPC, remote HTTPS and Android secrets.
