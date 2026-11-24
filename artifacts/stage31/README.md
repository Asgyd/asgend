# ASGEND Stage 31 — Memory Scan/Index audit artifacts

This directory is part of Stage 31 and contains the Release C scanner used for the measurements plus deterministic fixture files. The raw per-sample benchmark and per-file SHA-256 manifest live in `docs/PERFORMANCE_MEMORY_SCAN_INDEX_BENCHMARK_2026-10-28.json`.

## Reproduction

From the project root, after configuring and building a Release tree:

```sh
python3 scripts/benchmark_memory_scan_index.py \
  --binary /path/to/build/asgend-memory \
  --fixture-root /path/to/reproducible-fixture-root \
  --output-json docs/PERFORMANCE_MEMORY_SCAN_INDEX_BENCHMARK_2026-10-28.json \
  --timeout-seconds 120
python3 scripts/render_memory_scan_report.py \
  --json docs/PERFORMANCE_MEMORY_SCAN_INDEX_BENCHMARK_2026-10-28.json \
  --output docs/PERFORMANCE_MEMORY_SCAN_INDEX_BENCHMARK_2026-10-28.md
python3 scripts/verify_memory_scan_index_benchmark.py \
  --json docs/PERFORMANCE_MEMORY_SCAN_INDEX_BENCHMARK_2026-10-28.json \
  --fixture-root /path/to/reproducible-fixture-root \
  --repo-root . --binary /path/to/build/asgend-memory
```

The packaged `memory-scan-fixtures.tar.gz` contains the deterministic source files, but omits `.asgend_mem` generated index directories. It should unpack into a dedicated fixture root; do not use a previously scanned tree when reproducing a `new_index` run unless the harness deletes its index directory.

## Measurement limitations

The Linux page cache was **not** cleared. These are local Release-build wall-time measurements, not cold physical-I/O timings. `logical_mib_per_second` is source corpus bytes divided by wall time, not a measurement of disk bytes read. No production scanner algorithm or index schema was changed in Stage 31.

## Baseline provenance

The project source was reconstructed from the uploaded Stage 30 cumulative archive because that upload did not contain the claimed in-progress Stage 31 runner, JSON, or CTest log. Uploaded archive SHA-256:

`63d76cd3020900e6ec7d05ee02154aaea1d2252ec3760aa6baa3053de62db3f1`

The Stage 31 benchmark was then run anew and its raw data, hashes, verification results and CTest artifacts are included with the updated project archive. Final Release CTest after adding the timeout-process-tree guard: **54/54 PASS, 0 failures, exit code 0, 270.30 seconds**. Every registered CTest timeout is positive and finite (10–300 seconds), and the post-run process check found no target process remaining. The local archive manifest lists component hashes; downloadable tar archives have external `.sha256` sidecars to avoid self-referential archive hashes.
