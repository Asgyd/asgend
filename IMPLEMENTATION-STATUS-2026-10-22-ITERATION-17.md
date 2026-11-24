# ASGEND implementation status — iteration 17 (1.0.0 stabilisation)

Date: 2026-10-22

## Scope

Close the remaining 1.0.0 release gates by actually running every toolchain that earlier iterations lacked (PostgreSQL, Docker/Compose, Qt 6, Flutter/Android, the long integration test), fix what those runs exposed, and move every component to 1.0.0. The only open item is the maintainer's approval in `release/versions.json`.

## Environment

WSL2 Arch Linux x86_64 on Windows 11 (8 vCPU / 12 GiB; the host `.wslconfig` limit of 2 vCPU / 3 GiB was raised temporarily because Gradle and the Compose stack together exhausted 3 GiB). GCC + CMake/Ninja, Python 3.14, Docker 29.9 + Compose 5.6, `postgres:16-alpine` (16.15) with matching PG16 client tools, Qt 6.12 (Arch), `ubuntu:24.04` (Qt 6.4.2) and `fedora:42` (Qt 6.10.2) containers, Flutter 3.29.3 / Dart 3.7.2, JDK 17, Android SDK 35, NDK 27, Android 14 emulator (KVM). Tests ran as an unprivileged user.

## Defects found and fixed

Deployment (none of this had ever run before):

- `compose.yaml`: API's fixed `172.30.1.2` on `db_net` collided with PostgreSQL's dynamic address → every fresh `up` failed. Fixed addresses removed for the API; `proxy_net` dynamic range limited to `.128/25` so nothing can take the trusted proxy's `.2`.
- `compose.yaml`: NGINX under `cap_drop: ALL` crash-looped on `chown` of temp dirs → `CHOWN` added.
- Password file created per DEPLOYMENT.md (`umask 077`) unreadable by UID 10001 → guide sets owner 10001 / mode 0400; `config.py` error now names errno and the UID.
- `restore.sh`: `compose exec … healthcheck/verify-artifacts` bypassed ENTRYPOINT (`executable file not found`); `docker cp` created root-owned files the helper could not read; `pg_restore … -` is invalid. All fixed; `backup.sh` streams the dump too. DEPLOYMENT.md admin/ops commands use `python -m asgend_cp`.
- NGINX access log wrote `/v1/downloads/<ticket>` → redacting `log_format`.
- Dockerfile: pip root warning silenced.

Clients:

- Qt: two compile errors (`QByteArray::FromBase64Result::operator->`, ambiguous `qBound<qint64>`).
- Flutter: APIs newer than pinned 3.29.3 (`RadioGroup`, `DropdownButtonFormField.initialValue`, `Utf8Decoder` on `Stream<Uint8List>`), three analyzer infos, widget test matching `FilledButton.icon` by exact type; `bootstrap_android_wrapper.sh` now creates `android/local.properties`; NDK pinned to 27.0.12077973 as required by plugins; `pubspec.lock` committed.

Tests/tooling:

- `test_postgres_backup_restore.py` reports a newer-client/older-server pg tool mismatch explicitly.
- `test_release_gate.py` no longer asserts the repository is blocked (it would fail once gates pass); uses synthetic manifests.
- New: `server/scripts/compose_drill.sh` + `compose_drill_client.py` (end-to-end Compose drill), unreadable-secret diagnostics test, CI jobs `core-integration` and `compose-drill`, Qt CI DEB install smoke.

## Results (clean run, 2026-10-22)

| Gate | Result |
|---|---|
| Version consistency / release-gate tests | passed (6/6) |
| Release build + CTest without `integration` | no warnings; 18/18 |
| Long `integration` | passed, 119 s |
| Contracts | 31 passed, 1 optional skip |
| Control Plane suite | 34 run, 31 passed, 3 skips by design |
| Node agent | 37/37 |
| Compiled core E2E | passed |
| PostgreSQL 16 acceptance | 2/2 |
| `pg_dump`/`pg_restore` drill | 1/1 |
| Compose drill (incl. load probe and log-leak check) | passed |
| Qt Arch / Ubuntu 24.04 DEB / Fedora 42 RPM | 8/8 tests each; packages install, launch, remove |
| Flutter analyze / test / APK | 0 issues / 20/20 / unsigned APK 1.0.0 (2) |
| Android emulator launch smoke | starts, no crash |
| Security/operations review + load ramp | see `docs/SECURITY_OPERATIONS.md` |

`python3 scripts/check_release_readiness.py` → one blocker: `release_approved=false`. The maintainer then approved the stable release the same day; `--strict` now reports READY (exit 0).

## Decisions

- `query --plan` / `--rerank` are experimental in 1.0.0; `memory_quality` gate is non-required (maintainer decision).
- One Control Plane replica per database in 1.0.0.
- Supported packages: TGZ/DEB/RPM (x86_64), unsigned APK (sign with your own key).

## Reproduction

```bash
python3 scripts/check_release_readiness.py
python3 tests/test_release_gate.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure --timeout 900        # as a non-root user
python3 tests/contracts/test_contracts.py
python3 -m unittest discover -s server/tests -p 'test_*.py'
python3 -m unittest discover -s node_agent/tests -p 'test_*.py'
ASGEND_TEST_POSTGRES_URL=postgresql://… python3 -m unittest discover -s server/tests -p 'test_postgres_*.py' -v
sh server/scripts/compose_drill.sh                              # disposable Docker host
cmake -S clients/asgend_qt -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-qt && QT_QPA_PLATFORM=offscreen ctest --test-dir build-qt
(cd clients/asgend_flutter && flutter analyze && flutter test && ./scripts/bootstrap_android_wrapper.sh && flutter build apk --release)
```

## Not done here

- Hosted CI runs were not observed (no repository remote); workflows run the same commands.
- Remote pairing on a physical Android device; human-judged retrieval evaluation; multi-replica Control Plane; aarch64; AppImage/Flatpak — listed in `TODO.md` under "После 1.0.0".
