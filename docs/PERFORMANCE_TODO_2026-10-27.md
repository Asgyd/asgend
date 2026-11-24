# ASGEND performance audit — implementation TODO

Status key: `[x]` completed and locally checked; `[ ]` open; `[!]` requires missing service/SDK/device/provider. Priorities are defined in `PERFORMANCE_REVIEW_DECOMPOSITION_2026-10-27.md`.

## Completed work

- [x] Stage 20: inventory components/files/lines, toolchain limits, clean Release build and baseline CTest timings.
- [x] Stage 20: publish component review decomposition and initial TODO list.
- [x] Stage 21: add deterministic source/test/workflow inventory script and regression tests.
- [x] Stage 21: alternate exact/ANN measurement order, add warm-up/repetitions/percentiles, paired speed-up and path Recall@5; avoid hard thresholds from noisy timing.
- [x] Stage 21: benchmark synthetic 5,200 × 128-vector data and label CLI plus local embedding round-trip timing scope.
- [x] Stage 22: correct workflow job counting to inspect only top-level `jobs:`.
- [x] Stage 22: correct explicit timeout parser so dynamic `${...}` loop names are not reported as named tests.
- [x] Stage 22: assign a 60-second default to CTest cases without an explicit timeout, preserving explicit longer budgets; generated metadata confirmed all 48/48 tests have effective timeouts.
- [x] Stage 22: add bounded Control Plane load-probe unit tests against a local mock for success status, 503/error handling, latency metrics, validation limits and token/workspace redaction.
- [x] Stage 22: run clean Release configure/build and record environment/time/RSS data.
- [x] Stage 22: keep a machine-readable synthetic local mock load result, clearly labeled as harness-only rather than Control Plane capacity.
- [x] Stage 22: complete three sequential audit/review passes, update the final report, decomposition and this TODO.
- [x] Stage 22: record full final CTest result after completion, validate the final source tree and archive plus SHA-256.

## P1 — Core RPC and SQLite

- [ ] Add deterministic performance workloads for `submit/status/list/events/answer/cancel/retry`, with request p50/p95/p99 and submit-to-terminal duration.
- [ ] Measure concurrent SQLite read/write latency, busy/retry rates, lock waits and operation counts under controlled task/event/artifact volumes.
- [ ] Save `EXPLAIN QUERY PLAN` output and verify indexes for actual filters/orderings; do not alter indexes solely based on source guesses.
- [!] Validate PostgreSQL query plans/pool contention on disposable PostgreSQL because service/container runtime is unavailable in the current environment.

## P1 — Memory scan, embeddings and retrieval

- [ ] Profile `find_hit()` and nested graph/chunk deduplication at increasing sizes; change algorithms only after repeatable before/after measurement and identical correctness.
- [ ] Benchmark scan cold-start, no-op incremental, one-file delta and `--verify`; report files/s, MiB/s, CPU and RSS.
- [ ] Split text/Markdown/PDF/OOXML extraction latency and memory.
- [ ] Sweep embedding worker counts with local mock provider; report throughput, p95, DB contention and retry/error rate.
- [!] Measure external provider latency/quotas only with a separately authorized endpoint and bounded request volume.
- [ ] Evaluate ANN candidate-count vs latency and Recall@K/MRR/nDCG on representative relevance-labelled data; synthetic path Recall@5 0.60 is not an acceptance criterion.

## P1 — Control Plane and persistence

- [ ] Run the load probe against a disposable live Control Plane using a dedicated short-lived test token; store only redacted summaries.
- [ ] Add concurrent end-to-end task lifecycle and artifact upload/download tests with row count, payload size and latency labels.
- [ ] Inspect PostgreSQL plans, pool wait, lock wait, WAL/backup and restore throughput using representative data.
- [ ] Profile metrics endpoint cost as task/command/node counts increase.

## P2 — MCP, LLM/SSE and Node Agent

- [ ] Split MCP process spawn, handshake, tool discovery, invocation and teardown latency; preserve the Landlock runner constraint note.
- [ ] Measure local SSE first-token/inter-token/completion latency separately from remote provider latency.
- [ ] Sweep Node Agent fleet sizes; capture heartbeat/poll RPS, retry amplification, reconnection and time to recovery.

## P2 — clients / frontend

- [!] Android Macrobenchmark/instrumentation on a pinned emulator/device for startup, scrolling/jank, memory, network activity and reconnect.
- [!] Flutter profile-mode measurements for 100/1,000 row lists and build/raster frame time; verify polling/error backoff based on measurements.
- [!] Qt large-list repaint/model-update time and memory using Qt Test/QElapsedTimer.
- [ ] CLI/TUI/Web streaming throughput under long responses and slow consumers.

## P2 — CI/performance governance

- [x] Add finite CTest timeouts and confirm generated metadata (48/48).
- [ ] Save per-test duration, benchmark JSON, commit SHA, compiler/build mode, CPU/memory and runner fingerprint as CI artefacts.
- [ ] Collect at least five comparable runs before proposing stable performance budgets; track median and tail variability.
- [ ] Ensure all external benchmark subprocesses and temporary files are cleaned up even if interrupted.
- [!] Verify hosted GitHub Actions and retain run IDs/artefacts; local CTest does not prove hosted-runner results.

## Final acceptance checklist

- [x] Three sequential audit iterations documented.
- [x] Per-iteration archive created for completed stage.
- [x] Final component matrix distinguishes measured, harness-only, static risk and unavailable evidence.
- [x] Final full local test suite passes and log/exit status is included.
- [x] Final `.tar.gz` and SHA-256 checksum verify; build/cache directories excluded.
- [!] External subagent score (1–10): no separate subagent runtime is available; do not represent internal adversarial passes as an actual external agent evaluation.
