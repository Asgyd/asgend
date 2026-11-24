# Performance index-cost audit — Iteration 2 (2026-10-28)

## Result

The full benchmark passed for 1k/10k/100k rows, balanced/queued-heavy state distributions, five independent DB pairs per cell and 20 samples per operation per run. Each operation/configuration has 100 raw latency samples; seed insert is intentionally summarized as five min/median/max observations with no p95/p99.

## Findings

The isolated candidate improves ordered list reads at 10k/100k, but increases index storage and tends to slow seed insert, status transitions, priority updates and delete/reinsert on balanced larger corpora. At 100k balanced, index storage increases by 4,214,784 bytes (+31.13% relative to existing index bytes), seed inserts rise ~19.3%, and measured write medians increase ~24.7%–73.5% depending on operation.

## Decision

Do not promote the partial indexes. The same benchmark contains mixed write medians under queued-heavy distribution; ratios are descriptive local diagnostics, not causal guarantees. Keep raw values for subsequent re-analysis.
