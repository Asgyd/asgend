# ASGEND implementation status — 2026-10-21, iteration 5

## Summary

Implemented the next open WP6 item from `TODO.md`: a built-in, bounded Office OOXML text extractor for `.docx`, `.xlsx`, and `.pptx`, connected to the existing `asgend-memory extract` pipeline. The extractor uses in-memory ZIP parsing and does not unpack archive entries to the filesystem or modify the original source file.

## Changes

- Added `src/memory/extract_ooxml.c` and connected it in `CMakeLists.txt`.
- Routed Office files through the binary read/hash/stability-check path in `src/memory/extract.c`; a dedicated extractor version (`ooxml-text-v1`) ensures prior unsupported results are re-evaluated when this extractor becomes available.
- DOCX: extracts Word text nodes from the main document, headers, footers, footnotes, endnotes and comments.
- PPTX: extracts text nodes from slides and notes slides.
- XLSX: parses `sharedStrings.xml` and worksheet cells, including shared strings, inline strings, cached string values and numeric/boolean-like values.
- ZIP central/local headers and CRC32 are checked. Only ZIP STORE and DEFLATE are supported; ZIP64, multi-disk and encrypted members are rejected or surfaced as partial/unsupported rather than expanded without bounds. XML DTD/custom entities are never expanded.
- Limits: 64 MiB input archive, 4096 entries, 1024 bytes per entry name, 16 MiB per XML member, 64 MiB total decoded XML and 200,000/16 MiB limits for shared strings.
- Corrupt, over-limit or incomplete parts preserve recoverable text as `parsed_partial` where possible; documents with no recoverable text are `unsupported`. These statuses use the existing retrieval path.
- Added `tests/test_memory_ooxml.py` covering DOCX/PPTX extraction, XLSX shared/inline/numeric values, partial XML, invalid packages, CRC mismatch, size-limit behavior, hash metadata, skip-on-repeat, SQLite integrity and source-byte preservation.
- Updated `TODO.md`, `README.md`, `docs/MEMORY_INDEX.md`, `docs/ROADMAP.md` and `docs/IMPLEMENTATION_PLAN.md`.

## Verification

| Check | Result |
|---|---:|
| Release CMake build | Passed; no compiler warnings in captured build output |
| Short CTest suite (`integration` excluded) | 14/14 passed |
| Contract tests | 31 passed, 1 skipped (optional validator not installed) |
| Control Plane unit tests | 10/10 passed |
| Node-agent tests | 37/37 passed |
| OOXML smoke under AddressSanitizer + UBSan | Passed; no sanitizer errors reported |
| Python compile check for new test | Passed |

## Not verified / remaining

- The long CTest `integration` scenario was excluded from this iteration's final short suite; earlier project runs did not complete within the available timeout. It is not claimed as passing.
- Docker Compose runtime acceptance, live PostgreSQL migration/backup/restore and full CP ↔ node-agent ↔ core acceptance remain open.
- The extractor is a bounded text extractor, not a full OOXML renderer: it does not OCR images, parse legacy `.doc`/`.xls`/`.ppt`, evaluate spreadsheet formulas or expand macros/DTD entities. ZIP64 and encrypted Office files are unsupported.
- WP6 decision-model query planning (`query --plan`) remains the next memory task. Qt/Linux and Flutter/Android client work and final ASGEND 1.0 acceptance remain open.

The archive does not include temporary build directories, Python bytecode/cache, or local memory indexes.
