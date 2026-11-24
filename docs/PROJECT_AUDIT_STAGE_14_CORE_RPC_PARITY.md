# Stage 14 — local Core JSON-line RPC inventory and client parity

**Snapshot:** 2026-10-26, built on Stage 13 cumulative source.

## Finding

`docs/PROTOCOL.md` described many RPC examples but did not contain a complete, machine-checked inventory of the local TCP/Unix newline-delimited JSON API. As a result, adding/removing a dispatch branch in `src/server.c` could leave docs stale. Local Core RPC is a distinct protocol from the Control Plane HTTPS/OpenAPI contract covered in Stage 13; the two inventories must not be conflated.

## Changes

- Added a complete alphabetically sorted inventory block to `docs/PROTOCOL.md`, delimited by stable markers and compared to `server_handle_request()` at test time.
- Added descriptions for the previously under-documented `config_get`, legacy `config_save`, `models`, `resources`, `mcp_status`, `lsp_diagnostics`, and dynamic model lifecycle operations. Added explicit `events` and `answer` examples and reconnect/cursor notes.
- Added `tests/test_core_rpc_contract_inventory.py`, using only Python standard library. It extracts the actual C handler body while ignoring braces inside C comments/string/character literals, derives operation names from the dispatch clauses, compares the exact set to the docs inventory, requires a minimum operation count to detect extractor failure, and scans literal/dynamic operation values in Android, Flutter, Qt and Node Agent client sources.
- Registered `core_rpc_contract_inventory` in CTest.
- Reviewed the test's failure path with synthetic malformed inventories so missing/duplicate markers and absent fences fail rather than silently reducing coverage.

## Results

- Runtime dispatch inventory: **33 operations**.
- Protocol inventory: **33/33 exact**, sorted, unique; no undocumented or stale operation names.
- Client references found: native Android **33** supported operations including 4 dynamic model actions; Flutter **15** including 4 dynamic model actions; Qt **19** including 4 dynamic model actions; Node Agent **9**. All discovered operations are supported by the Core dispatcher. These are source-scanner results, not proof that every platform UI exposes every operation.
- `python tests/test_core_rpc_contract_inventory.py .`: **4/4 PASS**.
- Full C build and current CTest must be run for this stage; prior Stage 12 `40/40` does not count as Stage 14 verification. See the final stage report's check table and the actual build/CTest artifacts for the updated result.

## Review passes

Three local review passes were completed: (1) C handler extraction / exact inventory, (2) per-client static and dynamic operation coverage, (3) docs semantics, negative cases, CMake registration and packaging. This environment has no separate subagent launcher; these are local reviews, not an independent external-agent rating.

## Scope and limitations

- The scanner is deliberately scoped to the `server_handle_request()` C TCP/Unix RPC dispatcher. The HTTP `/api/*` browser routes and Control Plane HTTPS endpoints are documented and tested separately.
- Client scanning detects static literal operations and known dynamically selected model lifecycle operations. It does not prove end-to-end behavior for each platform; platform-specific runtime acceptance remains a separate gate.
- Stage 14 is a documentation/test-coverage improvement, not a production RPC behavior change.

## Build and CTest verification

- Fresh Release configure/build: **PASS** (GCC 14.2; C11 targets all compiled and linked).
- Targeted `core_rpc_contract_inventory`: **4/4 PASS** in standalone invocation and **1/1 PASS** as CTest target.
- `client_workflow_contract_triggers`: **PASS**, 3 workflows × 2 events, **76** required path assertions (38 per event type), including the RPC inventory test file.
- Client workflow YAML syntax: native Android, Flutter Android and Qt all parse successfully.
- Full CTest: **41/41 PASS**, 227.53 seconds, exit code `0`; this final run includes integration, the new inventory target, workflow path guard, and all Core/CLI/TUI/memory tests.
- Native Android source verifier: **PASS** (source-contract checks only; no Gradle/APK/device acceptance claim).
- Python `py_compile` for the changed contract scripts: **PASS**; `git diff --check`: clean.

The full CTest was run after the final client-workflow path-filter correction, so the result applies to the exact packaged Stage 14 source snapshot. Hosted GitHub Actions have not been triggered from this environment.