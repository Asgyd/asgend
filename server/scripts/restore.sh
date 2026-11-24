#!/bin/sh
# Restore a backup created by server/scripts/backup.sh. This is destructive to DB state.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 BACKUP_NAME [--verify-only]" >&2
  exit 2
fi
NAME=$1
MODE=${2:-}
case "$NAME" in
  ''|.*|*[!A-Za-z0-9._-]*) echo "invalid backup name" >&2; exit 2 ;;
esac
if [ -n "$MODE" ] && [ "$MODE" != "--verify-only" ]; then
  echo "unknown option: $MODE" >&2
  exit 2
fi
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
COMPOSE="$ROOT/server/deploy/compose.yaml"
BACKUP_DIR="$ROOT/backups/$NAME"
test -d "$BACKUP_DIR" || { echo "backup not found: $BACKUP_DIR" >&2; exit 1; }
test -s "$BACKUP_DIR/db.pgdump" || { echo "db.pgdump missing" >&2; exit 1; }

HELPER=""
STAGE=""
STOPPED=0
cleanup() {
  if [ -n "$HELPER" ] && [ -n "$STAGE" ]; then
    case "$STAGE" in /data/artifacts/.asgend-restore-*) docker exec "$HELPER" rm -rf -- "$STAGE" >/dev/null 2>&1 || true ;; esac
  fi
  if [ -n "$HELPER" ]; then docker rm -f "$HELPER" >/dev/null 2>&1 || true; fi
  if [ "$STOPPED" -eq 1 ]; then
    docker compose -f "$COMPOSE" up -d control-plane nginx >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT HUP INT TERM

# Preserve the source backup's security permissions. docker cp stages files in
# the helper container without making the host backup world-readable.
# Use an ephemeral container as a staging area; no host backup path is mounted.
HELPER=$(docker compose -f "$COMPOSE" run -d --no-deps --entrypoint sh control-plane -c 'sleep 600')
STAGE=$(docker exec "$HELPER" python -c 'import tempfile; print(tempfile.mkdtemp(prefix=".asgend-restore-", dir="/data/artifacts"))')
case "$STAGE" in /data/artifacts/.asgend-restore-*) ;; *) echo "unsafe restore staging path" >&2; exit 1 ;; esac
# Stream instead of `docker cp`: docker cp creates root-owned files, which the
# helper (UID 10001, no capabilities) cannot read when the backup is mode 0600.
tar -C "$BACKUP_DIR" -cf - . | docker exec -i "$HELPER" tar -C "$STAGE" --no-same-owner -xf -
docker exec "$HELPER" python -m asgend_cp restore --from "$STAGE" --verify-only
docker exec "$HELPER" cat "$STAGE/db.pgdump" | \
  docker compose -f "$COMPOSE" exec -T postgres pg_restore --list >/dev/null
if [ "$MODE" = "--verify-only" ]; then
  exit 0
fi
echo "WARNING: restoring '$NAME' replaces the PostgreSQL contents and merges its artifact blobs." >&2
printf "Type RESTORE %s to continue: " "$NAME" >&2
read -r CONFIRM
[ "$CONFIRM" = "RESTORE $NAME" ] || { echo "restore cancelled" >&2; exit 1; }

STOPPED=1
docker compose -f "$COMPOSE" stop --timeout 30 nginx control-plane
# Restore the file archive first. PostgreSQL is restored from the verified custom dump below.
docker exec "$HELPER" python -m asgend_cp restore --from "$STAGE" --force
docker exec "$HELPER" cat "$STAGE/db.pgdump" | \
  docker compose -f "$COMPOSE" exec -T postgres \
    pg_restore --clean --if-exists --no-owner --no-privileges -U asgend -d asgend

# Restart and remove the trap's automatic duplicate start; helper is discarded after DB restore.
docker compose -f "$COMPOSE" up -d control-plane nginx
STOPPED=0
# Wait for the restored schema and DB connectivity before checking blob integrity.
READY=0
ATTEMPT=0
while [ "$ATTEMPT" -lt 60 ]; do
  if docker compose -f "$COMPOSE" exec -T control-plane python -m asgend_cp healthcheck --ready >/dev/null 2>&1; then
    READY=1
    break
  fi
  sleep 1
  ATTEMPT=$((ATTEMPT + 1))
done
if [ "$READY" -ne 1 ]; then
  echo "Control Plane did not become ready after restore" >&2
  exit 1
fi
docker exec "$HELPER" rm -rf -- "$STAGE"
STAGE=""
docker rm -f "$HELPER" >/dev/null
HELPER=""
trap - EXIT HUP INT TERM
docker compose -f "$COMPOSE" exec -T control-plane python -m asgend_cp verify-artifacts
