#!/bin/sh
# End-to-end Compose deployment + backup/restore/readiness drill on a DISPOSABLE host.
#
# Builds and starts server/deploy/compose.yaml under a separate Compose project,
# seeds data through the public NGINX TLS endpoint (user + node protocol), runs
# server/scripts/backup.sh, mutates state, verifies the backup, restores it with
# server/scripts/restore.sh, restarts the API and checks that the seeded task,
# events, node token and artifact bytes survived while the post-backup change is
# gone. The stack, its volumes, the temporary secrets and the backup are removed
# afterwards unless ASGEND_DRILL_KEEP=1.
#
# Requirements: Docker Engine + Compose v2, openssl, python3. The Compose networks
# use fixed subnets, so do not run this next to a live ASGEND stack on one host.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
COMPOSE="$ROOT/server/deploy/compose.yaml"
export COMPOSE_PROJECT_NAME="${ASGEND_DRILL_PROJECT:-asgend-drill}"
HTTPS_PORT="${ASGEND_DRILL_HTTPS_PORT:-18443}"
HTTP_PORT="${ASGEND_DRILL_HTTP_PORT:-18080}"
PUBLIC_HOST=asgend-drill.test
NAME="drill-$(date -u +%Y%m%dT%H%M%SZ)"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/asgend-drill.XXXXXX")
STATE="$WORK/state.json"

step() { printf '\n==> %s\n' "$*"; }
dc() { docker compose -f "$COMPOSE" "$@"; }
client() { python3 "$ROOT/server/scripts/compose_drill_client.py" "$@" --url "https://127.0.0.1:$HTTPS_PORT" \
             --host-header "$PUBLIC_HOST" --ca-file "$WORK/tls/fullchain.pem" --state "$STATE"; }

cleanup() {
  result=$?
  trap - EXIT HUP INT TERM
  if [ "$result" -ne 0 ]; then
    echo "drill FAILED (exit $result); recent Control Plane logs:" >&2
    dc logs --no-color --tail 40 control-plane >&2 2>/dev/null || true
  fi
  if [ "${ASGEND_DRILL_KEEP:-0}" = "1" ]; then
    echo "ASGEND_DRILL_KEEP=1: leaving project $COMPOSE_PROJECT_NAME, $WORK and backups/$NAME" >&2
  else
    dc down -v --remove-orphans >/dev/null 2>&1 || true
    # Secret files were chowned to UID 10001; remove them from a container as well.
    docker run --rm -v "$WORK:/w" --entrypoint sh postgres:16-alpine -c 'rm -rf /w/secrets' >/dev/null 2>&1 || true
    rm -rf -- "$WORK" "$ROOT/backups/$NAME"
  fi
  exit "$result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for tool in docker openssl python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "missing required tool: $tool" >&2; exit 2; }
done
docker compose version >/dev/null

step "prepare disposable secrets and TLS certificate in $WORK"
mkdir -p "$WORK/secrets" "$WORK/tls"
(umask 077 && openssl rand -base64 36 > "$WORK/secrets/postgres_password.txt")
# The API containers run as UID 10001 and Compose keeps the host owner/mode of file secrets.
docker run --rm -v "$WORK/secrets:/s" --entrypoint sh postgres:16-alpine \
  -c 'chown 10001:10001 /s/postgres_password.txt && chmod 0400 /s/postgres_password.txt'
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=$PUBLIC_HOST" \
  -addext "subjectAltName=DNS:$PUBLIC_HOST,IP:127.0.0.1" \
  -keyout "$WORK/tls/privkey.pem" -out "$WORK/tls/fullchain.pem" >/dev/null 2>&1
chmod 0644 "$WORK/tls/privkey.pem"  # throwaway key; NGINX reads it with dropped capabilities
export ASGEND_CP_POSTGRES_PASSWORD_FILE="$WORK/secrets/postgres_password.txt"
export ASGEND_CP_TLS_DIR="$WORK/tls"
export ASGEND_CP_HTTPS_PORT="$HTTPS_PORT" ASGEND_CP_HTTP_PORT="$HTTP_PORT"
export ASGEND_CP_ALLOWED_HOSTS="localhost,127.0.0.1,$PUBLIC_HOST"

step "validate and start the stack (project $COMPOSE_PROJECT_NAME)"
dc config -q
dc up -d --build --wait --wait-timeout 300
dc ps --format '{{.Service}}: {{.Status}}'
dc exec -T control-plane python -m asgend_cp migrate --status

step "bootstrap workspace and owner through the admin CLI"
WS=$(dc exec -T control-plane python -m asgend_cp admin create-workspace --name drill |
     python3 -c 'import json,sys; print(json.load(sys.stdin)["workspace_id"])')
ASGEND_DRILL_OWNER_TOKEN=$(dc exec -T control-plane python -m asgend_cp admin create-user --name drill-owner \
     --workspace "$WS" --role owner | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')
export ASGEND_DRILL_OWNER_TOKEN

step "seed task, events and artifact through NGINX TLS"
client seed --workspace "$WS"
client verify --expect-mutation

step "backup ($NAME)"
"$ROOT/server/scripts/backup.sh" "$NAME"
client verify  # writers were stopped and restarted by the backup

step "mutate state after the backup"
client mutate
client verify --expect-mutation

step "verify-only restore preflight"
"$ROOT/server/scripts/restore.sh" "$NAME" --verify-only

step "destructive restore"
printf 'RESTORE %s\n' "$NAME" | "$ROOT/server/scripts/restore.sh" "$NAME"
client verify

step "API restart keeps restored state"
dc restart control-plane >/dev/null
dc up -d --wait --wait-timeout 120 >/dev/null
client verify

step "bounded load probe through NGINX TLS (${ASGEND_DRILL_LOAD_REQUESTS:-200} requests)"
ASGEND_CP_LOAD_TEST_TOKEN="$ASGEND_DRILL_OWNER_TOKEN" SSL_CERT_FILE="$WORK/tls/fullchain.pem" \
  python3 "$ROOT/scripts/smoke_control_plane_load.py" --base-url "https://127.0.0.1:$HTTPS_PORT" \
  --workspace "$WS" --requests "${ASGEND_DRILL_LOAD_REQUESTS:-200}" --workers "${ASGEND_DRILL_LOAD_WORKERS:-16}" \
  --timeout 5 --max-error-rate "${ASGEND_DRILL_LOAD_MAX_ERROR_RATE:-0}"
client verify

step "logs do not contain bearer tokens or download tickets"
LEAKS=$(dc logs --no-color control-plane nginx migrate 2>&1 | grep -cE '(ucp|ncp)_[A-Za-z0-9_-]{8,}|dlt_[A-Za-z0-9_-]{8,}' || true)
if [ "$LEAKS" -ne 0 ]; then
  echo "found $LEAKS log lines containing credentials or download tickets" >&2
  exit 1
fi

step "drill passed"
