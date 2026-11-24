# Stage 34 — ANN Retrieval Quality & Performance Benchmark

- Status: **complete**
- Judged queries: 8; corpus vectors: 5,200
- Dataset SHA-256: `730e4427822fd4965516d611099437275c990f76c952f45f53962255fd6bec81`
- Binary SHA-256: `1e905049d9bfe3210cf216970c4907c77767780ea71e3ff9e5fe4c5d2aea7ff1`
- Harness SHA-256: `39012ba7116eaa4e941cb5e3bef4b646c5f880291186db62525c3a23afab5d2f`
- Provider: local loopback mock only; no live provider used.

## Aggregate quality / latency

| Mode | Precision@5 | Recall@5 | MRR@5 | nDCG@5 | Median latency (ms) | p95 latency (ms) |
|---|---:|---:|---:|---:|---:|---:|
| Exact cosine | 0.8000 | 1.0000 | 1.0000 | 1.0000 | 274.430 | 346.179 |
| ANN LSH16/radius4 | 0.8000 | 1.0000 | 1.0000 | 1.0000 | 55.712 | 79.443 |

## Query-level results

| Query | Exact Recall@5 | ANN Recall@5 | ANN MRR@5 | ANN nDCG@5 | Candidates / vectors | Median exact / ANN latency ratio |
|---|---:|---:|---:|---:|---:|---:|
| topic-00 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 245 / 5,200 | 5.263× |
| topic-01 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 165 / 5,200 | 5.705× |
| topic-02 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 261 / 5,200 | 4.406× |
| topic-03 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 166 / 5,200 | 5.184× |
| topic-04 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 173 / 5,200 | 4.473× |
| topic-05 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 205 / 5,200 | 5.085× |
| topic-06 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 257 / 5,200 | 5.323× |
| topic-07 | 1.0000 | 1.0000 | 1.0000 | 1.0000 | 221 / 5,200 | 4.486× |

## Acceptance criteria

- [x] `mean_exact_recall_at_5`: actual `1.0`, threshold/reference `0.95`.
- [x] `mean_ann_recall_at_5`: actual `1.0`, threshold/reference `0.9`.
- [x] `mean_ann_mrr_at_5`: actual `1.0`, threshold/reference `0.9`.
- [x] `mean_ann_ndcg_at_5`: actual `1.0`, threshold/reference `0.9`.
- [x] `median_ann_candidate_ratio`: actual `0.040962`, threshold/reference `0.15`.
- [x] `all_queries_minimum_ann_candidates`: actual `165`, threshold/reference `32`.
- [x] `all_queries_maximum_ann_candidate_ratio`: actual `0.050192`, threshold/reference `0.15`.
- [x] `sqlite_integrity`: actual `ok`, threshold/reference `None`.
- [x] `request_accounting`: actual `96`, threshold/reference `96`.

## Methodology / caveats

end-to-end asgend-memory CLI wall latency including process startup, loopback embedding HTTP, probe setup and SQLite search; not pure kernel latency
Rank metrics use binary judged paths. Exact cosine is a retrieval comparison baseline, not the label oracle; relevance labels are the separate dataset paths. Synthetic geometry is intentionally clusterable and does not establish real-world semantic quality.
