#!/bin/sh
# Consistent PostgreSQL + artifact backup for the supplied Docker Compose stack.
set -eu

NAME=${1:-"asgend-$(date -u +%Y%m%dT%H%M%SZ)"}
case "$NAME" in
  ''|.*|*[!A-Za-z0-9._-]*) echo "invalid backup name" >&2; exit 2 ;;
esac
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
COMPOSE="$ROOT/server/deploy/compose.yaml"
OUT="$ROOT/backups/$NAME"
HELPER=""
STAGE=""
STOPPED=0
CREATED=0
COMPLETE=0

cleanup() {
  result=$?
  trap - EXIT HUP INT TERM
  if [ -n "$HELPER" ] && [ -n "$STAGE" ]; then
    case "$STAGE" in /data/artifacts/.asgend-backup-*) docker exec "$HELPER" rm -rf -- "$STAGE" >/dev/null 2>&1 || true ;; esac
  fi
  if [ -n "$HELPER" ]; then docker rm -f "$HELPER" >/dev/null 2>&1 || true; fi
  if [ "$STOPPED" -eq 1 ]; then
    docker compose -f "$COMPOSE" up -d control-plane nginx >/dev/null 2>&1 || true
  fi
  if [ "$CREATED" -eq 1 ] && [ "$COMPLETE" -ne 1 ]; then
    rm -rf -- "$OUT"
  fi
  exit "$result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$ROOT/backups"
if [ -e "$OUT" ] || [ -L "$OUT" ]; then
  echo "backup already exists; choose a new backup name: $NAME" >&2
  exit 1
fi
(umask 077 && mkdir "$OUT")
chmod 700 "$OUT"
CREATED=1

# Stop writers so pg_dump, artifact blobs and manifest describe one quiescent state.
STOPPED=1
docker compose -f "$COMPOSE" stop --timeout 30 nginx control-plane
docker compose -f "$COMPOSE" exec -T postgres \
  pg_dump --format=custom --no-owner --no-privileges -U asgend -d asgend > "$OUT/db.pgdump"

# Stage the database dump in a temporary Control Plane container; do not mount a
# writable host backup directory into the non-root production container.
HELPER=$(docker compose -f "$COMPOSE" run -d --no-deps --entrypoint sh control-plane -c 'sleep 600')
STAGE=$(docker exec "$HELPER" python -c 'import tempfile; print(tempfile.mkdtemp(prefix=".asgend-backup-", dir="/data/artifacts"))')
case "$STAGE" in /data/artifacts/.asgend-backup-*) ;; *) echo "unsafe backup staging path" >&2; exit 1 ;; esac
# Stream the dump so the helper user (UID 10001) owns it; `docker cp` would create
# a root-owned file that is unreadable to it under a restrictive host umask.
docker exec -i "$HELPER" sh -c 'cat > "$1"' sh "$STAGE/db.pgdump" < "$OUT/db.pgdump"
docker exec "$HELPER" python -m asgend_cp backup --out "$STAGE"
docker cp "$HELPER:$STAGE/." "$OUT/"
docker exec "$HELPER" rm -rf -- "$STAGE"
STAGE=""
docker rm -f "$HELPER" >/dev/null
HELPER=""

# Refuse a backup that does not contain the expected PostgreSQL dump or checksum manifest.
test -s "$OUT/db.pgdump"
test -s "$OUT/SHA256SUMS"
chmod 600 "$OUT"/*
COMPLETE=1
printf 'Backup created: %s\n' "$OUT"
