# Stage 34 Review Decomposition

## Findings

### F-34-01 — Existing ANN smoke did not measure labelled ranking quality (medium)
The scale regression used 5,200 vectors but its assertion concentrated on a seeded first result and candidate reduction. The existing retrieval quality smoke covered four lexical-only queries. Neither was a broad, labelled ANN relevance benchmark.

**Change:** add eight manually labelled synthetic topic queries, binary relevance paths, Recall@K/MRR/nDCG metrics and paired exact-vs-ANN latency on the real C CLI.

### F-34-02 — Synthetic benchmark could accidentally inherit an external provider endpoint or credential (high harness risk)
A benchmark must not silently send query data to the environment's configured live embedding endpoint.

**Change:** clear embedding/decision/reranker endpoint and key variables, explicitly use a loopback-only mock, restrict `NO_PROXY` to loopback, and reject any input not in the local query vector map. The mock verifies the requested model ID and counts each request.

### F-34-03 — Fixture paths and archive verification needed stronger invariants (medium)
Untrusted or malformed path labels could escape the temporary corpus; an archive with duplicate or unexpected members could otherwise pass a simple count check.

**Change:** validate dataset paths as relative judged Markdown paths before writing; independent verification checks exact archive member names, rejects duplicate/unsafe members, and hashes every source.

### F-34-04 — Acceptance report needed independent numeric recalculation (medium)
Checking only a stored `pass` boolean would permit stale or internally inconsistent reported values.

**Change:** a separate verifier recalculates per-query and aggregate ranking metrics, latency summaries, paired ratios, candidate statistics, and the actual acceptance values. It also verifies dataset, harness, binary, manifest, and archive content hashes.

## Acceptance criteria (frozen before full run)

- Exact macro Recall@5 ≥ 0.95.
- ANN macro Recall@5 ≥ 0.90, MRR@5 ≥ 0.90, and nDCG@5 ≥ 0.90.
- Median ANN candidate ratio ≤ 0.15; every query has at least 32 candidates and candidate ratio ≤ 0.15.
- 5,200 indexed vectors with signatures; SQLite integrity `ok`.
- All expected mock embedding calls observed, no unexpected input and no live provider use.
- Independent verifier reproduces metrics and hashes.
- Full CTest exit code zero, all registered tests pass with finite positive timeout; no matching test children remain.

## Scope boundary

No production ANN parameter/default changes were made. Synthetic results are not a substitute for human-labelled, representative, licensed evaluation data. No external agent/subagent review was used; the following are local critical review passes.

## Final gate result

Full Release CTest: **58/58 PASS**, 0 failures, exit code `0`, 274.73 seconds. All registered timeout values are positive and finite (10–300 seconds); no matching test processes remained after completion.
