# Performance audit iteration 2 — load and artifact RPC profiles

## Scope and workload

Executed 1k/10k/100k task rows, read-heavy (90/10) and write-heavy (20/80), concurrency 1/4, five repetitions and 100 operations/worker/repetition. This yields 15,000 raw SQLite samples. Artifact profiling uses actual Core TCP RPC with 100/1,000/1,200 files and `workspace_read_bytes` at 4 KiB/64 KiB/1 MiB.

## Result

- All 15,000 SQLite operations succeeded; zero SQLite errors. All three databases passed integrity and rollback checks.
- `artifact_list` and `workspace_read_bytes` had zero RPC errors; SHA/size/MIME and read payload SHA-256 validation passed.
- At 100k rows, list-all median is approximately 22 ms at concurrency 1; at concurrency 4, p95 exceeds 90 ms in this local profile.
- The first analysis pass found two reporting defects in intermediate measurement: the main SQLite file size could be observed before WAL pages were checkpointed, and a purported aggregate file-read throughput divided by the sum of individual latencies rather than batch wall time.

## Corrections made before the final run

Checkpoint WAL before reading DB file size; compute aggregate artifact MiB/s over parallel batch wall time; ensure request latency is measured from inside the worker; exclude failed operations from latency distributions; add explicit compiler/CMake, cgroup, and source/binary/script hashes.
