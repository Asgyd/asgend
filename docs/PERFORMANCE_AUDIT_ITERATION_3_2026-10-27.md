# Performance audit — iteration 3: adversarial review, test-budget safety, and load-probe validation

**Date:** 2026-10-27
**Scope:** challenge the accuracy of the audit tooling, validate the Control Plane load-probe harness without production services, ensure every CTest case has a finite effective timeout, and rerun the final tree.

## Independent-style critique findings and corrections

1. **Inventory could miscount workflow jobs.** Earlier logic counted two-space mapping keys in the entire workflow rather than only job IDs beneath top-level `jobs:`. The parser now limits counting to the `jobs` section; the unit fixture verifies that `on.push`, `on.pull_request`, and `permissions` are not counted as jobs.
2. **Inventory timeout parser could report the CMake loop variable as a named test.** Explicit timeouts now ignore `${...}` variable references and parse the generic default timeout separately. A regression test asserts there is no spurious `${ASGEND_TEST_NAME}` key.
3. **A CMake source line does not prove a timeout was applied.** The global default timeout condition now handles the CMake `*-NOTFOUND` sentinel. After a fresh configure, `ctest --show-only=json-v1` was parsed and confirmed **48/48 registered tests have an effective `TIMEOUT` property**. Explicit long budgets remain: integration 300 s, SSE watchdog 90 s, MCP launcher 60 s, RPC runtime 45 s, RPC state transitions 75 s, and selected focused tests 10–40 s; other tests default to 60 s.
4. **The Control Plane load probe needed local error-path evidence.** Added `tests/test_smoke_control_plane_load.py` and the `performance_load_probe_smoke` CTest. The tests use a local mock HTTP server, not an ASGEND service. They verify status/latency summaries, error-rate failure behavior, bounds on concurrency/workload, loopback-only HTTP, and that the token/workspace ID never appear in the JSON result.

## Local harness experiment (not Control Plane performance)

One 200-request / 16-worker localhost run against Python `ThreadingHTTPServer` returned 200/200 responses, 0 errors, and 186.49 requests/s. The observed latency distribution was min 0.91 ms, p50 4.13 ms, p95 37.20 ms, p99 1,063.02 ms, max 1,066.85 ms. This large tail under a synthetic loopback server is a warning about scheduling/contention/noise and is **not** an ASGEND Control Plane capacity result. The run only validates the harness's metrics path; it must not be used for production sizing or an SLA.

Machine-readable result: `docs/PERFORMANCE_CONTROL_PLANE_PROBE_MOCK_2026-10-27.json`.

## Build and targeted test evidence

- Clean Release configure: 5.16 s wall, reported max RSS 95,400 KiB.
- Clean Release build: 11.36 s wall with `--parallel 3`, reported max RSS 81,404 KiB. These values are not directly comparable to the earlier `--parallel 4` baseline as a speedup/regression claim.
- Python test units for inventory: 4/4 pass.
- Python test units for load probe: 2/2 pass.
- Python test units for benchmark methodology: 2/2 pass.
- The generated CTest JSON confirms 48 tests and zero missing effective timeouts.
- Final full CTest: **48/48 PASS**, 253.62 s total wall, exit code `0`, run serially as unprivileged `nobody`. Long tests: `integration` 135.56 s, `core_llm_sse_watchdog` 39.59 s, `core_mcp_launcher_smoke` 20.43 s. New load-probe smoke passed in 1.28 s; inventory regression passed in 0.80 s.
- CTest GNU-time process accounting: 80.85 s user CPU, 12.53 s system CPU, 130,196 KiB maximum RSS. This is process/child accounting, not the sum of all concurrent processes.
- Sanitized configured-test proof: `docs/PERFORMANCE_CTEST_TIMEOUTS_2026-10-27.json` lists every registered test and its effective timeout without the temporary build paths.
- Full output: `docs/PERFORMANCE_CTEST_FINAL_LOG_2026-10-27.txt`; build/environment details: `docs/PERFORMANCE_FINAL_ENVIRONMENT_2026-10-27.txt`.
- Post-run process and sandbox cleanup checks completed; no project test child remained active.

## Remaining performance evidence gaps

The local container lacks Docker/Podman, PostgreSQL service, Flutter/Dart, Gradle, Android adb, Qt/qmake, `perf`, and Valgrind. Consequently, this iteration did not measure live Control Plane or PostgreSQL performance, Node Agent fan-out, real MCP process startup/handshake decomposition, LLM provider first-token/inter-token latency, or UI frame time. No production-code performance optimization was attempted because the candidate linear dedup paths have not yet been profiled under a representative workload.

## Iteration 3 conclusion

The audit instrumentation is more trustworthy after fixing the inventory and timeout parsing gaps. A local load-probe harness is now covered by deterministic success/failure tests, and CTest's actual generated metadata confirms a finite timeout for every registered test. The mock load experiment highlights its own noisy tail and therefore reinforces the decision not to claim production performance from localhost-only measurements. Unmeasured service/device work remains in the TODO list.
