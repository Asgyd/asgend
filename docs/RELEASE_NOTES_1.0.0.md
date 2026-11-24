# ASGEND 1.0.0 — release notes

Date: 2026-10-22. Status: **stable** — all required gates passed; approved by the maintainer on 2026-10-22 (`release/versions.json`).

## Components

| Component | Version | Artifact |
|---|---|---|
| Core (`agent-orchestrator`, `asgend-cli`, `asgend-memory`) | 1.0.0 | build from source (CMake) |
| Control Plane | 1.0.0 | Docker image `asgend-control-plane:1.0.0` (`server/Dockerfile`) |
| Remote API | 1.0.0 (wire version `1`) | `contracts/v1/openapi.yaml` |
| Execution Node agent | ships with Control Plane 1.0.0 | `node_agent/` |
| Qt 6 Linux client | 1.0.0 | `asgend-qt-1.0.0-x86_64.{tar.gz,deb,rpm}` |
| Android client (Flutter) | 1.0.0 (build 2) | unsigned `app-release.apk`; sign with your own key |

## Highlights

- Remote Control Plane with PostgreSQL storage, durable command delivery (lease/ack/redelivery), event cursors, resumable artifacts with SHA-256, download tickets, RBAC, audit, metrics and rate limits.
- Execution Node agent bridging the C core to the Control Plane.
- Native Qt 6 desktop client and Android client for local and remote modes.
- Project memory: BM25/hybrid retrieval, graph expansion, PDF and OOXML extraction, ANN index, `gc`/`stats`.

## Fixed during 1.0.0 stabilisation

Deployment (found by running the reference stack for the first time):

- Compose: the API's fixed `db_net` address collided with PostgreSQL's dynamic one, so a fresh `up` failed; dynamic allocation on `proxy_net` is now kept away from the trusted proxy address.
- Compose: NGINX could not start under `cap_drop: ALL` without `CHOWN`.
- File secret created per the guide was unreadable by the UID 10001 containers; the guide now sets ownership, and the error names the cause.
- `restore.sh`: readiness/verification calls bypassed the image entrypoint, `docker cp` produced files the helper could not read, and `pg_restore -` was not valid; all fixed. `backup.sh` streams the dump the same way.
- NGINX no longer writes download tickets (bearer capabilities in the URL path) to its access log.
- `DEPLOYMENT.md` admin/healthcheck/migrate/cleanup commands now work (`python -m asgend_cp …`).

Clients:

- Qt: two compile errors (`QByteArray::FromBase64Result` use, ambiguous `qBound`) — the client had not been compiled before.
- Flutter: code used APIs newer than the pinned Flutter 3.29.3 (`RadioGroup`, `DropdownButtonFormField.initialValue`, `Utf8Decoder` on `Stream<Uint8List>`); analyzer infos fixed; a widget test matched `FilledButton.icon` by exact type.
- Android: `bootstrap_android_wrapper.sh` now creates `android/local.properties`, without which a clean checkout could not build the APK.

## Upgrade from 1.0.0-rc.1

No API, schema or state-machine change: the database stays at schema version 3 and the `migrate` service is a no-op. Rebuild the image (`docker compose … up -d --build`); the tag changes from `1.0.0-rc.1` to `1.0.0`. If you deployed rc.1 with the old `compose.yaml`, take the new file: it removes the fixed API addresses and adds `CHOWN` for NGINX. Re-check your password-file ownership (UID 10001, mode 0400).

## Known limitations

- One Control Plane replica per database (rate limiting and long-poll notification are process-local).
- `asgend-memory query --plan` and `--rerank` are experimental: quality was checked only with synthetic fixtures.
- The Android app was verified by unit/widget tests, an APK build and a launch smoke test on an Android 14 (API 34) emulator; the remote pairing flow has not been exercised on a physical device, and QR scanning is not built in.
- Linux x86_64 only was built and tested; AppImage/Flatpak/PKGBUILD are not provided.
- Run the core test suite as an unprivileged user; as root the MCP sandbox drops to UID 65534 and cannot read files under a private home.

## Verification

See `IMPLEMENTATION-STATUS-2026-10-22-ITERATION-17.md` for commands, environment and results, and `docs/RELEASE_STATUS.md` for the gate ledger.
