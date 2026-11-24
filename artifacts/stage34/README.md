# Stage 34 Benchmark Artifacts

This directory contains the full Stage 34 ANN relevance/performance run and its DB-derived manifest.

- `ann-quality-benchmark.json`: raw query rankings, repeated end-to-end latency samples, per-query quality metrics, candidate counts and acceptance outcomes.
- `ann-quality-corpus-manifest.json`: one row per vector from the actual indexed SQLite database, including source/input/vector hashes and ANN signature.
- `ann-quality-verification.json`: independently recalculated metrics and content/hash verification.
- `ann-quality-fixtures.tar.gz`: the reproducible 5,200-file synthetic source corpus plus the judged dataset, excluding `.asgend_mem` state.

The corpus is deliberately synthetic. It does not establish real-world search quality or justify changing production ANN defaults.

## Current validation

The full Release CTest passed **58/58** with exit code `0` in 274.73 seconds. The independent verifier confirmed all eight judged synthetic topics, 5,200 DB-derived vector manifest rows, 96 mock requests, and the fixture tar source/dataset hashes.
