# Android implementation Stage 9 — safe configuration update

Date: 2026-10-25
Status: **implemented in source; targeted runtime regression PASS; Android build acceptance pending**

## Problem

The previous Android guided configuration UI submitted a complete `{main, models, agents}` bundle through `config_save`. `config_get` intentionally omits legacy fixed-role agent profiles; full-bundle cleanup could delete those hidden files even when the user only edited an operational limit. The UI warning did not remove this destructive coupling.

## Changes

- Added `config_update_settings` to the client-independent TCP JSON RPC API. Its schema is an allow-list of explicitly supported server connection limits, runtime queue/worker settings, and sandbox switches/limits.
- Rejects unknown request fields, sections and keys; every value must have the exact JSON type and documented range. Booleans are not accepted as integers and floats/strings are rejected for numeric settings.
- Reads the current main config, merges only approved fields, writes a private staged candidate, validates the entire candidate through `config_load`, confirms that agent/model/database paths remain pinned, then atomically replaces only the main config. The original config is staged to a private rollback file during the commit durability window; failed validation leaves the source bytes unchanged; a directory fsync failure triggers rollback. A stale rollback file from an uncertain commit blocks subsequent updates rather than being overwritten. The running process reports `restart_required` because its in-memory snapshot does not hot-reload.
- Updated the legacy multi-file save cleanup to preserve hidden fixed-role `coder`, `planner`, and `reviewer` files omitted from `config_get`.
- Updated the Android repository and ViewModel to send only `RuntimeConfigDraft.toSettingsJson()`; secret/model/agent fields are not transmitted. The local transient config snapshot is updated only after success. The dialog describes partial-update semantics and retains explicit confirmation only for sandbox weakening.
- Updated protocol, client, README, native audit, decomposition, TODO and stage log.

## Verification evidence

| Check | Result |
|---|---|
| CMake Release configure/build of all targets | PASS |
| `core_config_safe_update` runtime test | PASS |
| Related config/string/HTTP/SQLite/MCP tests after final review | 6/6 PASS |
| `verify_native_source.py` | PASS |
| Python `py_compile` for new runtime test | PASS |

The runtime regression starts the real C core with a temporary configuration tree. It checks authenticated success, exact updated values, preservation of auth/provider secrets and paths, byte-identical models registry, unchanged legacy agent profile bytes, 0600 config permissions, rollback-file cleanup, fail-closed preservation of an unresolved rollback copy, strict rejection of bool-for-int/float/out-of-range/unknown/path changes, no partial writes on invalid input, no secret leakage in errors, and legacy full-bundle save not deleting hidden fixed-role profiles.

## Remaining gaps

- Full CTest suite and `integration` were not run as part of Stage 9; run them in the whole-project audit loop.
- Android SDK API 37/Gradle wrapper runtime is unavailable in this environment. Unit test source for the safe payload exists, but Gradle/JUnit and actual Compose compile/APK/device acceptance are **NOT RUN**.
- The legacy `config_save` remains a broader administrative operation; new partial setting forms should use `config_update_settings`.
- A failed rollback filesystem operation is reported as an uncertain commit and keeps the rollback copy rather than claiming a false failure/success. A subsequent partial update is refused until the retained copy is reconciled, so it cannot silently overwrite the only recovery copy.
