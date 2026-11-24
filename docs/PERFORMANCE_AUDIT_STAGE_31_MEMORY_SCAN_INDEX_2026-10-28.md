# ASGEND Performance Audit — Stage 31: Memory Scan/Index

## Scope and decision boundary

Stage 31 profiles the real C `asgend-memory scan ROOT` command over deterministic corpora with 1,000 and 10,000 files. Content profiles are small text, Markdown, and mixed-size files. Each of the 18 size/profile/scenario combinations has five timed repetitions (90 samples). Scenarios are new-index full scan, unchanged no-op scan, and one-file change.

No production scan/index algorithm or schema was changed as part of this stage. The changes are measurement tooling, process/timeout regression coverage, and audit artifacts. Any optimization to the scanner remains a separate engineering decision requiring profiling and correctness evidence.

## Results source of truth

- Raw measurements and per-file fixture SHA-256 manifest: `PERFORMANCE_MEMORY_SCAN_INDEX_BENCHMARK_2026-10-28.json`.
- Markdown tables recalculated from that JSON: `PERFORMANCE_MEMORY_SCAN_INDEX_BENCHMARK_2026-10-28.md`.
- Independent raw-row and fixture SHA verifier: `../scripts/verify_memory_scan_index_benchmark.py`; captured result: `PERFORMANCE_STAGE_31_BENCHMARK_VERIFICATION_2026-10-28.json`.
- Reproducible runner: `../scripts/benchmark_memory_scan_index.py` and `../scripts/memory_scan_runner.py`.
- Timeout regression: `../tests/test_memory_scan_timeout.py`, registered as `performance_memory_scan_timeout_guard`.

The JSON records `last_scan_id` and `last_scan_files_seen` from the scanner output for each measured run. It does not use a proxy `scan_run_count` field. Individual fixture hashes plus the binary, harness, and helper hashes are captured in the same JSON. The independent verifier checked all 33,000 fixture paths, byte lengths and hashes against the deterministic corpus (56,320,000 source bytes total).

The full Release CTest was rerun after registering the timeout regression: **54/54 PASS, 0 failed, exit code 0, 270.30 s**. Every registered test was observed in the final log, all timeout properties were positive and finite (10–300 seconds), and no targeted CTest/scanner/benchmark/test child or `sleep 30` process remained afterward. Evidence: `PERFORMANCE_STAGE_31_CTEST_LOG_2026-10-28.txt`, `PERFORMANCE_STAGE_31_CTEST_SHOW_ONLY_2026-10-28.json`, `PERFORMANCE_STAGE_31_CTEST_TIMEOUTS_2026-10-28.json` and `PERFORMANCE_STAGE_31_PROCESS_CLEANUP_2026-10-28.json`.

## Measurement limits

Linux page cache was not cleared. The results are not cold physical-I/O measurements. Reported `logical_mib_per_second` is the original corpus byte count divided by elapsed wall time; for no-op scans it is a normalized corpus-size ratio and must not be interpreted as actual disk read bandwidth. The benchmark is a local Release build on the recorded host, not a production deployment or live distributed workload.

## Review process

Three critical local review passes are documented in:

1. `PERFORMANCE_MEMORY_SCAN_INDEX_ITERATION_1_2026-10-28.md` — corpus/scenario correctness and metadata.
2. `PERFORMANCE_MEMORY_SCAN_INDEX_ITERATION_2_2026-10-28.md` — timeout process-group and pipe cleanup.
3. `PERFORMANCE_MEMORY_SCAN_INDEX_ITERATION_3_2026-10-28.md` — raw JSON, calculations, and fixture-hash integrity.

The passes are not represented as external/subagent reviews. No external reviewer tool was used.

## Acceptance checklist

- [x] All 90 raw measurements completed and validated: 18 groups, five repeats each; actual C scanner, all rows exit 0, no timeout, closed pipes, positive `VmHWM`.
- [x] Fixture hashes recomputed against deterministic generated fixtures: 33,000 files / 56,320,000 source bytes, individual SHA-256 and size all match.
- [x] Markdown values generated from the latest JSON; independent verifier passes and report medians are recalculated from `measurements[]`.
- [x] Focused timeout regression passes; pre-run CTest registry audit found 54 tests with positive, finite timeout values (10–300 seconds). Final full CTest result is recorded separately.
- [x] Complete Stage 31 CTest pass: 54/54 PASS, zero failures/timeouts/skips, exit code 0, 270.30 s; every timeout is positive and finite; post-run process check found no matches.
- [x] Stage-specific audit bundle and updated cumulative project archive created; both tar files can be listed/read, have safe member paths, and have verified SHA-256 sidecars next to the downloadable archives.

Stage 31 is complete for this local audit. Deliverables: `asgend-performance-memory-scan-index-audit-stage-31.tar.gz` and `asgend-performance-audit-cumulative-stage-31.tar.gz`. Their `.sha256` sidecars are kept beside the archive files (outside the archives to avoid circular hashes).
