# Iteration 2 — status-distribution challenge (2026-10-28)

## Дополнительный сценарий

Added `low_running_4`: exactly four running rows at every database size. This forces the candidate to process the queued band and preserve the full public order rather than stop after selecting 200 running tasks. Both distributions are measured at 1k/10k/100k rows and concurrency 1/4.

## Result

At 100k / fixture with 10,003 running tasks, C API median improves 65.20× (concurrency 1) and 69.81× (concurrency 4). In the four-running fixture, candidate is slower: 26.449 → 54.551 ms at concurrency 1 and 109.780 → 202.642 ms at concurrency 4. Throughput ratio is only 0.50×/0.55×.

## Decision

Candidate indexes remain isolated. A large win in a skewed fixture does not outweigh almost 2× slowdown on low-running / large-queued workload. Need a better queued aging strategy or different plan before considering production.
