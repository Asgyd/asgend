# Performance audit — iteration 1: inventory and baseline

**Date:** 2026-10-27
**Scope:** source tree, native C runtime, memory/retrieval, Python Control Plane and Node Agent, Qt/Android/Flutter clients, Web UI, CI/build/test setup.
**Review mode:** structural inventory + clean Release build + CTest timing baseline. No production benchmark or external service credentials were available.

## Results collected

- Clean CMake Release configure: **5.0 s** on this runner.
- Clean Ninja build: **10.33 s wall**, 18.74 s user CPU, 1.24 s system CPU, peak RSS reported by `/usr/bin/time`: **81,608 KiB**; 41 Ninja build actions, parallelism 4.
- Runtime/toolchain: Linux 6.18.55 x86_64, GCC 14.2.0, CMake 3.31.6, Python 3.13.5; 3 online CPU threads and 5.8 GiB visible RAM.
- Structural inventory: **231 first-party source/test files, 70,616 lines** across the inventory extensions and directories. This includes tests and clients; do not treat it as production LOC.
- CMake registers **45 tests** in the input archive.
- `tests/test_memory_ann.py` creates a deterministic 5,200-document, 128-dimensional synthetic vector index. It asserts the ANN path is enabled, the target result is correct, and candidate count is reduced below 20% of the corpus; it does **not** currently measure ANN versus exact-search latency.
- The existing `scripts/benchmark_memory_vectors.py` has an explicit external embedding endpoint prerequisite and measures whole CLI wall time, including query-embedding network time. It currently always runs exact before ANN, which is a source of order/warm-cache bias.
- `scripts/smoke_control_plane_load.py` measures task-list HTTP throughput and p50/p95/p99 latency, but requires a running Control Plane and a test token. It is explicitly diagnostic, not a production capacity certification.

The machine does not provide Docker/Podman, Flutter/Dart, Gradle, Android `adb`, Qt/qmake, `perf` or Valgrind. Therefore Android/Flutter/Qt device runtime, Compose/PostgreSQL load, CPU flamegraphs and allocator profiles cannot be claimed as measured here. Java alone does not substitute for the missing Android/Gradle toolchain.

## Component-by-component first-pass assessment

| Component | Present measurement evidence | First-pass concern | Confidence |
|---|---|---|---|
| Core C runtime/RPC/server | clean Release build, RPC/integration tests | request and task latency percentiles are not exported by local RPC path; integration test wall time is dominated by many fault/retry cases | Medium |
| SQLite task DB/artifacts | atomicity/fault tests; source schema/index inspection | production-shaped query plans and contention are not measured; index effectiveness under large workspace data unknown | Medium |
| MCP/sandbox/LSP | launcher/restart tests; process lifecycle smoke | subprocess startup and tool-discovery latency not separated from RPC/task wall time | Medium |
| Memory scan/extraction | smoke tests for scan, PDF/OOXML/extraction | throughput by file size/type and incremental-rescan cost not recorded | High gap in observability; no speed claim |
| Memory embeddings | concurrent worker implementation; local mock-backed tests | external-provider latency/rate limit, throughput per worker, DB write contention and retry load not measured | High gap in observability |
| Vector retrieval/ANN | deterministic 5,200-vector functional test and candidate-reduction assertion | no repeatable median/p95 latency or recall curve in CI; exact-vs-ANN benchmark includes external embedding latency and fixed execution order | High |
| Control Plane/API/PostgreSQL | DB-backed acceptance tests and optional load script | no measured request p50/p95/p99, pool wait, SQL plan or lock wait under a production-shaped corpus | High gap in observability |
| Node Agent | functional resilience and end-to-end tests | heartbeat/poll load at node-count scale and recovery/retry amplification not measured | High gap in observability |
| Flutter/Android/Qt | source contract/unit tests; CI workflows exist | device frame time, memory, battery/network cost, large-list rendering, reconnect load not measured in this environment | High gap in observability |
| CLI/TUI/Web UI | terminal layout/stream state tests | rendering latency and large output/terminal throughput are not benchmarked | Medium |
| Build/CI | clean native build; 45 CTest registrations; workflow definitions | no historical benchmark artifact/trend or per-component performance budget; total CI wall time is sensitive to serial long tests | High |

## Findings to validate in subsequent iterations

1. **PERF-01 — ANN measurements need to be robust.** The test proves candidate reduction but not latency. The benchmark orders exact before ANN and includes external embedding request time in the same wall-clock metric. Add paired-order control, warm-up/repetitions and explicit notes around end-to-end vs search-only timing.
2. **PERF-02 — candidate deduplication has linear scans.** `src/memory/embedding.c` uses `find_hit()` to linearly search the accumulated hit list. Graph candidate expansion and chunk deduplication also include nested scans. On larger candidate sets this can approach quadratic work. Measure first; avoid a risky data-structure rewrite before a benchmark proves impact.
3. **PERF-03 — Flutter pages use fixed polling timers.** `TaskDashboard` refreshes up to 100 tasks every 3 seconds; another local task page polls every 2 seconds. There is overlap suppression in the remote dashboard, but no interval backoff on repeated failures. Evaluate only while those pages are active, then consider backoff / server-driven events.
4. **PERF-04 — Control Plane data-plane SQL needs scale-shaped plans.** Task/event/metrics queries and PostgreSQL indexes must be tested with representative row counts and `EXPLAIN (ANALYZE, BUFFERS)`; absence of such evidence is not proof of a slow query.
5. **PERF-05 — embedding concurrency has no recorded saturation curve.** Worker threads and per-worker SQLite connections are present, but no benchmark varies worker count and reports throughput, p95 and provider error rate.
6. **PERF-06 — CI has no durable performance history.** The benchmark outputs should be machine-readable artifacts tagged with commit, compiler/build mode, corpus size, CPU count and environment; avoid hard failure on noisy absolute timings until a stable baseline exists.
7. **PERF-07 — client performance remains unmeasured.** Add emulator/device acceptance only in a runner with the corresponding SDKs; do not infer runtime UI quality from source inspection.

## Iteration 1 conclusion

No production bottleneck has been conclusively proven by this pass. The strongest actionable result is an **observability gap**: functional tests cover substantial behavior, but they do not yet produce reliable performance metrics for the key resource-intensive paths. The next iteration adds a deterministic project inventory/test to make scope and baseline repeatable, then hardens the ANN benchmark methodology. Performance optimizations to product code will be made only when a reproducible profile or controlled test demonstrates benefit.

## Runtime baseline confirmation

The input tree's **45-test** CTest set completed as `45/45 PASS` in **248.21 s** (exit 0), run serially as `nobody`. GNU `time` reported 74.39 s user CPU, 12.20 s system CPU and 130,328 KiB maximum RSS for the waited CTest command/process accounting; this is not a sum of concurrent process RSS. Long-tail test timings: `integration` 135.97 s, `core_llm_sse_watchdog` 39.70 s, `core_mcp_launcher_smoke` 20.67 s, and `core_rpc_state_transitions` 6.76 s. The memory ANN smoke fixture passed in 2.93 s; current test output records candidate reduction/functional correctness but not a latency distribution.

The first root-owned test invocation was interrupted by the command runner's execution limit while CTest was on `integration` and left an orphaned child process. This was a harness/cleanup incident, not counted as a product test failure. Re-running the same 45-test suite unprivileged finished cleanly. The inventory regression added during this iteration passes its two unit tests directly and as CTest (`performance_inventory`, 0.78 s) after a fresh configure. It is registered as the 46th CTest target; the 45-test full baseline above predates registration of that new test.
