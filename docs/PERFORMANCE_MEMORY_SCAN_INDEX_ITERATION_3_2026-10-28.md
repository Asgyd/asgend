# Stage 31 — Critical review iteration 3: raw data, calculations, and report integrity

## Scope

Validate the relationship between each raw sample, the JSON report, aggregate calculations, fixture manifests, and the final Markdown audit. This is a local critical review, not an external or subagent review.

## Acceptance checks encoded in the tooling

- Every run must exit 0, not time out, close both pipes, and produce a positive Linux `VmHWM` sample.
- Every sample's final scan metadata must report the expected scan ID and exact corpus file count.
- Every single-file-change run must report exactly one modified file.
- The renderer accepts exactly 90 raw samples, with 18 groups and five repeats per group, then recomputes medians/ranges/throughput/RSS from `measurements[]` rather than trusting the JSON's precomputed `summaries[]` field.
- Every fixture path, source size, and SHA-256 is recorded individually; report-level script/binary/helper digests identify the measurement implementation.
- The Markdown explicitly distinguishes normalized logical MiB/s from physical I/O and disclaims cold-cache inference.

## Status

**PASS.** The independent verifier in `scripts/verify_memory_scan_index_benchmark.py` checked 90/90 raw rows, 18 groups × 5 runs, all per-row process/metadata assertions, source binary/harness/helper hashes, and all 33,000 fixture SHA-256 digests and sizes (56,320,000 bytes total). The verification result is saved as `PERFORMANCE_STAGE_31_BENCHMARK_VERIFICATION_2026-10-28.json`. The Markdown renderer was rerun from the exact current raw JSON. The full CTest also passed 54/54 with exit code 0, all timeouts positive/finite, and no lingering target processes. Only artifact manifest/checksum creation and archive verification remain for closure.
