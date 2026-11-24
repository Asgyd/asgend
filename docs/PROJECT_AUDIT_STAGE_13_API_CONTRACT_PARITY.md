# Project audit — Stage 13: API/OpenAPI contract parity

**Snapshot date:** 2026-10-26
**Base:** `asgend-project-audit-stage-12-ci-triggers.tar.gz`
**Scope:** Control Plane runtime route registry, OpenAPI v1 operation inventory/security, and the endpoint references used by native Android, Flutter, Qt and the Node Agent.

## Findings and changes

### CONTRACT-01 — Runtime route inventory was not mechanically compared with OpenAPI (P1/P2)

The existing contract suite validated schemas, examples, `$ref` resolution, the contract sketch and state-machine documentation. It did not prove that the executable route table in `server/asgend_cp/app.py::_register_routes()` had exactly the same method/path pairs as `contracts/v1/openapi.yaml`.

**Change:** `RuntimeOpenApiParityTests.test_runtime_route_inventory_exactly_matches_openapi` parses the route registrations from the Python AST, normalizes named regex captures to path-template parameters, and compares the resulting set to OpenAPI operations in both directions. Duplicate runtime method/path pairs fail the test.

**Observed result on this snapshot:** 37 runtime method/path pairs; 37 OpenAPI operations; no missing or extra route; no duplicates.

### CONTRACT-02 — Runtime auth classes were not checked against per-operation security (P1)

A route could change from user-authenticated to node-authenticated (or unauthenticated) without a test detecting drift in its OpenAPI `security` declaration.

**Change:** `RuntimeOpenApiParityTests.test_runtime_auth_class_matches_openapi_security` maps `Router.add` auth classes to their documented security objects. The download-ticket route is explicitly treated as a path capability, not as a Bearer `Authorization` scheme.

**Observed result:** all 37 runtime routes have matching documented security. No mismatch found.

### CONTRACT-03 — Client endpoint references were not covered by the shared suite for client-only changes (P2)

The Control Plane workflow ran the contract suite for backend/contract changes, while client workflows did not run the same route checks when only a client source file changed.

**Change:** added request-bearing path extraction to `RuntimeOpenApiParityTests.test_client_control_plane_paths_exist_in_openapi` for native Android, Flutter, Qt and Node Agent. 49 references were found: native Android 14, Flutter 12, Qt 14 and Node Agent 9. The parser checks the method for Android, Flutter and Node Agent, and path coverage for Qt because several Qt request verbs are set separately from the path assignment. Qt request-method behavior continues to be covered by the Qt suite; the runtime-vs-spec test validates server method parity.

Also added `tests/contracts/requirements.txt` and invoked `python tests/contracts/test_contracts.py` from the Control Plane, native Android, Flutter Android and Qt workflows. This makes shared API compatibility part of each client validation path.

### CONTRACT-04 — Client route path matcher needed a negative regression (P2)

A matcher that only ever saw positive examples could become too permissive and silently accept a wrong static path segment.

**Change:** added `test_client_path_template_matcher_rejects_wrong_static_segment` and minimum discovered-reference floors for each client extractor. A dynamic task-action template is accepted for a documented action, while an unsupported literal action is rejected.

## Verification

| Check | Result | Notes |
|---|---|---|
| `python tests/contracts/test_contracts.py -v` | PASS | 35 tests, 34 passed, 1 skipped; the skip is optional `openapi-spec-validator` not installed |
| Runtime routes vs OpenAPI | PASS | 37/37 exact method/path pairs; no extras/missing/duplicates |
| Runtime auth vs OpenAPI security | PASS | 37/37 route auth declarations match |
| Client endpoint reference coverage | PASS | 49 refs across 4 clients; no unmatched path/method reference |
| `python tests/test_client_workflow_contract_triggers.py` | PASS | 3 workflows × 2 event types; 70 required path assertions |
| Control Plane `unittest` suite | PASS | 34 tests, 4 expected skips due to unavailable PostgreSQL/external environment |
| Node Agent `unittest` suite | PASS | 37/37 |
| Native Android source verifier | PASS | Static source contract only; not an APK build |
| `scripts/check_release_readiness.py` | PASS | `READY`, zero required blockers in the repository manifest |
| `tests/test_release_gate.py` | PASS | 6/6 |
| Workflow YAML parsing | PASS | Control Plane, Native Android, Flutter Android and Qt workflows |

No runtime route/security mismatch was found in this source snapshot; the fix closes the missing automation and CI coverage. The complete Stage 12 CTest result (40/40) remains historical evidence for the Stage 12 base. C runtime sources were not changed in Stage 13, and the complete CTest suite was not rerun for this documentation/Python-test/workflow-only stage.

## Limits

- Hosted GitHub Actions execution is not available from this local archive. Workflow YAML and test commands are checked locally; a push/PR run is still required to verify hosted runners.
- Qt's endpoint scanner currently checks path coverage rather than inferring the verb from each Qt request context. This limitation is documented rather than overstated.
- No separate subagent launcher is available. Three local review passes were performed; they are not represented as independent external-agent ratings.
