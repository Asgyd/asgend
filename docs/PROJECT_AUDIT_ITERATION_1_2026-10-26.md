# ASGEND project audit — iteration 1 (2026-10-26)

## Scope

Continued the cross-project audit from Stage 12. This iteration focused on source-of-truth consistency between the executable Python Control Plane router, OpenAPI v1 security/operation definitions, and endpoint paths embedded in Android, Flutter, Qt and Node Agent clients.

## Confirmed findings

- **CONTRACT-01:** runtime method/path inventory was not checked bidirectionally against OpenAPI.
- **CONTRACT-02:** runtime route auth classes were not compared with the operation-level OpenAPI `security` object.
- **CONTRACT-03:** the shared contract suite was not run by client-specific workflows when only client files changed.
- **CONTRACT-04:** the endpoint matcher needed a negative regression and minimum extractor counts to prevent silent under-discovery.

All four findings were addressed in this iteration. No route mismatch was found in the current 37-operation Control Plane snapshot; the findings concerned missing regression automation and CI execution coverage, not a discovered live endpoint mismatch.

## Changes

1. Added AST-based runtime route inventory extraction from `App._register_routes()` and exact method/path set comparison with OpenAPI.
2. Added route-auth to OpenAPI security parity checks (`none`, `userBearer`, `nodeBearer`, `metricsBearer`, and the download-ticket capability route).
3. Added client endpoint path checks for the native Android, Flutter, Qt and Node Agent HTTP clients. The test found 49 references (14 Android, 12 Flutter, 14 Qt and 9 Node Agent).
4. Added a negative path-template matcher test and minimum reference counts for each scanner.
5. Added `tests/contracts/requirements.txt` and invoked the shared contract suite from Control Plane, native Android, Flutter Android and Qt workflows.

## Verification

- Contract suite: **35 tests, 34 PASS / 1 SKIP**; optional `openapi-spec-validator` unavailable.
- Runtime route inventory: **37/37 exact** with no duplicate/extra/missing entries.
- Runtime security: **37/37 PASS**.
- Client endpoint references: **49/49 mapped** to documented paths/methods where the scanner extracts a verb.
- Workflow trigger guard: PASS, 70 required path assertions.
- Control Plane: **34 tests, 4 expected skips** (PostgreSQL/external environment unavailable).
- Node Agent: **37/37 PASS**.
- Native Android source verifier: PASS (static check only).
- Release readiness: `READY`; release gate tests **6/6 PASS**.
- Four touched workflow YAML files parsed successfully.

## Review rounds

Three local review passes were completed: (1) parser accuracy and exact route inventory, (2) dynamic path placeholders/security semantics and negative matcher behavior, (3) workflow dependency reproducibility, evidence accuracy and archive integrity. One review improvement was applied by adding minimum discovered-reference counts and a negative test for unsupported static path segments. No independent external-agent rating is claimed because a subagent launcher is not available in this environment.

## Remaining limits

- Hosted GitHub Actions was not run from this environment.
- Full CTest was not rerun because this stage changes contract tests, workflow wiring and documentation only; the Stage 12 result of 40/40 remains tied to that exact earlier snapshot.
- Qt method extraction is path-only and is explicitly documented as such.
