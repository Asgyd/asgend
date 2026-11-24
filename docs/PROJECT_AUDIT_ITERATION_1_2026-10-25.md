# ASGEND project audit — iteration 1

**Date:** 2026-10-25  
**Scope:** source inventory, Release build, C runtime regression tests, MCP process lifecycle, and audit evidence baseline.

## Findings addressed in this iteration

### CORE-15 — MCP restart/startup lifecycle race (high)

`start_server()` reset the MCP process descriptors, protocol capability flags, and `started` state outside `MCPServer.mutex`, taking the mutex only at the protocol handshake. `mcp_call_tool()` also inspected `started` before attempting to acquire the mutex. A request arriving after the restarted MCP peer responded to `tools/list` but before the parent marked startup complete could fail transiently with an unhelpful tool-allocation failure. The same unlocked writes could race lifecycle readers during restart.

**Change:** the full startup transition, including helper process setup and handshake, is serialized under `MCPServer.mutex`. All failure exits release the mutex. `mcp_call_tool()` now checks that the mutex exists, waits for the mutex, and then checks `started` under the lock. A regression case submits a task at the restart fixture's `restart_ready` boundary and asserts that the MCP tool call succeeds.

## Verification

| Check | Result | Notes |
|---|---|---|
| Clean CMake Release configure/build | PASS | GCC 14.2, json-c 0.18, libcurl 8.10.1, SQLite 3.46.1 in this environment |
| `core_db_fault_injection` | PASS | 0.08 s |
| `core_config_safe_update` | PASS | 0.70 s |
| `core_http_status_mapping` | PASS | 0.83 s |
| `core_mcp_launcher_smoke` after adding restart-boundary tool test | PASS | 7.85 s |
| Full `integration` | NOT CONFIRMED | Test command exceeded the 300-second execution limit and stopped in the SSE idle-after-token fault block before reaching the MCP restart scenario. It is not recorded as a pass. |

## Cross-project areas inventoried

- C execution core, HTTP server, config/editor, SQLite persistence, LLM/SSE, MCP and LSP
- Memory extraction/index/embedding/graph/ANN/retrieval pipeline
- C CLI and TUI libraries
- Python Control Plane and API contracts
- Node agent
- Flutter, Qt and native Kotlin/Compose Android clients
- Web UI, release packaging, scripts, and CI workflows
- Documentation, deployment and security operations

This iteration implements the demonstrated MCP lifecycle race and creates a reproducible baseline. The remaining subsystems are audited in later iterations; the absence of a toolchain or external service is recorded as NOT RUN / environment-dependent, not as PASS.
