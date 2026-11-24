# Control Plane security and operations checks

## Automated regressions

Run the bounded rate-limit, quota, logging-redaction and concurrent HTTP smoke tests with:

```bash
python -m unittest discover -s server/tests -p 'test_security_operations.py' -v
```

The rate limiter stores no more than `max_keys` bucket records. When the per-principal table is full, unseen keys share a limited overflow bucket until stale entries are pruned. Opportunistic prune scans are throttled to at most once per minute, so each fresh key cannot trigger an O(max_keys) scan. This prevents unique-IP/token churn from growing the table without bound or forcing a full-table scan per request. It is a defensive single-process fallback, not a distributed rate-limit implementation; deployments with multiple Control Plane replicas should use a shared limiter or enforce equivalent limits at a trusted gateway.

Quota checks reject active-task and artifact reservations before they exceed workspace limits. Prometheus counters `asgend_cp_quota_rejections_total` and `asgend_cp_rate_limited_total` expose rejection counts. Structured access logs must not contain task/request bodies, bearer tokens, password-bearing database URLs or credential fields.

## Staging HTTP smoke/load probe

The optional no-dependency probe sends a bounded number of authenticated task-list requests to an already running Control Plane:

```bash
export ASGEND_CP_LOAD_TEST_TOKEN='use-a-dedicated-viewer-token-here'
python scripts/smoke_control_plane_load.py \
  --base-url https://control-plane.staging.example \
  --workspace ws_test_123 \
  --requests 500 \
  --workers 16 \
  --timeout 5 \
  --max-error-rate 0.01
```

The probe prints JSON with status counts, request rate and p50/p95/p99/max latency. It bounds each response body to 2 MiB and intentionally does not print the token, response bodies or full URL paths. HTTPS is required for non-loopback targets; plain HTTP is permitted only for loopback tests. Prefer a dedicated low-privilege test account and a non-production workspace. Do not use this diagnostic as a production-capacity claim: choose a representative staging dataset and ramp concurrency in controlled steps before deciding an SLO or quota profile.

## 1.0.0 operations review (2026-10-22)

Performed on the reference Compose stack (`server/deploy/compose.yaml`: PostgreSQL 16, Control Plane image, NGINX TLS) running on a WSL2 Arch Linux host with 2 vCPU and ~2.9 GiB RAM. The load generator ran on the same host, so absolute throughput is a lower bound, not a capacity figure.

### Deployment defects found and fixed

| Defect | Effect before the fix | Fix |
|---|---|---|
| `control-plane` had a fixed `172.30.1.2` on `db_net`, while PostgreSQL started first with a dynamic address from the same range | every fresh `up` failed with `Address already in use` | no fixed addresses for the API; `proxy_net` dynamic allocation limited to `172.30.0.128/25` so nothing can take the trusted proxy address `172.30.0.2` |
| Password secret created as documented (`umask 077`) is owned by the host user, mode 0600 | `migrate` exited with `cannot read secret file` | DEPLOYMENT.md now hands the file to UID 10001 (mode 0400); the error names the errno and the UID that must read it |
| NGINX ran with `cap_drop: ALL` without `CHOWN` | proxy crash-looped on `chown /var/cache/nginx/client_temp` | `CHOWN` added to the minimal capability set |
| `docker compose exec control-plane healthcheck/verify-artifacts/admin …` bypass the image ENTRYPOINT | restore never saw readiness; documented admin commands failed (`executable file not found`) | scripts/docs call `python -m asgend_cp <command>` |
| `restore.sh` used `docker cp` (root-owned copies) and `pg_restore … -` | verify-only failed on 0600 backups; `pg_restore` treated `-` as a file name | files are streamed with `tar`/`cat` so UID 10001 owns them; `pg_restore` reads stdin without `-` |
| NGINX access log recorded `/v1/downloads/<ticket>` | short-lived download capabilities were written to proxy logs | dedicated `log_format` redacts the ticket path |

### Load and saturation (GET task list through NGINX TLS, new TLS connection per request)

| Scenario | Result |
|---|---|
| 1 token, 4–64 workers, 400 requests each | per-token limiter answers `429` once the 200-request burst is spent; no `5xx`, no transport errors |
| 4 tokens × 32 workers in parallel (1 600 requests) | 1 600 × `200` (one token also hit its own limit); ~145 req/s aggregate, p50 ≈ 0.75 s, p95 ≈ 1.3 s with CPU shared with the generator |
| Resource snapshot under that load | Control Plane ≈ 27 MiB RSS / 14 threads, NGINX ≈ 14 MiB, PostgreSQL ≈ 69 MiB, 15 of 16 pooled DB connections busy; 0 error-level log records |
| Compose drill (`server/scripts/compose_drill.sh`) | 200-request probe at 16 workers within the burst: 0 errors; bearer tokens and download tickets absent from Control Plane, NGINX and migrate logs |

### Decisions for 1.0.0

- **Single Control Plane replica.** The rate limiter, long-poll notifiers and maintenance loop are process-local. 1.0.0 supports exactly one API replica per database; running more replicas is unsupported until rate limiting moves to a shared store or a trusted gateway enforces equivalent per-principal limits.
- **Pool sizing.** `ASGEND_CP_DB_POOL_SIZE=16` saturates before PostgreSQL's default 100 connections; raise it only together with `max_connections`.
- **Rate limits.** Defaults (`1200/min`, burst `200` per principal/IP; `10/min`, burst `5` for pairing/enrollment) are the supported baseline. Tune per deployment after observing `asgend_cp_rate_limited_total`.

## Still requires operational sign-off

These are per-deployment duties; the 1.0.0 release review above covers the reference stack only.

- Confirm trusted proxy and client-IP settings before enabling IP-scoped rate limits behind a proxy.
- Run the load probe against your own staging host and data volume; review latency, errors, database connections, disk space and quota metrics before choosing SLOs.
- Re-run `server/scripts/compose_drill.sh` (or the equivalent backup/restore cycle) on the target platform before going live and after every upgrade.
- Review logs/metrics for secret leakage and high-cardinality labels with your own logging pipeline and retention.
- The stable release itself is approved by the maintainer through `release/versions.json` (`release_approved`), which the release-gate workflow enforces.
