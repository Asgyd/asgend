# Stage 34 Critical Review — Iteration 1: Corpus / Environment

**Focus:** corpus-label integrity, fixture path safety, process boundary, endpoint isolation.

**Findings and fixes:**
- The existing ANN regression did not adequately test multi-query judged relevance. Added eight synthetic judged topics with four relevant paths per query and a 5,168-vector distractor set.
- Added validation that query IDs/texts and labelled paths are unique, topic indices are contiguous, relevance paths are safe relative `judged/*.md` paths, and corpus/vector/distractor counts balance.
- Cleared external embedding/decision/reranker endpoints and keys from the benchmark subprocess environment, pinned the embedding endpoint to an ephemeral `127.0.0.1` server, and set loopback-only `NO_PROXY`.
- The server accepts only the expected model and exact known query strings; every embedding request is counted.

**Verification:** benchmark smoke passed on 5,200 vectors; independent verifier checked 5,200 source file SHA-256 values and exact fixture tar members. Full benchmark was then repeated after harness hardening. No production ANN code changed.
