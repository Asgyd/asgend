# Stage 19 — MCP sandbox policy parity and lifecycle stress

**Snapshot:** 2026-10-27, based on Stage 18.  
**Scope:** executable MCP launcher acceptance coverage; no change to production sandbox policy.

## Findings

The cross-project audit left a high-priority gap in `docs/PROJECT_AUDIT_TODO_2026-10-26.md`: MCP sandbox policy and lifecycle behavior had only partial runtime coverage. Environment isolation, UID/GID drop, rlimits, network denial, descriptor closure, and a single restart-boundary case were already exercised, but there was no explicit Landlock filesystem allow/deny matrix, failed-launch workspace cleanup assertion, or several simultaneous tool calls at the restart boundary.

## Changes

- Added `tests/mock_mcp_landlock_probe.py`. On Linux hosts where `landlock_create_ruleset` is callable, it probes explicit read allow-list behavior, denied sibling-file reads, workspace write access, denied writes in configured read-only and unrelated paths, and denied execution from a read-only path.
- Extended the test-only `tests/integration.py::start_server` helper with `sandbox_require_landlock` and `runtime_workers` parameters. Defaults preserve existing test behavior.
- Extended `tests/test_core_mcp_launcher.py` to detect whether the current host can create a Landlock ruleset. If available, the real sandbox-child allow/deny matrix runs with `require_landlock=true`. If unavailable/blocked, the test instead asserts that strict mode refuses MCP startup, does not execute the probe, and removes its private workspace.
- Added failure-cleanup acceptance for a nonexistent MCP executable after sandbox workspace creation, asserting no new `ao-mcp-sandbox-*` directory remains after startup fails.
- Expanded the restart-handshake regression from one task to four concurrent submissions, with four Core worker threads and independent terminal-state/result assertions for all tasks.
- Increased `core_mcp_launcher_smoke` CTest timeout from 35 to 60 seconds to account for intentional concurrent execution and retry backoff.

## Verification

- Fresh CMake Release configure: PASS.
- Fresh Release build: PASS (41 build steps; GCC 14.2).
- Targeted `core_mcp_launcher_smoke`: **1/1 PASS in 21.49 seconds**.
- This container reports `ENOSYS` for `landlock_create_ruleset` (`syscall 444`), so the positive Landlock allow/deny behavior matrix was not executed here. The local test exercises and passes the strict fail-closed branch instead. On a Linux host exposing Landlock, the same target runs the positive/negative filesystem matrix.
- The missing-executable startup path and workspace leak assertion passed; the four concurrent MCP tool tasks completed successfully after supervised restart.
- Fresh Release full CTest: **43/43 PASS in 248.74 seconds**, exit code `0`. Integration passed in 134.33 seconds, `core_llm_sse_watchdog` in 39.99 seconds, and this MCP target in 20.74 seconds. The complete log is [`PROJECT_AUDIT_STAGE_19_CTEST_LOG_2026-10-27.txt`](PROJECT_AUDIT_STAGE_19_CTEST_LOG_2026-10-27.txt).

## Limits

A passing launcher test in a container that blocks the Landlock syscall does not prove that the positive Landlock policy matrix passed. The test is present and conditionally executes on supporting hosts, while this environment's fail-closed behavior is independently verified. Hosted GitHub Actions has not been run from this session; the CI workflows must execute this test on a Landlock-capable Linux runner to establish the positive path. No production sandbox policy was changed in Stage 19.
