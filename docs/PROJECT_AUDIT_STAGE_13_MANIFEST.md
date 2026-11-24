# Stage 13 package manifest

This cumulative archive is based on `asgend-project-audit-stage-12-ci-triggers.tar.gz` and includes Stage 13 API/OpenAPI/client endpoint parity checks, shared contract-test dependencies, updated Control Plane/native Android/Flutter/Qt workflows, and audit/decomposition/TODO reports dated 2026-10-26.

- Contract suite: 35 tests, 34 pass and 1 optional skip.
- Runtime API inventory: 37/37 exact route method/path pairs and 37/37 security mappings.
- Client API references: 49 found across native Android (14), Flutter (12), Qt (14), and Node Agent (9), with no unmatched reference.
- Workflow path guard: 70 required assertions pass.
- This stage changes tests/workflows/docs, not C runtime sources; the Stage 12 CTest 40/40 result remains attributed to Stage 12.
