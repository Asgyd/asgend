# ASGEND Control Plane — deployment and recovery

> The Control Plane/API image tag is `1.0.0`. The reference stack and the scripts below were verified end to end with `server/scripts/compose_drill.sh` on 2026-10-22; see [`../docs/SECURITY_OPERATIONS.md`](../docs/SECURITY_OPERATIONS.md) for the operations review and the per-deployment checks that remain your responsibility. 1.0.0 supports a single Control Plane replica per database.

The supplied Compose stack is the production-oriented example: PostgreSQL stores
control state, a named volume stores immutable artifact blobs, and NGINX handles
public TLS. The API container runs as UID/GID 10001, has a read-only root
filesystem, drops Linux capabilities, and is reachable only from the Compose
networks. It does not execute tasks; Execution Nodes remain separate.

## Prerequisites

- Docker Engine and the Docker Compose v2 plugin.
- A DNS name for the service and a TLS certificate/key whose SAN covers that name.
- A securely stored PostgreSQL password file. Keep secrets, private keys, and
  backup directories outside version control.

From the repository root:

```sh
cp server/deploy/.env.example server/deploy/.env
mkdir -p server/deploy/secrets server/deploy/tls backups
umask 077
openssl rand -base64 36 > server/deploy/secrets/postgres_password.txt
# Compose bind-mounts file secrets with the host owner and mode unchanged. The
# migrate/control-plane containers run as UID 10001 and must be able to read it
# (the PostgreSQL entrypoint reads it as root during first initialisation):
sudo chown 10001:10001 server/deploy/secrets/postgres_password.txt
sudo chmod 0400 server/deploy/secrets/postgres_password.txt
# Put the certificate chain and private key at:
#   server/deploy/tls/fullchain.pem
#   server/deploy/tls/privkey.pem
```

Edit `server/deploy/.env` and set `ASGEND_CP_ALLOWED_HOSTS` to a comma-separated
list of the actual accepted hostnames, including `localhost,127.0.0.1` for the
container readiness probe. Update `ASGEND_CP_TLS_DIR` and
`ASGEND_CP_POSTGRES_PASSWORD_FILE` if your secret/certificate directories differ.
Do not use the example domain in a live deployment. The supplied proxy network
uses `172.30.0.0/24` and the database network uses `172.30.1.0/24`; change both
subnets and `ASGEND_CP_TRUSTED_PROXIES` together if they overlap with your local
routing. The trusted-proxy setting must only identify the reverse proxy, not an
untrusted client network.

Start the stack:

```sh
docker compose --env-file server/deploy/.env -f server/deploy/compose.yaml config
docker compose --env-file server/deploy/.env -f server/deploy/compose.yaml up -d --build
docker compose --env-file server/deploy/.env -f server/deploy/compose.yaml ps
```

The one-shot `migrate` service applies versioned DB migrations before the API
starts. The Control Plane has a readiness healthcheck that fails if the database
is unavailable or the schema is not current. The API is not published directly
on a host port; public access goes through NGINX. Plain HTTP is permitted only
on the isolated internal control-plane listener behind this TLS proxy.

Create an initial workspace and owner via the administrative CLI. The token is
shown once; capture it in a password manager or secret store:

```sh
docker compose --env-file server/deploy/.env -f server/deploy/compose.yaml \
  exec control-plane python -m asgend_cp admin create-workspace --name production
```

The command returns a workspace ID. Create an owner account, optionally attaching
it to that workspace using `admin create-user --name ... --workspace ... --role owner`.
Use `python -m asgend_cp admin --help` from inside the container for other administrative options.
Keep admin tokens out of shell history, CI logs, tickets, and monitoring output.

## Operations

```sh
# Check API and readiness from inside the container
docker compose --env-file server/deploy/.env -f server/deploy/compose.yaml \
  exec control-plane python -m asgend_cp healthcheck --ready

# Check applied migration version
docker compose --env-file server/deploy/.env -f server/deploy/compose.yaml \
  exec control-plane python -m asgend_cp migrate --status

# Run retention and TTL cleanup once
docker compose --env-file server/deploy/.env -f server/deploy/compose.yaml \
  exec control-plane python -m asgend_cp cleanup
```

Configure the external firewall so only ports 80/443 are exposed. Keep the
PostgreSQL and API networks private. If enabling metrics, set a strong
`ASGEND_CP_METRICS_TOKEN` secret and restrict access to the metrics endpoint.
Review allowed CORS origins, rate limits, artifact quotas, retention periods,
log retention, hostnames and backup permissions before production use.

## Backup

The backup script briefly stops the reverse proxy and API so the PostgreSQL dump,
artifact archive and manifest refer to one quiescent state. PostgreSQL is
exported with `pg_dump`; the application creates `artifacts.tar.gz`,
`manifest.json` and `SHA256SUMS` in a private directory:

```sh
server/scripts/backup.sh before-upgrade-2026-10-21
server/scripts/restore.sh before-upgrade-2026-10-21 --verify-only
```

Store backups encrypted and off-host. A checksum manifest detects accidental or
in-transit modification; it is not a signature and does not authenticate the
backup's origin. Preserve the backup directory permissions and add an external
retention policy. Verification checks that the database dump itself is covered by
`SHA256SUMS`, validates manifest structure, re-hashes each content-addressed blob
inside the archive, and checks the SQLite dump's integrity/schema/artifact inventory
before restoring it. For PostgreSQL, `restore.sh --verify-only` also asks
`pg_restore --list` to validate the staged custom-format dump before any database
replacement. Large staging files are stored in a uniquely named hidden directory
on the artifact volume (outside `blobs/`) rather than the Compose `/tmp` tmpfs,
which is intentionally limited to 16 MiB; the script removes that directory on
success and attempts cleanup on failure. The restore script does not relax source
backup file permissions or mount a host backup directory into the API container.

## Restore drill

Restore is destructive to current database state and merges artifact blobs from
that backup. Run the verification-only command first, then schedule a maintenance
window and run the restore script interactively. The script asks for an explicit
confirmation, verifies the staged dump and artifact archive before mutation, stops
API writers, safely stages and merges content-addressed blobs, restores the PostgreSQL
snapshot, starts the stack, and checks artifact hashes. Artifact archive extraction
is staged first and individual blob files are atomically promoted; symlink entries
and symlinked destination trees are rejected. The target PostgreSQL
instance must be compatible with the source dump and the restored schema must be
supported by the deployed application. Test backup and restore against an
isolated disposable deployment before adopting a new release.

### Automated end-to-end drill

On a disposable Docker host (never next to a live stack: the networks use fixed
subnets), `server/scripts/compose_drill.sh` builds the image, starts the stack as
Compose project `asgend-drill`, seeds a task, events and an artifact through the
NGINX TLS endpoint, runs `backup.sh`, changes state, runs `restore.sh --verify-only`
and the destructive restore, restarts the API, verifies that the seeded data and
artifact bytes came back while the post-backup change is gone, runs a bounded load
probe and checks that no bearer token or download ticket reached the logs. It
removes the stack, volumes, temporary secrets and backup afterwards
(`ASGEND_DRILL_KEEP=1` keeps them for inspection). CI runs it in the
`compose-drill` job.

## Development

SQLite remains the low-friction dev/test backend and is covered by the Python
unit tests. For local Control Plane tests:

```sh
python3 -m unittest discover -s server/tests -p 'test_*.py' -v
python3 -m asgend_cp migrate --help  # from server/ with PYTHONPATH=.
```

The supplied stack and scripts are reference deployment tooling, not a claim that
a production environment has already been provisioned or load/security-audited.

## PostgreSQL CI acceptance

The `control-plane-node-e2e` workflow also provisions an isolated PostgreSQL 16
service and runs `server/tests/test_postgres_acceptance.py` using a unique schema
for each test. It begins at schema version 1 and exercises the real upgrade to the
current version, repeat migration as a no-op, Control Plane process restart,
command lease expiry/redelivery, idempotent task submission, acknowledgement
persistence, and event-cursor continuity. The job is intentionally separate from
SQLite unit tests. To run it locally, install `psycopg[binary]` and set
`ASGEND_TEST_POSTGRES_URL` to a disposable database where the test role may create
and drop schemas. The test drops its generated schema on cleanup; never point it
at a production database.
