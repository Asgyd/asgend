# ASGEND implementation status — iteration 16

Date: 2026-10-21

## Scope

Iteration 16 targets the open WP8 `security_operations_review` gate with concrete local regressions, a bounded HTTP smoke/load tool, and a fix for rate-limiter resource exhaustion under unique-key churn. It does not claim production sign-off.

## Implementation

- Hardened `server/asgend_cp/ratelimit.py` so the configured `max_keys` is a hard bucket-table cap. The final slot is reserved for a shared overflow token bucket used by previously unseen keys once the per-key table is full. This means key churn cannot allocate an unlimited number of buckets or give every fresh key a new burst.
- Throttled opportunistic prune scans to at most once per minute. Without this, a full table with all-recent keys could cause an O(max_keys) sweep for every new key, turning a memory-exhaustion vector into CPU exhaustion.
- Validate `RateLimiter.hit()` token cost: non-finite/non-positive costs and costs above bucket capacity raise `ValueError` rather than mint tokens or return an unusable retry delay.
- Added `server/tests/test_security_operations.py`, covering 2,000 distinct keys, bounded table size, prune-scan throttling, concurrent burst accounting, invalid costs, active-task and artifact quotas, visible Prometheus counters, JSON log redaction, a 120-request concurrent availability smoke test and the actual HTTP load-probe runner.
- Added `scripts/smoke_control_plane_load.py`: bounded concurrency and request count, HTTPS required except loopback, token read from an environment variable, no response-body/token logging, maximum 2 MiB response body, status/error-rate and p50/p95/p99/max latency statistics.
- Added `docs/SECURITY_OPERATIONS.md`, updated the release checklist, workflow path filters, `TODO.md`, root README, and release-gate evidence.

## Test results

| Check | Result |
|---|---|
| Clean CMake Release configuration/build | Passed |
| CTest excluding the long `integration` scenario | **18/18 passed** |
| Release gate unit tests | **5/5 passed** |
| Contract tests | **31 passed, 1 optional skip** |
| Control Plane server test suite | **30 passed, 3 skipped** |
| Node-agent suite | **37/37 passed** |
| Security/operations test pack | **10/10 passed** |
| Python compile checks | Passed |
| Workflow YAML parsing | Passed |
| Backup/restore shell syntax | Passed |

The three server-test skips are expected in this environment: the compiled-core E2E test is skipped during plain unittest discovery because no binary path is supplied (it passed as the registered CTest `node_agent_c_core_e2e`), and both live PostgreSQL tests require an unavailable `ASGEND_TEST_POSTGRES_URL` plus PostgreSQL tools.

## Release state

`python3 scripts/check_release_readiness.py --write-report` reports **BLOCKED**, with 10 remaining blockers counted by the readiness checker. The `security_operations_review` gate remains `not_run`: the newly passing tests demonstrate local behavior, but do not replace a production threat-model review, multi-replica rate-limit decision, representative staging load profile, saturation review, or operator approval.

Still open: observed green PostgreSQL migration/restart and `pg_dump`/`pg_restore` CI, Compose runtime restore/readiness drill, long-running core integration test, Qt build/package smoke, Flutter analysis/widget tests/APK, independent human-judged retrieval evaluation, and production security/operations sign-off. Docker/PostgreSQL and Qt/Flutter SDKs are unavailable in this environment.

## Reproduction

```bash
python3 tests/test_release_gate.py
python3 tests/contracts/test_contracts.py
python3 -m unittest discover -s server/tests -p 'test_*.py' -v
python3 -m unittest discover -s node_agent/tests -p 'test_*.py' -v
ctest --test-dir /tmp/asgend-iter16-build --output-on-failure --timeout 45 -E '^integration$'
```

For a running staging Control Plane, see `docs/SECURITY_OPERATIONS.md` for a sample `scripts/smoke_control_plane_load.py` invocation. Use a dedicated low-privilege test account; never place its bearer token directly in command-line arguments.
