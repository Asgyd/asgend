# Stage 34 TODO — ANN Retrieval Quality & Performance

## Closed by benchmark/verifier

- [x] Freeze 8 manually declared synthetic judged queries and binary `relevant_paths` labels.
- [x] Compare exact cosine with production ANN LSH16/radius4 on 5,200 vectors / 128 dimensions.
- [x] Five measured paired repetitions per query with alternating exact/ANN execution order and one paired warm-up.
- [x] Report Precision@5, Recall@5, MRR@5, nDCG@5, candidate ratio, latency median/p95 and paired exact-over-ANN ratios.
- [x] Run only against explicit local loopback mock; clear provider endpoints and keys from child environment; verify no unexpected requests.
- [x] Verify source hashes, dataset hash, binary/harness hashes, DB-derived vector manifest hash, ANN signatures, SQLite integrity, and fixture archive exact contents.
- [x] Independent verifier recomputes metrics, latency summaries, candidate statistics, acceptance values and their pass/fail decisions.
- [x] Add benchmark + verifier CTest smoke cases with finite timeout; smoke benchmark and verifier passed directly.
- [x] Keep production ANN code/defaults unchanged pending real-world evaluation.
- [x] Full Release CTest: **58/58 PASS**, 0 failures, exit code 0, 274.73 s; all timeouts finite/positive (10–300 s), no matching processes remained.

## Full-run findings so far

- 8 judged queries; 32 relevant docs; 5,168 deterministic distractors; 5,200 vectors in 128 dimensions.
- Full benchmark: 40 exact and 40 ANN measured samples; 96 valid loopback embedding requests including warm-ups.
- Macro Recall@5/MRR@5/nDCG@5 = 1.0 for ANN; median candidate ratio 0.040962; max per-query ratio 0.050192.
- Median exact CLI latency 274.430 ms; median ANN CLI latency 55.712 ms; median paired exact/ANN ratio 5.1343×. Latency is end-to-end with process startup and loopback HTTP.
- Synthetic result only; no real-user quality claim. Do not change production defaults based on this corpus.

## Open next steps

1. Establish dataset policy, consent/licensing, anonymization and human-label adjudication for representative documents and queries.
2. Repeat ANN quality/latency on that approved corpus, with acceptance targets frozen before measurement.
3. Run a separately approved live-provider test only when explicit authorization and credentials exist.
4. Exercise disposable Control Plane/PostgreSQL under load with pool wait, row lock, SQL latency and error metrics.
5. Continue extractor substage profiling for real-world licensed fixtures.

## Stage 34 closure

All acceptance checks for this synthetic Stage 34 harness and complete CTest suite passed. This does not close the separate follow-up for representative human-labelled licensed data; it remains an explicit next-stage task.
