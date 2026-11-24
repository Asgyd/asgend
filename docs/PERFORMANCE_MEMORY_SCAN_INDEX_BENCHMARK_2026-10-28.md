# ASGEND Stage 31 — Memory Scan/Index Performance Benchmark

- Generated UTC: `2026-10-28T12:53:23.278502+00:00`
- Real scanner binary SHA-256: `47a3e3f83c4ddd3fcfec12bff042a2c835a3a1206443409d8980bdf70b19e64f`
- Harness SHA-256: `8cd24681f67f20105b23c026e7998e97b21bfbdcbfe2cebf14192e2343b9ea3e`
- Runner helper SHA-256: `f5d54ac15de60d5786d7f28c83adf925c7f4cea3f2f837e8a3c77a304a96cdb3`
- Fixture files hashed: **33000**
- Measurement completion: **90/90**

## Method

The timed operation is the actual C executable `asgend-memory scan ROOT`, not a mock or Python filesystem walker. Each corpus is deterministic and its every source file is listed with SHA-256 in the JSON fixture manifest. A fresh-index sample removes `.asgend_mem` before timing. A no-op sample performs an untimed initial scan before the timed unchanged scan. A single-file-change sample performs an untimed initial scan, restores `doc_00000` to the original fixture, modifies that one file, then times the scan.

Wall time is measured around the scanner process. Peak RSS is sampled from Linux `/proc/<pid>/status` `VmHWM`. The runner uses its own process group, drains both pipes, and kills the group on timeout. The timeout regression is registered in CTest separately.

**Cache/throughput limitation:** Linux page cache was not cleared. These timings are not cold physical-I/O measurements. `logical MiB/s` is total original source-corpus bytes divided by elapsed wall time; it is a normalized logical-throughput ratio, not measured disk bytes read. For no-op scans it must not be interpreted as actual read bandwidth.

## Recomputed results

All values below are recomputed from the raw `measurements[]` array in the JSON file supplied to this renderer. Median wall time is in seconds, files/s counts the scanner-reported `files`, logical MiB/s uses the fixture-corpus byte total, and peak RSS is median of per-process `VmHWM` samples.

| Files | Profile | Scenario | Runs | Median wall (s) | Range (s) | Median files/s | Median logical MiB/s | Median peak RSS (KiB) |
|---:|---|---|---:|---:|---:|---:|---:|---:|
| 1,000 | `markdown` | `new_index` | 5 | 0.527 | 0.459–0.583 | 1896.0 | 3.70 | 9012 |
| 1,000 | `markdown` | `no_op` | 5 | 0.319 | 0.277–0.350 | 3138.4 | 6.13 | 7684 |
| 1,000 | `markdown` | `single_file_change` | 5 | 0.306 | 0.266–0.311 | 3271.9 | 6.39 | 8660 |
| 1,000 | `mixed_size` | `new_index` | 5 | 0.428 | 0.413–0.471 | 2336.4 | 5.70 | 9112 |
| 1,000 | `mixed_size` | `no_op` | 5 | 0.296 | 0.291–0.312 | 3382.4 | 8.26 | 7644 |
| 1,000 | `mixed_size` | `single_file_change` | 5 | 0.324 | 0.318–0.338 | 3087.8 | 7.54 | 8660 |
| 1,000 | `small_text` | `new_index` | 5 | 0.502 | 0.431–0.521 | 1993.5 | 0.97 | 9052 |
| 1,000 | `small_text` | `no_op` | 5 | 0.304 | 0.268–0.379 | 3287.7 | 1.61 | 7600 |
| 1,000 | `small_text` | `single_file_change` | 5 | 0.324 | 0.300–0.371 | 3090.1 | 1.51 | 8624 |
| 10,000 | `markdown` | `new_index` | 5 | 11.111 | 10.875–11.759 | 900.0 | 1.76 | 11616 |
| 10,000 | `markdown` | `no_op` | 5 | 3.207 | 2.907–3.716 | 3117.9 | 6.09 | 10356 |
| 10,000 | `markdown` | `single_file_change` | 5 | 3.011 | 2.871–3.259 | 3320.9 | 6.49 | 11348 |
| 10,000 | `mixed_size` | `new_index` | 5 | 11.177 | 10.668–11.300 | 894.7 | 2.18 | 11596 |
| 10,000 | `mixed_size` | `no_op` | 5 | 3.344 | 2.951–3.559 | 2990.8 | 7.30 | 10348 |
| 10,000 | `mixed_size` | `single_file_change` | 5 | 3.892 | 3.361–3.987 | 2569.6 | 6.27 | 11344 |
| 10,000 | `small_text` | `new_index` | 5 | 10.649 | 10.495–11.361 | 939.0 | 0.46 | 11560 |
| 10,000 | `small_text` | `no_op` | 5 | 3.009 | 2.758–3.057 | 3323.4 | 1.62 | 10308 |
| 10,000 | `small_text` | `single_file_change` | 5 | 3.722 | 3.464–3.818 | 2686.4 | 1.31 | 11328 |

## Validation findings

- All 90 timed scanner invocations returned exit code 0, did not time out, and closed both pipes: **True**.
- Every measurement observed a positive Linux `VmHWM`: **True**.
- `last_scan_id` matched 1 for fresh-index samples and 2 for setup-plus-measurement samples: **True**.
- `last_scan_files_seen` matched corpus size in every row: **True**.
- Every single-file-change sample detected exactly one modified file: **True**.
- No claim is made about cold-cache physical I/O or production deployment performance.

## Raw source of truth

The JSON file next to this report is authoritative for all sample values, individual fixture hashes, environment facts, and the exact scanner/harness/helper hashes. Re-render this Markdown after any JSON change; do not manually edit numeric results.
