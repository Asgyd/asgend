# Stage 34 Critical Review — Iteration 2: Metrics / Acceptance

**Focus:** metric definitions, stale report detection, independent reproducibility.

**Findings and fixes:**
- Acceptance must not be inferred from exact cosine alone. The dataset stores separate manually assigned `relevant_paths`; exact results are treated only as a comparison baseline.
- Added independent recomputation of Precision@5, Recall@5, MRR@5, nDCG@5, aggregate means, per-query paired latency ratio, median/p95 latency, ANN candidate count/ratio, and each threshold's actual value plus pass/fail decision.
- Added SHA-256 checks for dataset, C binary, benchmark harness and canonical DB-derived manifest rows. The verifier checks all source hashes in the fixture tar, duplicate paths, unexpected members, and dataset tar hash.
- Corrected the latency table wording to `exact / ANN latency ratio`, where a value greater than one means the ANN CLI run was faster on this synthetic benchmark.

**Verification:** full eight-query benchmark and the separate verifier both completed successfully; the verifier independently reproduced the aggregate scores and candidate thresholds. The fixture archive contains 5,200 judged/distractor source files plus `seed.md` and the dataset.
