# ASGEND project audit — iteration 2

**Date:** 2026-10-25  
**Scope:** follow-up lifecycle review, SSE deadline/cancellation behavior, and integration re-verification after iteration 1.

## Findings addressed

### CORE-16 — SSE idle timeout could be nondeterministic on a silent socket (high)

The previous implementation relied on `CURLOPT_XFERINFOFUNCTION` while using `curl_easy_perform()`. For an SSE peer that sent a first token and then left a chunked response open without further bytes, the callback did not provide a reliable independent wall-clock watchdog in every observed run. The task could remain active beyond its idle deadline and the test process could outlive its expected fault fixture.

**Change:** added `perform_stream_with_watchdog()` in `src/llm.c`, which drives the easy handle through `curl_multi_poll()` with a bounded 100 ms poll interval. It checks the monotonic first-token, inter-token idle, total timeout and cancellation conditions outside the socket callback wait. The existing libcurl progress callback remains enabled as a second check. A dedicated `tests/test_core_llm_sse_watchdog.py` and `core_llm_sse_watchdog` CTest invoke the SSE faults independently from the long integration.

## Verification

| Check | Result | Notes |
|---|---|---|
| Release build after SSE change | PASS | Clean compile/link with existing warning policy |
| `core_llm_sse_watchdog` | PASS | 1/1, 39.31 s, exit code 0; includes 404/500, malformed JSON, missing `[DONE]`, first-token timeout, idle timeout, reasoning-first-token, cancellation, partial-reset recovery, and cancellation before headers |
| `core_mcp_launcher_smoke` | PASS 3/3 | Each completed run includes the MCP restart-boundary tool-call regression |
| Core config/HTTP/SQLite targeted group | PASS 4/4 | `core_db_fault_injection`, `core_config_safe_update`, `core_http_status_mapping`, `core_mcp_launcher_smoke` |
| Full `integration` | PASS | 1/1, 126.75 s, CTest exit code 0 |
| Complete combined CTest | PENDING | A single full CTest invocation is the next acceptance gate; all 37 non-integration tests and the full integration have each passed in completed grouped runs, but the combined invocation is not yet confirmed |

The integration test's earlier timeout result is preserved as iteration-1 history. It is superseded for the current tree by the later completed run with exit code `0`.

## Remaining audit scope

- Run the complete registered CTest set once with all tests enabled and retain its exit code / count.
- Review Control Plane/API contract drift and packaging/CI trigger coverage across every client.
- Run available Python, Node and web asset checks; distinguish tests requiring PostgreSQL/Docker/Qt/Flutter/Android SDK.
- Android SDK, Flutter, Qt, PostgreSQL and Docker acceptance remain environment-dependent where their toolchains/services are absent.


### Follow-up re-verification

After this report's first draft, the registered `core_llm_sse_watchdog` CTest was rerun on the exact updated tree and completed **1/1 PASS in 39.31 seconds (exit code 0)**. The complete `integration` test was also run in a separate completed CTest invocation and passed **1/1 in 126.75 seconds (exit code 0)**. Earlier timeout observations are retained as history and are superseded for this exact source state. The complete combined 39-test CTest invocation is still pending.
