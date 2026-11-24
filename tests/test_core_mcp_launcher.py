#!/usr/bin/env python3
"""Runtime regression for the single-threaded MCP launcher bootstrap."""
from __future__ import annotations
import importlib.util
import concurrent.futures
import ctypes
import errno
import json
import os
import platform
import sys
import tempfile
import time
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
script = PROJECT / "tests" / "integration.py"
saved_argv = sys.argv[:]
try:
    sys.argv = [str(script), str(BINARY), str(PROJECT)]
    spec = importlib.util.spec_from_file_location("asgend_core_mcp_launcher_integration", script)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load integration helpers")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
finally:
    sys.argv = saved_argv


def wait_until(predicate, timeout: float, label: str):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except (OSError, json.JSONDecodeError):
            pass
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {label}; last={last!r}")


def landlock_runtime_status() -> tuple[bool, str]:
    """Probe LANDLOCK_CREATE_RULESET_VERSION without changing process policy."""
    if not sys.platform.startswith("linux"):
        return False, f"Landlock requires Linux (platform={sys.platform})"
    if platform.machine().lower() not in {"x86_64", "amd64", "aarch64", "arm64", "riscv64"}:
        return False, f"Landlock syscall number is not validated on {platform.machine()}"
    libc = ctypes.CDLL(None, use_errno=True)
    # Linux assigns __NR_landlock_create_ruleset=444 on the supported 64-bit ABIs.
    result = libc.syscall(444, ctypes.c_void_p(), ctypes.c_size_t(0), ctypes.c_uint(1))
    if result >= 1:
        return True, f"Landlock ABI {result}"
    error = ctypes.get_errno()
    if error in {errno.ENOSYS, errno.EPERM, errno.EACCES, getattr(errno, "EOPNOTSUPP", 95)}:
        return False, f"Landlock syscall unavailable or blocked (errno={error}: {os.strerror(error)})"
    raise AssertionError(f"unexpected Landlock capability probe failure: errno={error} ({os.strerror(error)})")


def sandbox_dirs() -> set[str]:
    return {str(path) for path in Path("/tmp").glob("ao-mcp-sandbox-*") if path.is_dir()}


def assert_no_new_sandbox_dirs(before: set[str], label: str) -> None:
    leaked = sandbox_dirs() - before
    if leaked:
        # Give the supervisor's stop/cleanup path a short chance to finish.
        wait_until(lambda: not (sandbox_dirs() - before), timeout=2, label=f"cleanup of {label}")
    leaked = sandbox_dirs() - before
    assert not leaked, f"{label} leaked MCP sandbox workspace(s): {sorted(leaked)}"


with tempfile.TemporaryDirectory(prefix="ao-core-mcp-launcher-") as td:
    root = Path(td)
    # With sandbox enabled, verify the helper rebuilds the server allow-list
    # rather than inheriting environment variables from the multithreaded core.
    probe_name = f"launcher-env-{os.getpid()}.json"
    prior_secret = os.environ.get("ASGEND_TEST_PARENT_SECRET")
    os.environ["ASGEND_TEST_PARENT_SECRET"] = "must-not-leak-from-service"
    env_proc = None
    try:
        env_proc = module.start_server(
            module.free_port(), root / "env.sqlite3", mcp_servers=[{
                "name": "launcherenv", "command": sys.executable,
                "args": [str(PROJECT / "tests" / "mock_mcp_env_probe.py"), probe_name],
                "env": {"ASGEND_MCP_EXPLICIT_VALUE": "configured-for-this-server"},
                "timeout_seconds": 3,
            }], sandbox_enabled=True)
        probe_file = wait_until(
            lambda: next(iter(Path("/tmp").glob(f"ao-mcp-sandbox-*/{probe_name}")), None),
            timeout=5, label="MCP environment probe written inside its sandbox")
        env_data = json.loads(probe_file.read_text(encoding="utf-8"))
        assert env_data["parent_secret"] is None, env_data
        assert env_data["explicit_value"] == "configured-for-this-server", env_data
        assert env_data["path"] == "/usr/local/bin:/usr/bin:/bin", env_data
        assert env_data["ld_preload"] is None, env_data
        assert env_data["home"] == str(probe_file.parent), env_data
        assert env_data["tmpdir"] == str(probe_file.parent), env_data
        # The helper must preserve the sandbox identity and resource policy.
        if os.geteuid() == 0:
            assert env_data["euid"] == 65534 and env_data["egid"] == 65534, env_data
            assert env_data["groups"] == [], env_data
        assert env_data["limits"]["cpu"] == [120, 121], env_data
        assert env_data["limits"]["address_space"] == [512 * 1024 * 1024] * 2, env_data
        assert env_data["limits"]["file_size"] == [64 * 1024 * 1024] * 2, env_data
        assert env_data["limits"]["open_files"] == [64, 64], env_data
        assert env_data["limits"]["core"] == [0, 0], env_data
        assert env_data["network_socket_errno"] == 1, env_data  # EPERM from seccomp
        assert env_data["unix_socketpair_ok"] is True, env_data  # Renderer IPC remains available.
    finally:
        if env_proc is not None:
            module.stop_server(env_proc)
        if prior_secret is None:
            os.environ.pop("ASGEND_TEST_PARENT_SECRET", None)
        else:
            os.environ["ASGEND_TEST_PARENT_SECRET"] = prior_secret

    # Exercise the explicit Landlock allow-list when the host can create a
    # ruleset. The current test container may block syscall 444; in that case
    # the strict fail-closed branch below is still mandatory and is asserted.
    landlock_available, landlock_status = landlock_runtime_status()
    if landlock_available:
        root.chmod(0o755)
        allowed_dir = root / "landlock-allowed"
        denied_dir = root / "landlock-denied"
        allowed_dir.mkdir(mode=0o777)
        denied_dir.mkdir(mode=0o777)
        allowed_dir.chmod(0o777)
        denied_dir.chmod(0o777)
        (allowed_dir / "allowed.txt").write_text("allowed-read", encoding="utf-8")
        (allowed_dir / "execute-denied.sh").write_text("#!/bin/sh\nprintf 'EXECUTE_ALLOWED\n'\n", encoding="utf-8")
        (allowed_dir / "execute-denied.sh").chmod(0o755)
        (denied_dir / "denied.txt").write_text("denied-read", encoding="utf-8")
        for path in (allowed_dir / "allowed.txt", allowed_dir / "execute-denied.sh", denied_dir / "denied.txt"):
            path.chmod(0o644 if path.name != "execute-denied.sh" else 0o755)
        probe_name = f"landlock-probe-{os.getpid()}-{time.monotonic_ns()}.json"
        landlock_proc = None
        try:
            landlock_proc = module.start_server(
                module.free_port(), root / "landlock.sqlite3", mcp_servers=[{
                    "name": "landlockprobe", "command": sys.executable,
                    "args": [str(PROJECT / "tests" / "mock_mcp_landlock_probe.py"), probe_name],
                    "env": {
                        "ASGEND_TEST_ALLOWED_FILE": str(allowed_dir / "allowed.txt"),
                        "ASGEND_TEST_DENIED_FILE": str(denied_dir / "denied.txt"),
                        "ASGEND_TEST_READONLY_DIR": str(allowed_dir),
                        "ASGEND_TEST_OUTSIDE_WRITE": str(denied_dir / "outside-write.txt"),
                        "ASGEND_TEST_EXEC_FILE": str(allowed_dir / "execute-denied.sh"),
                    },
                    "read_only_paths": [str(allowed_dir)], "timeout_seconds": 3,
                }], sandbox_enabled=True, sandbox_require_landlock=True)
            probe_file = wait_until(
                lambda: next(iter(Path("/tmp").glob(f"ao-mcp-sandbox-*/{probe_name}")), None),
                timeout=5, label="Landlock filesystem probe inside MCP sandbox")
            policy = json.loads(probe_file.read_text(encoding="utf-8"))
            assert policy["allowed_read"] == {"ok": True, "text": "allowed-read"}, policy
            assert not policy["denied_read"]["ok"] and policy["denied_read"].get("errno") in {errno.EACCES, errno.EPERM}, policy
            assert policy["workspace_write"] == {"ok": True}, policy
            assert not policy["readonly_write"]["ok"] and policy["readonly_write"].get("errno") in {errno.EACCES, errno.EPERM}, policy
            assert not policy["outside_write"]["ok"] and policy["outside_write"].get("errno") in {errno.EACCES, errno.EPERM}, policy
            assert policy["execute_readonly_path"].get("allowed") is False, policy
            assert policy["execute_readonly_path"].get("errno") in {errno.EACCES, errno.EPERM}, policy
        finally:
            if landlock_proc is not None:
                module.stop_server(landlock_proc)
        print(f"Landlock allow/deny matrix passed ({landlock_status})", flush=True)
    else:
        before = sandbox_dirs()
        strict_probe_name = f"landlock-must-not-run-{os.getpid()}.json"
        try:
            failed_proc = module.start_server(
                module.free_port(), root / "landlock-required.sqlite3", mcp_servers=[{
                    "name": "landlockrequired", "command": sys.executable,
                    "args": [str(PROJECT / "tests" / "mock_mcp_env_probe.py"), strict_probe_name],
                    "timeout_seconds": 3,
                }], sandbox_enabled=True, sandbox_require_landlock=True)
        except AssertionError as failure:
            failure_text = str(failure)
            assert "server failed to start" in failure_text and "MCP sandbox setup failed" in failure_text, failure_text
        else:
            module.stop_server(failed_proc)
            raise AssertionError("require_landlock=true allowed MCP startup although Landlock is unavailable")
        assert_no_new_sandbox_dirs(before, "strict Landlock rejection")
        assert not list(Path("/tmp").glob(f"ao-mcp-sandbox-*/{strict_probe_name}")), "MCP executed despite required Landlock being unavailable"
        print(f"Landlock allow/deny matrix skipped; fail-closed startup verified ({landlock_status})", flush=True)

    # A failed MCP exec after sandbox creation must close child pipes and remove
    # its private workspace before the core reports initial startup failure.
    before_failed_exec = sandbox_dirs()
    missing_executable = root / "definitely-not-an-mcp-executable"
    try:
        failed_proc = module.start_server(
            module.free_port(), root / "failed-exec.sqlite3", mcp_servers=[{
                "name": "launcherfailexec", "command": str(missing_executable),
                "args": [], "timeout_seconds": 3,
            }], sandbox_enabled=True, sandbox_require_landlock=False)
    except AssertionError as failure:
        failure_text = str(failure)
        assert "server failed to start" in failure_text, failure_text
    else:
        module.stop_server(failed_proc)
        raise AssertionError("MCP with a missing executable unexpectedly completed startup")
    assert_no_new_sandbox_dirs(before_failed_exec, "failed MCP exec")

    # Without Landlock, inspect the executed MCP's descriptor table directly.
    # No control pipe or service descriptor may survive the final exec.
    fd_probe = root / "launcher-fds.json"
    fd_proc = module.start_server(
        module.free_port(), root / "fds.sqlite3", mcp_servers=[{
            "name": "launcherfds", "command": sys.executable,
            "args": [str(PROJECT / "tests" / "mock_mcp_env_probe.py"), str(fd_probe)],
            "timeout_seconds": 3,
        }], sandbox_enabled=False)
    try:
        fd_data = wait_until(
            lambda: json.loads(fd_probe.read_text(encoding="utf-8")) if fd_probe.exists() else None,
            timeout=5, label="MCP descriptor probe")
        assert fd_data["open_fds"] is not None, fd_data
        assert set(fd_data["open_fds"]).issubset({0, 1, 2}), fd_data
        assert fd_data["network_socket_errno"] is None, fd_data
    finally:
        module.stop_server(fd_proc)

    # A child that exits during its first tools/list is restarted by the
    # supervisor; the second incarnation completes the handshake.
    state_file = root / "mcp-restart-state.txt"
    restart_proc = module.start_server(
        module.free_port(), root / "restart.sqlite3", mcp_servers=[{
            "name": "launcherrestart", "command": sys.executable,
            "args": [str(PROJECT / "tests" / "mock_mcp_restart_server.py"), str(state_file)],
            "timeout_seconds": 3,
        }], sandbox_enabled=False)
    try:
        state_text = wait_until(
            lambda: state_file.read_text(encoding="utf-8")
            if state_file.exists() and "restart_ready" in state_file.read_text(encoding="utf-8") else None,
            timeout=12, label="supervisor restarts MCP through the launcher")
        assert "first_start" in state_text and "restart_start" in state_text and "restart_ready" in state_text, state_text
    finally:
        module.stop_server(restart_proc)

    # Regression for the startup/restart handshake window: the fixture writes
    # restart_ready after it has replied to tools/list but before the parent
    # necessarily flips MCPServer.started to true. Submit immediately at that
    # boundary; mcp_call_tool must wait on the lifecycle mutex instead of
    # returning a transient allocation failure.
    race_state = root / "mcp-restart-tool-race-state.txt"
    race_port = module.free_port()
    llm_port = module.free_port()
    llm_server = module.ThreadingHTTPServer(("127.0.0.1", llm_port), module.MockLLMHandler)
    llm_thread = module.threading.Thread(target=llm_server.serve_forever, daemon=True)
    llm_thread.start()
    race_agents = root / "restart-race-agents"
    module.shutil.copytree(PROJECT / "config" / "agents", race_agents)
    generalist_path = race_agents / "generalist.json"
    generalist_cfg = json.loads(generalist_path.read_text(encoding="utf-8"))
    generalist_cfg.setdefault("tools", []).append("mcp__racerestart__echo")
    generalist_path.write_text(json.dumps(generalist_cfg, ensure_ascii=False), encoding="utf-8")
    race_proc = None
    try:
        race_proc = module.start_server(
            race_port, root / "restart-tool-race.sqlite3", agents_dir=race_agents,
            llm_enabled=True, llm_base_url=f"http://127.0.0.1:{llm_port}/v1",
            mcp_servers=[{
                "name": "racerestart", "command": sys.executable,
                "args": [str(PROJECT / "tests" / "mock_mcp_restart_server.py"), str(race_state)],
                "timeout_seconds": 3,
            }], sandbox_enabled=False, runtime_workers=4)
        state_at_boundary = wait_until(
            lambda: race_state.read_text(encoding="utf-8")
            if race_state.exists() and "restart_ready" in race_state.read_text(encoding="utf-8") else None,
            timeout=12, label="MCP restart is at the post-tools/list boundary")
        assert "first_start" in state_at_boundary and "restart_start" in state_at_boundary, state_at_boundary
        # Hit the just-restarted server from several request threads and let
        # four core workers issue MCP calls concurrently. The MCP lifecycle
        # mutex must serialize descriptor/protocol use without losing replies.
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            submit_futures = [pool.submit(module.rpc, race_port, {
                "op": "submit", "task": f"Verify concurrent MCP call {index} after supervised restart",
                "agent": "generalist"}) for index in range(4)]
            submitted_rows = [future.result(timeout=5) for future in submit_futures]
        for submitted in submitted_rows:
            assert submitted.get("ok") and submitted.get("task_id"), submitted

        terminal_statuses = ("completed", "failed", "cancelled", "completed_without_model")
        def await_completed(submitted):
            terminal = wait_until(
                lambda: (lambda current: current if current.get("status") in terminal_statuses else None)(
                    module.rpc(race_port, {"op": "status", "task_id": submitted["task_id"]})),
                timeout=24, label=f"concurrent MCP tool call {submitted['task_id']} completes")
            assert terminal["status"] == "completed", terminal
            result_text = json.dumps(terminal.get("result", {}), ensure_ascii=False)
            assert "MCP_RESTART_RECOVERED:hello-from-agent" in result_text, terminal
            return terminal

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            terminal_rows = list(pool.map(await_completed, submitted_rows))
        assert len(terminal_rows) == 4, terminal_rows
    finally:
        if race_proc is not None:
            module.stop_server(race_proc)
        llm_server.shutdown()
        llm_server.server_close()
        llm_thread.join(timeout=2)

print("core MCP launcher tests passed")
