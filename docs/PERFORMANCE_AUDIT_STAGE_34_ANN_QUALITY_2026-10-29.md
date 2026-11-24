# ASGEND Performance Audit — Stage 34: ANN Retrieval Quality & Performance

Date: 2026-10-29

## Executive result

Stage 34 measures the existing production retrieval implementation `random-hyperplane-lsh16-radius4` against a deterministic synthetic corpus with manually declared relevance labels (not human-adjudicated). No production retrieval algorithm or production defaults were changed. A separate benchmark harness was added, the dataset acceptance limits were committed before the full run, and a second verifier independently recalculates the metrics and checks the corpus manifest and fixture archive.

The full benchmark completed 8 judged queries × 5 exact/ANN measured pairs, with one paired warm-up per query. It produced 40 exact and 40 ANN latency samples and exactly 96 valid loopback embedding requests including warm-ups. No live embedding provider, API key, cloud billable request, or external endpoint was used.

## Results from the latest full JSON

| Metric | Exact cosine | ANN LSH16/radius4 |
|---|---:|---:|
| Precision@5 (macro average) | 0.8000 | 0.8000 |
| Recall@5 (macro average) | 1.0000 | 1.0000 |
| MRR@5 (macro average) | 1.0000 | 1.0000 |
| nDCG@5 (macro average) | 1.0000 | 1.0000 |
| Median end-to-end CLI latency | 274.430 ms | 55.712 ms |
| p95 end-to-end CLI latency | 346.179 ms | 79.443 ms |

The median paired exact/ANN latency ratio is 5.1343×. The median ANN candidate ratio is 4.0962%; per-query candidates ranged from 165 to 261 of 5,200 vectors, and the worst observed candidate ratio was 5.0192%. Judged relevant-path overlap between exact and ANN at K=5 was 1.0 for all eight topics.

The dataset acceptance limits were recorded in `tests/fixtures/ann_quality_judged_dataset.json` before the benchmark. All pass: exact mean Recall@5 ≥ 0.95; ANN mean Recall@5 ≥ 0.90; ANN mean MRR@5 ≥ 0.90; ANN mean nDCG@5 ≥ 0.90; median ANN candidate ratio ≤ 0.15; at least 32 candidates per query; maximum per-query candidate ratio ≤ 0.15; SQLite integrity `ok`; exact mock request accounting.

## Methodology

- 5,200 document vectors, 128 dimensions: 32 curated-by-construction judged items (8 topics × 4 relevant paths) and 5,168 deterministic distractors.
- Each query maps through a local `127.0.0.1` mock to a deterministic topic prototype. Query text is deliberately absent from fixture titles/descriptions to avoid lexical overlap.
- The source files are physically generated in a temporary corpus and scanned by the real `asgend-memory` executable. Description/vector rows are attached to that actual SQLite index, then the real `ann-index` command computes signatures.
- For each judged query, exact cosine and ANN query are executed in alternating paired order. Latency includes process startup, loopback embedding request, ANN probe setup and SQLite search; it is not a search-kernel-only measurement.
- Quality labels come from the fixture's separate `relevant_paths`, not from exact search. Exact-vs-ANN overlap is reported independently.
- The independent verifier recomputes all per-query and aggregate ranking metrics, latency summaries, speedup ratios, candidate ratios and pass/fail conditions; validates dataset/binary/harness/manifest hashes; checks all 5,200 source hashes in the fixture tar and its exact member set.

## Limitations and interpretation

This is a deterministic synthetic geometry benchmark, and the documents were deliberately constructed as tight topic clusters. Perfect Recall@5/MRR@5/nDCG@5 does **not** establish real-world semantic retrieval quality; these labels were defined from synthetic topic construction, not adjudicated by human raters. The 5.1343× median latency ratio is specific to this test setup and the current local host; process startup and loopback HTTP are included. Do not change production ANN defaults or claim a real-user quality score from these results. The next quality step should use approved, licensed, human-judged documents and queries, with dataset governance and acceptance limits fixed before measurement.

## Implementation changes

- Added `scripts/benchmark_memory_ann_quality.py`.
- Added `scripts/verify_memory_ann_quality.py` as a separate verifier.
- Added `tests/fixtures/ann_quality_judged_dataset.json` with eight query labels and predeclared acceptance conditions.
- Added CTest benchmark smoke and dependent verifier smoke tests, each with finite timeout.
- No production C files or public API defaults were changed.

## Test suite

The full Release CTest completed with **58/58 PASS, 0 failures, exit code 0, 274.73 seconds**. All 58 tests have positive finite timeouts (10–300 seconds). The post-run process scan found no remaining CTest, ASGEND executable, or benchmark/test Python process. The raw log is `docs/PERFORMANCE_STAGE_34_CTEST_LOG_2026-10-29.txt`; structured results, timeout inventory, and process cleanup records are provided separately. Overall Stage 34 acceptance is complete for this synthetic benchmark scope.
