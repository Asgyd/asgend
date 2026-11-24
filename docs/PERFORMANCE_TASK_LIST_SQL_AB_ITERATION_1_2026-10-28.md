# Stage 28 — Iteration 1: baseline and isolated candidate

- Added a reproducible benchmark for 1k/10k/100k task rows, concurrency 1/4, five runs, fixed aging anchor and one warm-up per worker.
- Implemented the candidate in a separate Python SQL function with two partial indexes in a disposable database. No candidate index or query replacement was promoted.
- First reference validation was only against a Python mirror. Critical review marked this as insufficient proof for full ordered equivalence; Stage 28 was strengthened with a C reference harness in Iteration 2.
- Preliminary query plan confirmed that current full-list SQL scans tasks and uses a temp B-tree; candidate running/other bands can use ordered partial indexes.
