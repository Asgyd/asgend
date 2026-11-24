# MCP Child Launch Remediation — Design and Acceptance Matrix

**Priority:** High  
**Status:** launcher implementation is in `src/mcp.c`/`src/main.c`; Stage 19 adds the Landlock allow/deny matrix, strict fail-closed and startup-cleanup checks, and four-way restart stress. Positive Landlock behavior still needs execution on a host that exposes the syscall.

## Problem

`src/mcp.c::start_server()` calls `fork()` and performs environment construction and sandbox setup in the child before `execvp()`. The MCP supervisor may execute this path from a thread while the service has other threads. `clearenv()`, `setenv()`, formatted diagnostics, path-policy construction and related libc work are not a safe general post-fork sequence for a multithreaded process.

## Implemented design

The service now forks only to perform the minimal descriptor/process-group setup and `execve()` the same binary in `--mcp-launcher` mode. The fork child uses `setpgid`, `dup2`, `fcntl`, `write`, descriptor closure, `execve` and `_exit`-style failure handling; it no longer builds environments or applies sandbox policy. The helper receives a fixed-layout, versioned and bounds-validated payload through control descriptor 3, closes the control descriptor, rebuilds only the MCP-specific configuration, then reuses the existing environment allow-list, resource limits, UID/GID drop, Landlock filesystem policy and seccomp network policy before executing the configured MCP target.

The payload does not contain the service-wide configuration or unrelated service credentials. The helper process is exec-created and single-threaded, and the target MCP inherits the same process group and stdin/stdout channels used by the supervisor. The control data is sent over a pipe instead of argv/environment variables. A plain `posix_spawn()` replacement is not used because the policy semantics require explicit preservation.

## Required policy equivalence matrix

| Policy / invariant | Existing behavior to preserve | Required validation |
|---|---|---|
| Environment | clear inherited environment; set fixed PATH/locale/HOME/TMPDIR; apply configured per-server allow-list | helper sees only the approved variables; sentinel parent secrets are absent |
| Process identity | when the service runs as root, clear supplementary groups and drop to UID/GID 65534 | inspect effective UID/GID/groups in a probe child |
| Resource limits | CPU, address space, file size, descriptor count, core dump disabled | child reports configured limits; each setup failure blocks exec |
| Filesystem sandbox | private workspace, Landlock read-only system/runtime paths and explicit configured paths, bounded write workspace | probe allowed and denied read/write/execute cases; required-Landlock failure remains fail-closed |
| Network sandbox | seccomp deny-network filter unless explicitly allowed; includes tested policy for relevant syscall families | network probe denied by default and allowed only with explicit config |
| File descriptors | only stdin/stdout plus intentional helper control descriptors survive; service sockets/credentials are not inherited | probe checks no unexpected descriptors survive exec |
| Process lifecycle | child process group, restart/backoff, cancellation, termination and reaping behavior | repeated crash/restart, blocked I/O, shutdown and descendant cleanup tests |
| Failure reporting | bounded diagnostics, no secret values, no hanging supervisor | inject failure at each stage and assert bounded, observable failure |

## Rollout steps

1. [x] Add a fixed-layout launcher protocol with version/magic validation, bounded counts and NUL-termination checks.
2. [x] Switch the production MCP launch path to the helper while keeping process group, stdin/stdout and environment policy semantics.
3. [x] Add runtime probes for environment isolation, UID/GID and groups, resource limits, default network denial, descriptor closure and supervisor restart.
4. [~] Add negative/positive Landlock read/write/execute tests for the explicit path policy. The matrix is implemented in `tests/mock_mcp_landlock_probe.py` and invoked by `core_mcp_launcher_smoke`; the current container returns `ENOSYS` for the Landlock version syscall, so the positive matrix was not executed here. Strict fail-closed behavior is tested instead.
5. [x] Inject an MCP exec failure after sandbox creation; assert startup failure is observable and no new private workspace remains. The strict required-Landlock failure also checks cleanup on this host.
6. [x] Stress the post-restart handshake with four simultaneous task submissions and four Core worker threads; every task must complete with the recovered MCP tool result. Shutdown/cancel stress beyond this scenario remains a follow-up.

## Acceptance criteria

- No environment or sandbox configuration runs between `fork()` and `execve()` in the multithreaded service child. Current source review confirms the branch is limited to process/descriptor operations and exec.
- Existing environment, UID/GID, resource-limit, network-socket denial and descriptor tests remain in place. Stage 19's `core_mcp_launcher_smoke` passes with startup-failure cleanup and four concurrent requests across the restart boundary.
- On this container the Landlock syscall returns `ENOSYS`; strict `require_landlock=true` rejection and cleanup pass, but positive/negative Landlock operations await a Landlock-capable Linux runner.
- Full CTest status is recorded in `docs/PROJECT_AUDIT_STAGE_19_CTEST_LOG_2026-10-27.txt`; hosted CI and production device acceptance are separate checks.
