# ASGEND performance review — decomposition and status

**Updated:** 2026-10-27, after three sequential audit iterations.

## Evidence rules

- **Measured:** a timed invocation with command, environment and workload recorded.
- **Harness-only:** a mock service or synthetic dataset exercises the measurement mechanism, not the production component.
- **Static risk:** source inspection identifies a plausible cost; no measured bottleneck claim is made.
- **Not measured:** missing SDK, service, provider access, representative dataset or device.
- Product changes to optimize performance require before/after measurements and preserved correctness. No hard performance SLO is introduced from one shared-runner sample.

## Component matrix

| Area | Current evidence | Risks / missing evidence | Status |
|---|---|---|---|
| Build / CMake / CTest | Clean Release configure/build timing; 48 tests registered; every generated test has finite `TIMEOUT`; full final CTest log included when completed | Build samples use different parallelism across stages; do not infer speedup from direct comparison | Measured locally |
| Core RPC / task lifecycle | Functional integration and RPC contract/state tests; CTest long-tail timings | No separate submit-to-terminal, request-type p50/p95/p99 or sustained concurrent workload | Partially measured; P1 gap |
| SQLite / artifacts | DB atomicity, fault-injection and artifact tests | No production-sized row-count contention benchmark, `EXPLAIN QUERY PLAN` report, transaction/lock wait distribution | Not performance-measured |
| PostgreSQL / Control Plane | Load probe supports bounded authenticated task-list requests; success/error path unit-tested; localhost mock validates metrics and token redaction | No live ASGEND API or PostgreSQL service was available; mock's latency is not application performance | Harness validated; production unmeasured |
| Node Agent | C core E2E/contract tests | No multiple-node load curve for heartbeat/poll/retry and recovery | Not performance-measured |
| MCP / sandbox / LSP | Launcher/restart and lifecycle tests; whole MCP CTest timing | Spawn, handshake, tool discovery, invocation and teardown are not separately timed; Landlock positive policy depends on runner support | Partially measured; lifecycle latency not decomposed |
| LLM / SSE | Local watchdog test timing and cancellation/error behavior | No actual provider first-token/inter-token metrics; mock timing is not remote model latency | Not provider-measured |
| Memory scan / extraction | Functional scan, PDF and OOXML tests | No bytes/s, files/s, cold-vs-incremental scan, per-format cost or peak-memory sweep | Not performance-measured |
| Embedding pipeline | Existing embedding tests; local mock-backed behavior | No worker-count saturation curve, SQLite contention, provider quota/latency distribution | Not performance-measured |
| Vector retrieval / ANN | Synthetic 5,200 × 128 vectors; alternating exact/ANN order, warm-ups, five paired samples; latency and path Recall@5 emitted | Full CLI and local mock embedding HTTP time included; only five samples; synthetic path Recall@5 0.60 is not representative quality | Synthetic end-to-end measured |
| Flutter | Source inspection: remote task dashboard polls 100 tasks every 3 s; local task view polls every 2 s | No Flutter/Dart available for profile-mode, retry backoff or frame timing | Static risk only |
| Native Android | Source/CI inventory | No Gradle/adb/emulator for startup, scrolling jank, memory, network-per-active-minute | Not measured |
| Qt | Source/CI inventory | No Qt/qmake runner for repaint/model update/large-list benchmarks | Not measured |
| CLI / TUI / Web UI | Functional layout/stream tests; inventory of largest source files | No sustained streamed-output or terminal redraw throughput benchmark | Functional only |
| Packaging / CI | Workflow inventory and 48-test suite; final workflow/test changes from earlier audit remain present | No hosted GitHub Actions run IDs or artefacts; local execution does not prove hosted-runner performance | Locally validated only |

## Work packages and priorities

### WP-P1 — baseline and inventory: completed
- Record toolchain/environment, clean Release build, CTest test list and long-tail tests.
- Build deterministic component/file/line/workflow/test inventory.
- Add regression checks for job counting and timeout accounting.

### WP-P2 — repeatable local benchmark harness: completed with caveats
- Alternate exact/ANN ordering, optionally warm up, run repeated pairs, emit latency percentiles and paired speed-up.
- Report candidate count and retrieval quality along with timing; no absolute-time assertion.
- Validate load-probe success/failure metric formatting and secret redaction with local mocks.

### WP-P3 — Core RPC / SQLite: P1 open
- Instrument submit/status/list/events/answer/cancel/retry request latencies and submit-to-terminal duration.
- Add deterministic concurrency workload with controlled task counts and separate SQLite operation timings.
- Explain SQLite query plans and test indexed queries against representative data sizes.

### WP-P4 — memory / indexing / retrieval: P1 open
- Profile `find_hit()` and nested candidate/graph/chunk dedup at increasing corpus sizes before changing data structures.
- Measure cold, unchanged incremental and one-file-delta scans, plus `--verify`, by file count and bytes.
- Time extraction by text/Markdown/PDF/OOXML; sweep embedding workers with local mock provider; measure throughput, p95, error/retry rate and DB contention.
- Evaluate ANN latency/Recall@K/MRR/nDCG across sizes and candidate budgets on a representative relevance dataset.

### WP-P5 — MCP / LLM-SSE / Node Agent: P2 open
- Split MCP spawn, handshake, tool discovery, invocation and teardown times.
- Measure SSE first-token/inter-token/completion latency using local deterministic fixtures, then separately against an authorized provider.
- Simulate increasing Node Agent fleet size; measure heartbeat/poll rate, retry amplification, reconnect and recovery time.

### WP-P6 — Control Plane / PostgreSQL / artefacts: P1 open
- Execute `scripts/smoke_control_plane_load.py` against a disposable live Control Plane with a dedicated test token.
- Add task submit/list/events/answer/cancel/retry and artefact transfers under concurrency.
- Record p50/p95/p99, error rate, response sizes, pool wait, row counts, SQL plans and DB lock wait; run PostgreSQL `EXPLAIN (ANALYZE, BUFFERS)` with representative fixtures.
- Validate backup/restore throughput on realistic stores.

### WP-P7 — clients and UI: P2 open / platform constrained
- Android: Macrobenchmark or instrumented tests on a pinned emulator/device; cold startup, frame/jank, memory, active-minute network calls, lifecycle/reconnect.
- Flutter: profile mode for 100/1,000 task rows; build/raster times; measure polling while active/inactive and repeated-error behavior before adding backoff.
- Qt: QElapsedTimer/Qt Test large-list paint and model update, plus memory under many tasks/artifacts.
- CLI/TUI/Web: throughput and latency with long streamed responses and slow output sinks.

### WP-P8 — performance governance: partially completed
- Keep machine-readable source inventory and ANN/load-probe results.
- CTest finite timeouts are implemented and verified from generated metadata.
- Still add versioned benchmark artifacts and runner/environment fingerprint in hosted CI; gather at least five comparable runs per benchmark before budgets.

## Severity definitions

- **P0:** reproduced severe user-visible delay, resource leak or unbounded behavior — none confirmed by this audit.
- **P1:** critical hot-path/scale gap that should be measured before release claims; not necessarily a proven slow query.
- **P2:** observability, harness, medium-risk or platform-specific gap.
- **P3:** optional tuning once a stable profile demonstrates an opportunity.
