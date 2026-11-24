# Stage 28 — Iteration 3: critical review and decision

- Repeated full benchmark after adding one warm-up per worker and regenerating timestamp metadata; script/source/reference-binary SHA-256 are captured in raw JSON.
- Rechecked source-to-benchmark SQL markers, every status filter, aging and exact ties, candidate LIMIT boundaries, C reference outputs, and baseline/candidate query plans.
- Results: at concurrency 1 median ratio baseline/candidate is 1.45× / 8.11× / 50.17× for 1k / 10k / 100k; at concurrency 4 it is 0.77× / 0.90× / 8.31×. Therefore the candidate does not provide a stable win at all sizes and concurrency values.
- Decision: do not add the partial indexes to production yet. They require write/storage overhead tests and additional status-distribution scenarios. Only deterministic tie ordering was made explicit in production.
- Release build прошёл; полный CTest завершился **51/51 PASS за 277,28 с, exit code 0**. Все 51 теста имеют positive finite timeout. Полный журнал: `PERFORMANCE_STAGE_28_CTEST_LOG_2026-10-28.txt`; timeout metadata: `PERFORMANCE_STAGE_28_CTEST_TIMEOUTS_2026-10-28.json`.
