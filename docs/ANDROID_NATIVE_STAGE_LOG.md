# ASGEND Native Android — stage log

## Stage 1 — source/design audit
- Analyzed `clients/asgend_cli.c`, `src/asgend_tui.c`, `include/asgend_tui.h`, `src/server.c`, existing Flutter client and Control Plane contract/docs.
- Added: `docs/ANDROID_NATIVE_AUDIT.md`, `docs/ANDROID_NATIVE_DECOMPOSITION.md`, `docs/ANDROID_NATIVE_TODO.md`.
- External source verification: Android 17 = API 37; Material3 stable 1.4.0 on 2026-10-21; adaptive stable 1.3.0 on 2026-10-07; current Compose BOM guide lists 2026.09.00.
- Validation limitation: no `ANDROID_HOME` / `ANDROID_SDK_ROOT`, no Android `android.jar`, no Gradle executable/cache and no Flutter SDK in the active runtime. APK build cannot be run here.
- Independent agent: not available in tool environment; do not describe local review as independent subagent feedback.
- Result: audit/spec/TODO are complete; native implementation is separate subsequent stage work.

## Stage 2 — native Android foundation (source implementation)
- Added standalone `clients/asgend_android` Kotlin/Compose project targeting `compileSdk/targetSdk 37`, `minSdk 24`; Flutter client remains unchanged.
- Pinned AGP 9.4.1, Gradle 9.6.0, Compose compiler plugin 2.4.10, Compose BOM 2026.09.00 and Material3 1.4.0. Versions selected from official Android/AndroidX docs checked 2026-10-25.
- Added manifest/backup exclusions, cleartext HTTP disabled, Material 3 Expressive theme with dynamic color/fallback light-dark, compact NavigationBar + expanded NavigationRail, connection dialog and initial shell destinations.
- Added AES/GCM token encryption with Android Keystore, local preferences for non-secret settings, bounded JSON-line TCP client with 5s connect and absolute response deadline, server error handling/redaction.
- Added unit tests for connection validation/redaction and native Android CI workflow.
- Static checks: XML resources parse; `CoreSettings.kt` compiles with available Kotlin compiler; Kotlin source parser probe found no syntax-level `expecting` diagnostics. Full `kotlinc`/Gradle compile is not possible because AndroidX/Android SDK libraries are absent.
- Build limitation: no `gradle`, `gradle-wrapper.jar`, Android SDK/platform `android-37`, emulator, or external network access in this environment. CI/build is therefore pending and must not be called PASS. The project README documents generating the standard Gradle wrapper in a connected Android development environment.
- Stage 2 is source-complete but binary-build validation is pending.


## Stage 2 — native build foundation (2026-10-25)

**Implemented in source:** independent `clients/asgend_android` Gradle project targeting Android API 37, Compose Material 3 Expressive, adaptive navigation shell, Material theme/dynamic color, connection settings screen, Keystore-backed token storage, bounded JSON-line TCP RPC client, basic connection state ViewModel, Android manifest cleartext/backup hardening, settings unit test sources, and CI workflow for Android SDK build/test/APK artifacts. Existing Flutter client and C core are preserved.

**Checks actually run in this environment:** XML parse PASS for manifest, themes, backup/data extraction and network security XML; static Gradle values/security assertions PASS; `CoreSettings.kt` compiles with local Kotlin CLI; archive extraction/integrity checked. **Not run:** Gradle unit tests, Compose/Kotlin Android compilation, APK build, emulator/instrumented tests — no Gradle executable/wrapper JAR or Android SDK/platform 37 is installed in this authoring environment. CI must establish those results.

**Open stage risk:** network protocol tests need a test seam/local mock server, and RPC DTOs/workflow state still need implementation during Stage 3. Native UI shell is not yet feature-parity with C TUI.


## Stage 3 — local CLI/TUI workspace capability parity (2026-10-25)

**Implemented in source:** core-backed task dashboard (status filters, priority 0–9, idempotency key), task detail/timeline polling through `status` + `events(after_id)` with de-duplication, typed clarification options and free-text answer, follow-up using the existing `task-<id>` session convention, cancel confirmation and server-validated failed-task retry. To-do/Goals supports global and task scope, add/edit/mark done/reopen/delete. Files supports selecting task workspace, directory navigation, text preview, fallback to bounded `workspace_read_chunk`, and writing UTF-8 text within the core 1 MiB limit with dirty-state confirmation. The UI filters event kinds that explicitly denote private reasoning/raw model payloads.

**Quality improvement:** `CoreRepository` now depends on `CoreRpcTransport`, enabling request-contract tests to capture exact operation/field names without real sockets. Connection validation rejects common device-loopback spellings on Android, and the event timeline filters internal/private/reasoning/prompt-payload event-type families. Added tests for task submission idempotency/session fields, event cursor, plan/workspace operations, oversized write rejection, and question/event parsing.

**Checks actually run:** clean Kotlin CLI compilation for `CoreSettings.kt`; parser scan over all Android Kotlin/test sources produced no Kotlin syntax/`expecting` diagnostics (full compiler result is not a build: AndroidX/Compose/Android/JUnit dependencies unavailable); API operation names statically cross-checked against `src/server.c`; all Android XML files parsed and security assertions rerun. Gradle/JUnit/Compose build and UI tests remain pending in Android CI.

**Not complete:** process-death restoration of active task; runtime/models/agents/resources/config screens; HTTPS Control Plane; actual APK and device/emulator validation.

## Stage 4A — runtime, models, agents, resources and MCP (2026-10-25)

**Implemented in source:** the Runtime destination is now backed by the core API rather than a placeholder. Sections cover queue/worker health, task counts by status, model/decision-model states and resource estimates, model enable/disable/start/stop actions, agents, resource catalog file names/counts and MCP state. Actions are mutually exclusive, use the core operation and kind allow-list, have a long operation timeout and refresh from core state afterward. UI intentionally does not render raw config JSON, model credentials/API keys, base URLs, last-error payloads, local file paths, or log paths. Runtime failure messages are generic to avoid reflecting potentially sensitive server error strings into the UI.

**Checks added/updated:** repository contract tests cover model action operation, model ID/kind fields, 180-second timeout and rejection of unsupported operation/kind before a network call. Source audit confirmed the UI consumes only allow-listed model/agent/resource/MCP fields and that operation names match `src/server.c` (`health`, `models`, `list_agents`, `resources`, `mcp_status`, `model_enable/disable/start/stop`).

**Not run:** Gradle/JUnit/Compose build and Android device tests remain blocked because the active environment has no Gradle executable/wrapper JAR and no Android SDK API 37. The feature is implemented in source; Android compilation is not claimed as PASS.


## Stage 4B — guided configuration editor (2026-10-25)

**Implemented in source:** the Runtime destination now includes a Config section with editable, bounded operational settings only: server connection/timeout limits, runtime workers/queue/retry/priority-aging values, sandbox enabled/require-Landlock/network policy, and CPU/memory/file/open-file limits. Client-side ranges match `src/config.c` validation. Secret fields, endpoints, paths, the full raw JSON and model/agent registry are never rendered. The unmodified config/model/agent bundle is held only in the ViewModel's private transient memory so the existing `config_save` contract can preserve fields not editable in Android; it is not written to preferences or SavedState and errors are generic.

**Safety behavior:** the form flags `restart_required`, explains that the legacy core endpoint saves main config + model registry + agent profiles as one bundle, warns that the endpoint may delete legacy fixed-role agent files omitted by `config_get`, requires explicit acknowledgement of that bundle side effect, and requires separate acknowledgement when changing sandbox settings in a way that weakens policy. Reloading dirty input asks for confirmation. Cancellation exceptions propagate correctly rather than being swallowed.

**Tests/validation:** added `RuntimeConfigDraftTest` for preservation of hidden tokens/paths, exact allowed-field mutations, default deny-network policy, and core config range boundaries. Kotlin ViewModel/repository/config-draft plus repository/config test sources pass a narrow type-check against local API-compatible stubs. The dependency-free Python source-contract checker passes and is run in Android CI before Gradle. All XML resources parse. A Kotlin source parser scan shows no parser diagnostics. **Not run:** JUnit/Compose/Gradle compilation, APK assembly or device tests because Android SDK 37 and Gradle are not installed in this runtime.


## Stage 5 — remaining local CLI/TUI operations (2026-10-25)

**Implemented in source:** a `dead_letters` task filter backed by the core's terminal-failure list and existing validated retry path; task-scoped artifact inventory through `artifact_list`; artifact cards showing relative path, size, MIME and abbreviated SHA-256; visible scan-truncated and skipped-entry warnings; binary artifact export through bounded `workspace_read_bytes` chunks, SHA-256 and length verification, private cache and a non-exported FileProvider; and a separate LSP diagnostics form which explains execution is on the core host and confined to its configured project root. Client path validation rejects absolute/drive-like separators, backslashes, NUL and `.`/`..` components, while the core independently resolves the path and enforces root confinement, regular-file and 5 MiB constraints. Fixed file editor dirty-state so it clears only after a successful write.

**Contract tests added and executed:** bounded `dead_letters`, artifact list task scoping, `workspace_read_bytes` request/response and chunk constraints, path validation, LSP relative-path request and rejection of absolute/traversal paths, `plan_wait` revision/deadline and invalid-parameter rejection. A standalone Kotlin/JVM harness using local API-compatible stubs compiled the repository and contract tests; **14/14 passed**. This is not Android Gradle/JUnit acceptance. All six Android XML files parse and the dependency-free source verifier passes.

**Additional review correction:** the existing Runtime screen dispatched `RuntimeSection.Configuration` but the enum omitted that member, which would fail compilation when the relevant Compose file is compiled. Added `Configuration("Config")` and a static source-contract assertion so the enum and dispatch stay aligned.

**Artifact integrity/export:** binary content is requested in bounded 256 KiB chunks (core API cap 1 MiB), written into app-private `cache/artifacts`, SHA-256 and byte count are verified against inventory metadata, file changes/no-progress are rejected, and export uses a non-exported `FileProvider` whose paths XML exposes only that cache subdirectory. Client export cap is 64 MiB. To-do/Goals now uses lifecycle-bound `plan_wait` revision long-poll with 2-second backoff on temporary errors rather than a tight repeated fetch loop.

**Not run:** full Gradle/JUnit/Compose compilation, APK build and device acceptance remain unrun because Gradle and Android SDK/platform 37 are unavailable.

**Next:** Stage 6 builds separate HTTPS Control Plane client functionality (pairing/workspaces/remote tasks/role gate/artifact download verification) without mixing HTTPS with raw TCP core RPC; Stage 7 is final accessibility/platform/security review and cumulative packaging.


## Stage 6A — HTTPS Control Plane and pairing (2026-10-25)

**Implemented in source:** `ControlPlaneClient` is separate from `CoreRpcClient`; it enforces HTTPS-only endpoint policy, rejects user info/query/fragment/path traversal, normalizes `/v1` suffixes, disables redirects, relies on platform TLS validation, caps JSON responses at 2 MiB, adds request IDs and checks API version. It supports health, pairing redeem, `/v1/me`, workspace nodes and server-side logout. The remote token is validated against the OpenAPI `ucp_` pattern and stored under its own Android Keystore alias/preference key; local-core token remains independent. Pairing code is not persisted or placed in a URL. Remote settings UI shows identity, membership/role and nodes.

**Contract tests:** 7/7 API-compatible Kotlin/JVM tests passed: normalized HTTPS endpoint, reject insecure/user-info/query/traversal endpoints, unauthenticated pairing request, Bearer header only, no token → no network call, API version/redirect behavior, node path segment encoding and malformed short token response. Note the test cases count is seven because endpoint rejection conditions are grouped into one test.

**Not run:** Android Gradle/Compose compilation, official JUnit task, APK build and device acceptance remain blocked by the absent Gradle installation/wrapper JAR and Android SDK/platform 37. This stage covers remote connection and identity only; remote tasks/events and remote artifact downloads remain Stages 6B/6C.


## Stage 6B — remote workspace/task parity (2026-10-25)

**Implemented in source:** workspace switching persists only the selected workspace metadata; remote task filter/pagination follows the OpenAPI v1 `state`, `node_id`, `limit`, `cursor` fields; create requests follow the schema and retain the same idempotency key for an unconfirmed exact request; remote task detail and event timeline follow `after_event_id`, `wait_ms`, timeout margin, cursor deduplication/monotonicity and server cursor reset; answer/cancel/retry use the exact durable-command body shape. Workspace membership is refreshed before every mutation and owner/operator is required in the client UI/ViewModel, in addition to server authorization. Cancel/retry ask for confirmation. There is no offline mutation queue.

**Privacy/reliability review:** raw event payloads are not stored in the presentation model or rendered. A typed safe-summary mapper exposes only known event categories and validated state labels; unknown types are ignored for display but still advance the event cursor. A confirmed task-acceptance response is distinguished from a subsequent refresh failure. Session 401 clears the remote secret but not the local core credential; 403 is presented as a permission problem.

**Checks actually run:** API-compatible Kotlin/JVM harness using local JSON/coroutine/JUnit stubs compiled the HTTPS client and test sources: **14/14 passed**. Source-policy verifier and 6/6 XML parsing passed. A Kotlin parser probe showed no parse diagnostics. These are not Android Gradle/JUnit/Compose/APK/device acceptance.

**Not complete:** process-death persistence for remote event cursor/history; Stage 6C remote artifact ticket and verified download; full Android CI build and device acceptance.


## Native Android audit — Iteration 1/3 (2026-10-25)

**Focus:** architecture and functional parity. `verify_native_source.py` passed. Confirmed Stage 6C remote artifacts are absent; pending idempotency keys/event cursor/selected task are only ViewModel fields; Android Gradle/APK/device acceptance has not run; Compose UI/accessibility tests are missing; backend contract changes do not trigger the native workflow; config save still relies on a potentially destructive legacy full-bundle operation. Corrected the stale Stage 4A TODO checkboxes to reflect implemented Stage 4B config source while preserving the non-destructive endpoint as open. Full finding details: `ANDROID_NATIVE_AUDIT_ITERATION_1.md`; consolidated plans: `ANDROID_NATIVE_REVIEW_DECOMPOSITION.md`, `ANDROID_NATIVE_AUDIT_TODO.md`.


## Native Android audit — Iteration 2/3 (2026-10-25)

**Focus:** transport/credential lifecycle/protocol edge cases. Re-ran `verify_native_source.py` — PASS. Source review confirmed HTTPS-only/no-redirect/bounded JSON/body-header auth boundary, separate Keystore aliases, allow-listed remote event summaries and owner/operator refresh before mutations. Added findings ANDR-007 (workspace switch may be partially committed while reporting previous workspace), ANDR-008 (401 cleanup exception is suppressed), ANDR-009 (required body `api_version` is not checked), ANDR-010 (idempotency keys do not survive process death), ANDR-011 (selected task/event cursor state restoration missing) and ANDR-012 (no test evidence for these cases). Full report: `ANDROID_NATIVE_AUDIT_ITERATION_2.md`; decomposition/TODO extended accordingly.


## Native Android audit — Iteration 3/3 (2026-10-25)

**Focus:** UI/design, API37 behavior, accessibility, build/test evidence. `verify_native_source.py` PASS; Android XML 6/6 parse; standalone `CoreSettings.kt` compiled with local `kotlinc`. `src/androidTest` is absent; Gradle and Android SDK/platform 37 are unavailable; actual Compose/JUnit/APK/device tests are NOT RUN. Added findings ANDR-013 (NavigationRail without multi-pane workspace), ANDR-014 (M3 Expressive/accessibility not accepted), ANDR-015 (Android 17 behavior not device-verified), ANDR-016 (hard-coded strings), ANDR-017 (source checker is guardrail only) and ANDR-018 (large feature files). Consolidated report is `ANDROID_NATIVE_AUDIT_FINAL.md`; final decomposition and TODO include these findings. Release decision: HOLD/NOT RELEASE-READY until P1 gaps and Android CI/device evidence are closed.


## Stage 7 — remote reliability and API contract remediation (2026-10-25)

**Implemented:** NAT-17 atomic workspace switch and coherent refresh snapshot; NAT-18 fail-closed credential deletion on 401/logout with visible retry UI; NAT-19 required numeric `api_version == 1` in successful JSON response bodies; NAT-02/NAT-20 persistent remote mutation idempotency keys/digests written before requests, with no raw task/answer content in the ledger; remote selected task/filter/event cursor plus up to 100 safe event summaries persisted for process recovery (NAT-03 remote subset).

**Safety detail:** expired ambiguous operations are not automatically assigned a new key after seven days. UI requires the user to inspect remote tasks/events and explicitly confirm reconciliation before discarding the old ledger entry. Ledger capacity is bounded at 128; corruption/full state fails closed instead of silently creating a new key.

**Checks actually run:** native source verifier PASS; Android XML 6/6 parse PASS; standalone Kotlin compile of `RemoteOperationLedger.kt` PASS; deterministic runtime harness 4/4 PASS. Added JUnit source tests for body API version and ledger semantics but did not run them because Gradle/JUnit/Android SDK are unavailable. Compose/ViewModel/Kotlin full compile, APK and API37 emulator/process-kill tests are NOT RUN. Stage report: `ANDROID_NATIVE_IMPLEMENTATION_STAGE_7.md`.

**Review fixes applied:** updated source verifier for fail-closed logout order; a local-ledger cleanup failure after confirmed server acceptance no longer reports the operation as unknown; malformed stored ledger refuses mutations; 401/403 during selected-task restore is not swallowed; expired operation has an explicit user-reviewed recovery route.


## Stage 8 — Remote Control Plane artifact delivery (2026-10-25)

**Implemented:** NAT-01 source paths for task artifact inventory, authenticated ticket creation, bearer-free same-origin HTTPS byte streaming, bounded download, strict metadata and response header validation, SHA-256 and length verification, one refresh after expired ticket, partial cleanup, app-private verified file and non-exported FileProvider Sharesheet. `artifact_available` event triggers inventory refresh. Filenames/ticket/digest values are redacted in diagnostic `toString()` methods.

**Checks actually run:** `verify_native_source.py` PASS; Android XML 6/6; Kotlin/JVM contract harness 27/27 PASS using local API-compatible stubs and the real ControlPlaneClient/RemoteArtifactTransfer/test sources. Three local review passes found and fixed: early filename validation before ticket request, correct required nullable `expires_at` validation, event-driven inventory refresh, cleanup after stream I/O failure and duplicate response closure. This is local review, not external independent agent review.

**Not run:** Android Gradle/JUnit, Compose compiler, APK, FileProvider/ShareSheet instrumented acceptance and API37 device/emulator. Environment lacks Gradle/wrapper JAR/Android SDK. Stage report: `ANDROID_NATIVE_IMPLEMENTATION_STAGE_8.md`.


## Stage 9 — Non-destructive guided configuration (2026-10-25)

**Implemented:** new `config_update_settings` RPC with explicit server/runtime/sandbox allow-list, strict JSON types/ranges, merged-candidate `config_load` validation, private staged replacement and rollback copy during the commit durability window. The endpoint does not touch model registry or agent profiles. Legacy `config_save` now skips hidden legacy fixed-role profiles during cleanup. Native Android `RuntimeConfigDraft` produces an allow-listed JSON object and `CoreRepository` calls the new RPC; the ViewModel updates only its transient local snapshot after core success. UI no longer asks users to acknowledge full-bundle save; it retains a separate acknowledgement when the requested sandbox change weakens policy.

**Verification:** full CMake Release build PASS; new `core_config_safe_update` runtime test PASS; targeted config/string/HTTP/SQLite regression group 5/5 PASS; native Android source verifier PASS; Python syntax validation PASS. `tests/test_core_config_safe_update.py` checks preservation of hidden credentials/paths, byte-identical models registry, legacy fixed-role agent survival, strict type/range/field rejection, no partial updates after invalid requests, 0600 permissions, cleanup of rollback file and no secret leakage in error replies.

**Not run:** full CTest/integration for this stage, Android Gradle/JUnit/Compose compile, APK install, API 37 device acceptance. See `ANDROID_NATIVE_IMPLEMENTATION_STAGE_9.md`.
