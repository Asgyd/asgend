# ASGEND Native Android — Stage 6B: remote workspace and task parity

**Date:** 2026-10-25  
**Base:** Stage 6A HTTPS pairing archive  
**Status:** source implemented; Android Gradle/Compose/JUnit/device acceptance pending an Android SDK 37 environment.

## Implemented

- Workspace selector is based on the freshly loaded `/v1/me` membership list. Selecting a workspace persists only its ID and role; remote token remains in its separate Keystore slot. Workspace switch clears stale task/event state and reloads nodes/tasks.
- Remote task list uses `GET /v1/workspaces/{ws}/tasks` with exact OpenAPI filter names (`state`, `node_id`), `limit` 1..200 and opaque pagination cursor. Native UI offers all contract states and a load-more action.
- Task submission uses `node_id`, `task`, `idempotency_key`, `priority` (0..9), optional agent/session/TTL fields. A key is retained for the exact same pending request after an ambiguous failure; once a 200/201 response confirms acceptance the key is cleared. Refresh errors after accepted submission are reported separately from submission failure.
- Task detail uses `GET /v1/tasks/{task}`. Event timeline uses `/v1/tasks/{task}/events?after_event_id=&limit=&wait_ms=`. A 25-second server wait gets a 45-second client deadline. Polling is owned by a Compose `LaunchedEffect`, propagates coroutine cancellation, deduplicates/sorts by `event_id`, keeps a monotonic cursor, detects server cursor reset, and retries after failures without moving the cursor backwards.
- Unknown event types are not rendered, but the cursor advances. Display summaries are mapped from event type and a validated task state, rather than rendering raw `payload`; the event view model does not retain payload JSON. This avoids leaking arbitrary provider/planner/tool payloads into UI.
- Answer, cancel and retry use the exact routes and `idempotency_key` request bodies. The current workspace membership is refreshed from `/v1/me` immediately before every mutation. Both UI and ViewModel gate mutation to `owner`/`operator`, while the server remains authoritative. Cancel/retry require explicit confirmation. No offline command queue is present.
- `401` clears remote credentials and account binding without touching local core token. `403` is presented as a permission issue rather than offline. Command acceptance is described as asynchronous; accepted does not imply a node has started work.

## Contract checks actually run

A standalone Kotlin/JVM test harness using local API-compatible `org.json`, coroutine and JUnit stubs compiled the HTTPS client and `ControlPlaneClientContractTest` sources: **14/14 passed**. Added tests assert remote list filters/cursor URL encoding, task idempotency body, event cursor and timeout, answer/cancel/retry routes, validation before network, event summary privacy, and unknown event cursor progression. This harness is not the Android Gradle/JUnit task.

The static `verify_native_source.py` checker and XML parsing also pass. A Kotlin parser probe reported no Kotlin syntax diagnostics. Since Gradle, Android SDK/platform 37 and emulator are unavailable in the current environment, Compose compilation, actual JUnit execution, APK assembly and device tests are not claimed as passed.

## Remaining for Stage 6C/7

- Artifact inventory, download tickets, same-origin download policy, streaming bounds and checksum before export.
- Durable restoration of a remote event cursor across process death and wider lifecycle behavior.
- Full Android CI build, Compose UI tests, accessibility/TalkBack, large text, adaptive-window, offline and permission-change acceptance.
