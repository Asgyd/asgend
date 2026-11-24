# Stage 31 — Critical review iteration 1: benchmark design and scenario correctness

## Scope

Review the measurement boundary, fixture determinism, scenario isolation, and consistency of scan metadata. This is a local source review, not an external or subagent review.

## Findings and disposition

- **Fixed before the measured run — single-file fixture drift.** Repeated mutations would otherwise have accumulated in `doc_00000`, changing corpus bytes across repetitions. The harness now restores that file from the deterministic fixture generator before every setup scan and applies exactly one mutation before the timed scan.
- **Guarded — scenario isolation.** `new_index` removes `.asgend_mem` before the timed scan. `no_op` and `single_file_change` run one untimed baseline scan per repeat; only the measured scan contributes to wall time.
- **Guarded — metadata semantics.** Each raw row records the scanner-emitted `last_scan_id` and `last_scan_files_seen`; it no longer relies on a proxy `scan_run_count`. Assertions require ID 1 for a fresh index, ID 2 after a baseline+measurement sequence, and `files_seen == corpus size`.
- **Clarified — throughput semantics.** The computed MiB/s value is named `logical_mib_per_second`: source-corpus bytes divided by elapsed time. It is not physical I/O bandwidth, especially for no-op scans.
- **Clarified — cache conditions.** No attempt is made to clear Linux page cache. Results are not labeled cold physical-I/O measurements.

## Acceptance criteria for this pass

- Three deterministic profiles, sizes 1,000/10,000, three scenarios, five repeats per cell.
- Exactly 90 timed invocations and 18 groups of five samples.
- No mock scanner: command is the built C `asgend-memory scan ROOT` binary.
- Raw fixture hashes, binary hash, harness hash, and runner-helper hash are saved in the result JSON.

## Status

**PASS.** The measured JSON contains 90/90 clean runs in 18 groups of five. The independent verifier validates expected IDs/file counts and exactly one modification per single-file-change sample. All 33,000 deterministic fixture hashes and sizes match the live corpus roots. The source records actual C scanner and tooling hashes.
