# ASGEND 1.0 — implementation status, iteration 4

Date: 2026-10-21

## Scope

Implemented the next WP6 item from `TODO.md`: activate the existing built-in PDF text extractor as part of `asgend-memory extract` and make partial PDF extraction usable by the retrieval pipeline.

## Changes

- Added `src/memory/doc_common.c` and `src/memory/extract_pdf.c` to the `asgend_memory` target and linked zlib (`ZLIB::ZLIB`) for bounded FlateDecode support.
- Added a binary PDF read path that reads a bounded random-access input (64 MiB max), computes SHA-256 while reading, and checks file identity, size, mtime, and hash before persisting extracted text.
- PDF extractor statuses now distinguish `extracted`, `truncated`, `parsed_partial`, and `unsupported`. Partially parsed documents keep their explanatory reason and their available text.
- Updated chunk synchronization, embeddings, graph construction, retrieval, and text-count statistics to include `parsed_partial` where text is accepted as a retrieval source.
- Kept the existing generic text extractor ID stable so ordinary text files do not needlessly re-extract after upgrade; PDFs use `pdf-text-v1`.
- Added `tests/test_memory_pdf.py` and registered `memory_pdf_smoke` in CTest. Covered valid text extraction, partial fallback, encrypted PDF rejection, SHA-256, repeat extraction, database integrity, and source-file immutability.
- Updated `TODO.md`, `README.md`, and `docs/MEMORY_INDEX.md`.

## Verification

- CMake Release build: passed.
- CTest excluding long `integration`: **13/13 passed**.
- Contract tests: **31 passed, 1 skipped** (optional validator-dependent check).
- Execution Node agent tests: **37/37 passed**.
- Control Plane tests: **10/10 passed**.
- `tests/test_memory_pdf.py`: passed through CTest.

### Not verified

The full `integration` CTest did not finish within the available execution limit. A test `agent-orchestrator` process remained after the runner stopped and was terminated; this test is **not** counted as passed. Docker runtime, live PostgreSQL migration/restore, actual CP ↔ node-agent ↔ core end-to-end acceptance, and Qt/Android builds remain unverified.

## Remaining WP6 work

- Office OOXML extraction (`docx`, `xlsx`, `pptx`) with bounded ZIP/deflate handling, XML text extraction, and zip-bomb limits.
- Decision-model query planning (`query --plan`) with opt-in behavior, strict JSON validation, and untrusted-evidence handling.
- ANN performance tuning on a representative real-world corpus.

## Overall status

This iteration closes the PDF extraction integration item only. It does not mark ASGEND 1.0 as production-ready; WP2 deployment/runtime acceptance, WP4 Qt packaging, WP5 Android build, and WP8 end-to-end acceptance remain open.
