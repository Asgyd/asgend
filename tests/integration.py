#!/usr/bin/env python3
import json
import copy
import asyncio
import re
import os
import socket
import stat
import sqlite3
import subprocess
import sys
import tempfile
import time
import threading
import shutil
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BINARY = Path(sys.argv[1]).resolve()
PROJECT = Path(sys.argv[2]).resolve()
MEMORY_CLI = BINARY.with_name("asgend-memory")


def write_test_gguf(path: Path):
    def string(value):
        encoded = value.encode("utf-8")
        return len(encoded).to_bytes(8, "little") + encoded
    metadata = [
        ("general.architecture", 8, string("llama")),
        ("llama.block_count", 4, (4).to_bytes(4, "little")),
        ("llama.embedding_length", 4, (64).to_bytes(4, "little")),
        ("llama.attention.head_count", 4, (4).to_bytes(4, "little")),
        ("llama.attention.head_count_kv", 4, (2).to_bytes(4, "little")),
    ]
    body = b"GGUF" + (3).to_bytes(4, "little") + (0).to_bytes(8, "little") + len(metadata).to_bytes(8, "little")
    for key, value_type, value in metadata:
        body += string(key) + value_type.to_bytes(4, "little") + value
    path.write_bytes(body)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def rpc(port, payload, token="integration-secret", timeout=3):
    body = dict(payload)
    if token is not None:
        body["auth_token"] = token
    data = (json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(data)
        chunks = bytearray()
        while True:
            b = sock.recv(65536)
            if not b:
                break
            chunks.extend(b)
            if b"\n" in chunks:
                break
    line = bytes(chunks).split(b"\n", 1)[0]
    return json.loads(line.decode())


def run_plan_crud_smoke(port):
    """Exercise common To-do/Goal storage through the C core RPC contract."""
    marker = str(int(time.time() * 1000000))
    todo_title = f"PLAN_TODO_SMOKE_{marker}"
    goal_title = f"PLAN_GOAL_SMOKE_{marker}"
    todo = rpc(port, {"op": "plan_add", "scope_id": "global", "kind": "todo", "title": todo_title})
    goal = rpc(port, {"op": "plan_add", "scope_id": "global", "kind": "goal", "title": goal_title})
    assert todo.get("ok") and isinstance(todo.get("item_id"), int), todo
    assert goal.get("ok") and isinstance(goal.get("item_id"), int), goal
    try:
        listing = rpc(port, {"op": "plan_list", "scope_id": "global"})
        assert listing.get("ok") and any(row.get("item_id") == todo["item_id"] for row in listing.get("items", [])), listing
        assert any(row.get("item_id") == goal["item_id"] for row in listing.get("items", [])), listing

        current = rpc(port, {"op": "plan_wait", "revision": 0, "timeout_ms": 0})
        assert current.get("ok") and current.get("changed") is True and current.get("revision", 0) >= 2, current
        watcher_ready = threading.Event()
        notification = {}

        def wait_for_plan_change():
            watcher_ready.set()
            notification["reply"] = rpc(port, {"op": "plan_wait", "revision": current["revision"], "timeout_ms": 1500})

        watcher = threading.Thread(target=wait_for_plan_change, daemon=True)
        watcher.start()
        assert watcher_ready.wait(timeout=1), "plan notification watcher did not start"
        updated = rpc(port, {"op": "plan_update", "scope_id": "global", "item_id": todo["item_id"], "status": "done"})
        assert updated.get("ok"), updated
        watcher.join(timeout=3)
        assert not watcher.is_alive(), "plan_wait did not wake promptly after a mutation"
        changed = notification.get("reply", {})
        assert changed.get("ok") and changed.get("changed") is True and changed.get("revision", 0) > current["revision"], changed
        listed_todos = rpc(port, {"op": "plan_list", "scope_id": "global", "kind": "todo"})
        matched = [row for row in listed_todos.get("items", []) if row.get("item_id") == todo["item_id"]]
        assert matched and matched[0].get("status") == "done", listed_todos
        invalid_scope = rpc(port, {"op": "plan_add", "scope_id": "not-a-task-id", "kind": "goal", "title": "must not be created"})
        assert invalid_scope.get("ok") is False, invalid_scope
    finally:
        rpc(port, {"op": "plan_delete", "scope_id": "global", "item_id": todo["item_id"]})
        rpc(port, {"op": "plan_delete", "scope_id": "global", "item_id": goal["item_id"]})


def run_cli_fullscreen_tui(cli_path, env, task_id, todo_title, goal_title, expected_file,
                           workspace_root, launch_cwd, nested_dir_row, nested_filename, nested_content,
                           nested_file_row, drag_filename, drag_content, drag_file_row):
    """Exercise task-workspace preview, mouse selection, SIGWINCH and terminal restore."""
    import fcntl
    import pty
    import signal
    import struct
    import termios
    import select

    master_fd, slave_fd = pty.openpty()
    fcntl.ioctl(slave_fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 120, 0, 0))
    process = subprocess.Popen([str(cli_path)], stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
                               close_fds=True, env=env, cwd=str(launch_cwd), start_new_session=True)
    os.close(slave_fd)
    captured = bytearray()
    row_marker = re.compile(r"\x1b\[(\d+);1H\x1b\[2K")
    ansi_control = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")

    def screen_from_text(text):
        """Reconstruct the terminal rows from incremental CUP + erase-line updates."""
        matches = list(row_marker.finditer(text))
        rows = {}
        for index, match in enumerate(matches):
            next_start = matches[index + 1].start() if index + 1 < len(matches) else len(text)
            row_text = ansi_control.sub("", text[match.end():next_start])
            rows[int(match.group(1))] = row_text.rstrip("\r\n")
        return "\n".join(rows.get(row, "") for row in range(1, max(rows, default=0) + 1))

    def read_ready(timeout=0.0):
        """Append PTY output without blocking longer than the supplied timeout."""
        ready, _, _ = select.select([master_fd], [], [], timeout)
        if not ready:
            return False
        try:
            chunk = os.read(master_fd, 16384)
        except OSError:
            return False
        if not chunk:
            return False
        captured.extend(chunk)
        return True

    def get_current_screen():
        text = captured.decode("utf-8", errors="replace")
        return screen_from_text(text)

    def text_visible(screen, value):
        # Narrow panes can wrap in the middle of a word and each terminal row has
        # border glyphs. Compare letters/digits rather than requiring a raw substring.
        compact = lambda value: "".join(char.casefold() for char in value if char.isalnum())
        return compact(value) in compact(screen)

    def frame_count():
        # Count row writes rather than full-frame clears: the renderer updates dirty rows.
        return len(row_marker.findall(captured.decode("utf-8", errors="replace")))

    def wait_for_screen(predicate, timeout, description, after_frames=0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            read_ready(0.1)
            if frame_count() > after_frames and predicate(get_current_screen()):
                return get_current_screen()
            if process.poll() is not None:
                break
        raise AssertionError(
            f"TUI timed out waiting for {description}; returncode={process.poll()}; "
            f"current screen:\n{get_current_screen()[-5000:]}"
        )

    welcome_expected = ["Рабочее пространство", "Активные задачи", "Запрос"]
    expected = ["Чат задачи", "TO-DO", "GOALS", todo_title, goal_title, expected_file]
    deadline = time.monotonic() + 8
    try:
        while time.monotonic() < deadline:
            screen = get_current_screen()
            if all(fragment in screen for fragment in welcome_expected):
                break
            ready, _, _ = select.select([master_fd], [], [], 0.15)
            if ready:
                try:
                    chunk = os.read(master_fd, 16384)
                except OSError:
                    break
                if not chunk:
                    break
                captured.extend(chunk)
            if process.poll() is not None:
                break
        text = captured.decode("utf-8", errors="replace")
        screen = get_current_screen()
        assert all(fragment in screen for fragment in welcome_expected), (welcome_expected, screen[-4000:], text[-8000:], process.poll())
        # The startup directory is intentionally a decoy. The left pane must be
        # populated from the task workspace after selecting a task via /session.
        assert "CLI_CWD_SHOULD_NOT_APPEAR.txt" not in screen, screen[-3000:]
        os.write(master_fd, b"s")
        wait_for_screen(lambda current: "Выбор задачи" in current or "/session" in current,
                        timeout=3, description="open session picker")
        os.write(master_fd, task_id.encode("ascii"))
        wait_for_screen(lambda current: task_id in current, timeout=3,
                        description="filter sessions by task ID")
        before = frame_count()
        os.write(master_fd, b"\r")
        screen = wait_for_screen(lambda current: all(fragment in current for fragment in expected),
                                 timeout=6, description="open task workspace and plan", after_frames=before)
        assert "\x1b[?1049h" in text, "full-screen TUI must use the terminal alternate screen"
        assert "\x1b[?1002h" in text, "TUI should enable button-motion reporting for drag selection"
        # Check each mode in the latest rendered frame. Historical output is not
        # enough: otherwise a still-pending key can be consumed after the next mouse event.
        for key, expected_mode in ((b"1", "все раскрыты"),
                                   (b"2", "все скрыты"),
                                   (b"3", "динамика")):
            before = frame_count()
            os.write(master_fd, key)
            wait_for_screen(lambda screen, mode=expected_mode: mode in screen,
                            timeout=3, description=f"display mode {expected_mode}", after_frames=before)

        # First select the other row so the following directory click produces a real
        # dirty-row update. Clicking the already-selected row correctly emits no frame.
        file_y = 4 + 1
        before = frame_count()
        os.write(master_fd, f"\x1b[<0;15;{file_y}M".encode())
        wait_for_screen(lambda _screen: True, timeout=3,
                        description="file row mouse selection redraw", after_frames=before)
        directory_y = 4 + nested_dir_row
        before = frame_count()
        os.write(master_fd, f"\x1b[<0;15;{directory_y}M".encode())
        wait_for_screen(lambda _screen: True, timeout=3,
                        description="directory row mouse selection redraw", after_frames=before)
        before = frame_count()
        os.write(master_fd, b"\r")
        wait_for_screen(
            lambda screen: nested_filename in screen and drag_filename in screen,
            timeout=5, description="enter nested project directory", after_frames=before)

        # E2E drag-selection: press on the first fixture file, move with the
        # left button held (SGR motion bit), release, then Enter must open the
        # row reached by the drag rather than the row where the press began.
        source_y = 4 + nested_file_row
        target_y = 4 + drag_file_row
        before = frame_count()
        os.write(master_fd, f"\x1b[<0;15;{source_y}M".encode())
        wait_for_screen(lambda _screen: True, timeout=3,
                        description="drag-selection mouse press redraw", after_frames=before)
        before = frame_count()
        os.write(master_fd, f"\x1b[<32;15;{target_y}M".encode())
        wait_for_screen(lambda _screen: True, timeout=3,
                        description="drag-selection held-button motion redraw", after_frames=before)
        # A real SGR mouse release ends with lowercase m. It must not be
        # mistaken for the keyboard Escape key or close the file preview.
        os.write(master_fd, f"\x1b[<0;15;{target_y}m".encode())
        before = frame_count()
        os.write(master_fd, b"\r")
        dragged_screen = wait_for_screen(
            lambda screen: text_visible(screen, "opened by dragging") and text_visible(screen, "selection")
                and drag_filename in screen and "ФАЙЛ ·" in screen,
            timeout=5, description="Enter opens the file reached by drag-selection", after_frames=before)
        assert not text_visible(dragged_screen, nested_content), "drag should select the destination row, not the source file"

        # Close the dragged-to file and click the original file for the existing
        # streaming-preview, wheel and external-editor checks below.
        os.write(master_fd, b"\x1b")
        wait_for_screen(
            lambda screen: nested_filename in screen and drag_filename in screen and "Goals · MCP" in screen,
            timeout=3, description="close drag-selected file preview")
        before = frame_count()
        os.write(master_fd, f"\x1b[<0;15;{source_y}M".encode())
        wait_for_screen(lambda _screen: True, timeout=3,
                        description="select original nested file after drag", after_frames=before)
        before = frame_count()
        os.write(master_fd, b"\r")
        wait_for_screen(
            lambda screen: text_visible(screen, "opened from nested") and text_visible(screen, "directory")
                and nested_filename in screen and "ФАЙЛ ·" in screen,
            timeout=5, description="open nested workspace file preview", after_frames=before)

        # Horizontal SGR wheel enables no-wrap viewing and pans the file viewport.
        before_horizontal_frames = frame_count()
        os.write(master_fd, b"\x1b[<67;110;5M")
        horizontal_deadline = time.monotonic() + 3
        while time.monotonic() < horizontal_deadline:
            ready, _, _ = select.select([master_fd], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master_fd, 16384)
                except OSError:
                    break
                if not chunk:
                    break
                captured.extend(chunk)
            current_text = captured.decode("utf-8", errors="replace")
            current_screen = screen_from_text(current_text)
            if (frame_count() > before_horizontal_frames and
                    "без переноса" in current_screen):
                break
        current_text = captured.decode("utf-8", errors="replace")
        current_screen = screen_from_text(current_text)
        assert frame_count() > before_horizontal_frames, "horizontal wheel should redraw the TUI"
        assert "без переноса" in current_screen, current_screen[-5000:]
        preview_start = "opened from nested workspa"
        assert not text_visible(current_screen, preview_start), "horizontal wheel should pan the long preview line"
        os.write(master_fd, b"\x1b[<66;110;5M")
        pan_back_deadline = time.monotonic() + 3
        while time.monotonic() < pan_back_deadline:
            current_text = captured.decode("utf-8", errors="replace")
            current_screen = screen_from_text(current_text)
            if text_visible(current_screen, preview_start):
                break
            ready, _, _ = select.select([master_fd], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master_fd, 16384)
                except OSError:
                    break
                if not chunk:
                    break
                captured.extend(chunk)
        assert text_visible(current_screen, preview_start), current_screen[-5000:]

        # SGR wheel-down/up events scroll the preview, not merely select a panel row.
        before_wheel_frames = frame_count()
        os.write(master_fd, b"\x1b[<65;110;5M")
        wheel_down_deadline = time.monotonic() + 3
        while time.monotonic() < wheel_down_deadline:
            ready, _, _ = select.select([master_fd], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master_fd, 16384)
                except OSError:
                    break
                if not chunk:
                    break
                captured.extend(chunk)
            current_text = captured.decode("utf-8", errors="replace")
            current_screen = screen_from_text(current_text)
            if (frame_count() > before_wheel_frames and
                    not text_visible(current_screen, preview_start) and "wheel-line-02" in current_screen):
                break
        current_text = captured.decode("utf-8", errors="replace")
        current_screen = screen_from_text(current_text)
        assert frame_count() > before_wheel_frames, "wheel-down should redraw the TUI"
        assert "wheel-line-02" in current_screen and not text_visible(current_screen, preview_start), current_screen[-5000:]

        before_wheel_frames = frame_count()
        os.write(master_fd, b"\x1b[<64;110;5M")
        wheel_up_deadline = time.monotonic() + 3
        while time.monotonic() < wheel_up_deadline:
            ready, _, _ = select.select([master_fd], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master_fd, 16384)
                except OSError:
                    break
                if not chunk:
                    break
                captured.extend(chunk)
            current_text = captured.decode("utf-8", errors="replace")
            current_screen = screen_from_text(current_text)
            if (frame_count() > before_wheel_frames and
                    text_visible(current_screen, preview_start) and "wheel-line-00" in current_screen):
                break
        current_text = captured.decode("utf-8", errors="replace")
        current_screen = screen_from_text(current_text)
        assert frame_count() > before_wheel_frames, "wheel-up should redraw the TUI"
        assert text_visible(current_screen, preview_start) and "wheel-line-00" in current_screen, current_screen[-5000:]

        # Local project files are preview-only in this view; task workspace editing is
        # exercised separately through the authenticated workspace API tests.

        # Escape closes the preview; Tab returns focus to files; b navigates to the parent.
        os.write(master_fd, b"\x1b")
        wait_for_screen(
            lambda screen: nested_filename in screen and drag_filename in screen and
                "Goals · MCP" in screen,
            timeout=3, description="close the file preview")
        before = frame_count()
        os.write(master_fd, b"\t")
        wait_for_screen(lambda screen: nested_filename in screen and drag_filename in screen,
                        timeout=3, description="focus the files pane", after_frames=before)
        before = frame_count()
        os.write(master_fd, b"b")
        parent_screen = wait_for_screen(
            lambda screen: "nested/" in screen and expected_file in screen,
            timeout=4, description="navigate to parent directory", after_frames=before)
        assert "nested/" in parent_screen and expected_file in parent_screen, parent_screen[-5000:]

        # Force and wait for the resize path explicitly because a detached PTY may
        # not have a controlling terminal to deliver SIGWINCH automatically.
        fcntl.ioctl(master_fd, termios.TIOCSWINSZ, struct.pack("HHHH", 34, 140, 0, 0))
        before = frame_count()
        os.kill(process.pid, signal.SIGWINCH)
        wait_for_screen(lambda _screen: True, timeout=3,
                        description="redraw after SIGWINCH", after_frames=before)
        # First q leaves the selected task chat; a second q exits the welcome screen.
        os.write(master_fd, b"q")
        wait_for_screen(lambda screen: "Активные задачи" in screen,
                        timeout=3, description="return to welcome after task chat")
        os.write(master_fd, b"q")
        process.wait(timeout=4)
        drain_deadline = time.monotonic() + 1.0
        while time.monotonic() < drain_deadline:
            ready, _, _ = select.select([master_fd], [], [], 0.1)
            if not ready:
                break
            try:
                chunk = os.read(master_fd, 16384)
            except OSError:
                break
            if not chunk:
                break
            captured.extend(chunk)
        end_text = captured.decode("utf-8", errors="replace")
        assert process.returncode == 0, (process.returncode, end_text[-4000:])
        assert "\x1b[?1049l" in end_text, "TUI should restore the normal terminal buffer on exit"
        assert "\x1b[?1006l" in end_text and "\x1b[?1000l" in end_text and "\x1b[?1002l" in end_text, "TUI should disable mouse reporting on exit"
    finally:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        os.close(master_fd)


def run_cli_two_client_plan_sync(cli_path, env, port):
    """Verify plan changes notify simultaneously open TUI clients without manual refresh."""
    import fcntl
    import pty
    import struct
    import termios
    import select

    sessions = []
    todo_item = None
    goal_item = None
    todo_title = "TUI_TWO_CLIENT_SYNC_TODO"
    goal_title = "TUI_TWO_CLIENT_SYNC_GOAL"

    def wait_for(session, fragment, after=0, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = session["captured"].decode("utf-8", errors="replace")
            if fragment in text[after:]:
                return text
            if session["process"].poll() is not None:
                break
            ready, _, _ = select.select([session["master"]], [], [], 0.12)
            if ready:
                try:
                    chunk = os.read(session["master"], 16384)
                except OSError:
                    break
                if not chunk:
                    break
                session["captured"].extend(chunk)
        text = session["captured"].decode("utf-8", errors="replace")
        raise AssertionError(f"two-client TUI timed out waiting for {fragment!r}: {text[-5000:]}")

    try:
        for _ in range(2):
            master_fd, slave_fd = pty.openpty()
            fcntl.ioctl(slave_fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 120, 0, 0))
            process = subprocess.Popen([str(cli_path)], stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
                                       close_fds=True, env=env, start_new_session=True)
            os.close(slave_fd)
            session = {"master": master_fd, "process": process, "captured": bytearray()}
            sessions.append(session)
            wait_for(session, "Рабочее пространство", timeout=6)

        # Client one creates a shared To-do through the TUI prompt.
        first = sessions[0]
        second = sessions[1]
        second_cursor = len(second["captured"].decode("utf-8", errors="replace"))
        cursor = len(first["captured"].decode("utf-8", errors="replace"))
        os.write(first["master"], b"t")
        wait_for(first, "Новый пункт To-do", after=cursor)
        os.write(first["master"], (todo_title + "\n").encode())
        wait_for(first, "○ " + todo_title, after=cursor)

        # Client two must receive the change from the core notification API without pressing r.
        wait_for(second, "○ " + todo_title, after=second_cursor)

        # Client two creates a Goal; client one should be notified automatically as well.
        first_cursor = len(first["captured"].decode("utf-8", errors="replace"))
        cursor = len(second["captured"].decode("utf-8", errors="replace"))
        os.write(second["master"], b"g")
        wait_for(second, "Новая Goal", after=cursor)
        os.write(second["master"], (goal_title + "\n").encode())
        wait_for(second, "○ " + goal_title, after=cursor)

        wait_for(first, "○ " + goal_title, after=first_cursor)
        print("[integration] simultaneous TUI clients receive To-do/Goals notifications without refresh", flush=True)
    finally:
        for session in sessions:
            if session["process"].poll() is None:
                try:
                    os.write(session["master"], b"q")
                    session["process"].wait(timeout=4)
                except Exception:
                    try:
                        os.killpg(session["process"].pid, 9)
                    except ProcessLookupError:
                        pass
                    session["process"].wait(timeout=2)
            os.close(session["master"])
        try:
            for title in (todo_title, goal_title):
                found = rpc(port, {"op": "plan_list", "scope_id": "global"})
                if found.get("ok"):
                    for item in found.get("items", []):
                        if item.get("title") == title:
                            rpc(port, {"op": "plan_delete", "scope_id": "global", "item_id": item["item_id"]})
        except Exception:
            pass


def unix_rpc(path, payload, token="integration-secret", timeout=3):
    body = dict(payload)
    if token is not None: body["auth_token"] = token
    data = (json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout); sock.connect(str(path)); sock.sendall(data)
        chunks = bytearray()
        while True:
            part = sock.recv(65536)
            if not part: break
            chunks.extend(part)
            if b"\n" in chunks: break
    return json.loads(bytes(chunks).split(b"\n", 1)[0].decode())

def wait_for(predicate, timeout=8, label="condition"):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except (OSError, ConnectionError, TimeoutError, json.JSONDecodeError):
            pass
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {label}; last={last!r}")


def start_server(port, db_path, token="integration-secret", mcp_servers=None, llm_enabled=False, llm_base_url=None,
                 agents_dir=None, ui_enabled=False, model_registry=None, agent_model_id=None, resource_dirs=None,
                 removed_cwd=False, lsp_servers=None, mcp_modern=False, llm_timeout_seconds=5, sandbox_enabled=True,
                 project_root=None, sandbox_require_landlock=False, runtime_workers=1):
    config_path = db_path.parent / "config.json"
    effective_mcp_servers = copy.deepcopy(mcp_servers or [])
    if mcp_modern:
        for mcp_cfg in effective_mcp_servers:
            mcp_args = mcp_cfg.get("args", []) if isinstance(mcp_cfg, dict) else []
            if any("mock_mcp_server.py" in str(arg) for arg in mcp_args):
                explicit_env = mcp_cfg.setdefault("env", {})
                if not isinstance(explicit_env, dict):
                    raise AssertionError("test MCP env must be an object")
                explicit_env["ASGEND_MCP_MODERN_MOCK"] = "1"
    if agents_dir is None:
        effective_agents_dir = db_path.parent / "agents"
        if effective_agents_dir.exists(): shutil.rmtree(effective_agents_dir)
        shutil.copytree(PROJECT / "config" / "agents", effective_agents_dir)
    else:
        effective_agents_dir = Path(agents_dir)
    if agent_model_id:
        generalist_path = Path(effective_agents_dir) / "generalist.json"
        generalist_data = json.loads(generalist_path.read_text(encoding="utf-8"))
        generalist_data["model_id"] = agent_model_id
        generalist_path.write_text(json.dumps(generalist_data, ensure_ascii=False), encoding="utf-8")
    models_path = db_path.parent / "models.json"
    if model_registry is None:
        if llm_enabled:
            legacy_registry = json.loads((PROJECT / "config" / "models.json").read_text(encoding="utf-8"))
            legacy_registry.setdefault("defaults", {})["decision_model_id"] = ""
            legacy_registry.setdefault("defaults", {})["analysis_model_id"] = ""
            legacy_registry["models"] = []
            models_path.write_text(json.dumps(legacy_registry, ensure_ascii=False, indent=2), encoding="utf-8")
        else:
            shutil.copyfile(PROJECT / "config" / "models.json", models_path)
    else:
        models_path.write_text(json.dumps(model_registry, ensure_ascii=False, indent=2), encoding="utf-8")
    config = {
        "server": {"host": "127.0.0.1", "port": port, "auth_token": token,
                   "max_request_bytes": 65536, "max_connections": 24, "request_timeout_seconds": 4,
                   "unix_socket": {"enabled": True, "path": str(db_path.parent / "runtime.sock")}},
        "runtime": {"workers": runtime_workers, "queue_max_pending": 100, "max_retries": 2, "priority_aging_seconds": 2,
                    "database_path": str(db_path)},
        "local_llm": {"enabled": llm_enabled, "base_url": llm_base_url or "http://127.0.0.1:11434/v1",
                      "model": "mock-model", "api_key": "", "timeout_seconds": llm_timeout_seconds,
                      "temperature": 0.0},
        "paths": {"asgend_home": str(db_path.parent / "asgend-home"),
                  "agents_dir": str(effective_agents_dir),
                  "models_config": str(models_path),
                  "skills_dir": str((resource_dirs or {}).get("skills", PROJECT / "skills")),
                  "knowledge_dir": str((resource_dirs or {}).get("knowledge", PROJECT / "knowledge")),
                  "system_prompts_dir": str((resource_dirs or {}).get("system_prompts", PROJECT / "system_prompts")),
                  "tools_dir": str((resource_dirs or {}).get("tools", PROJECT / "tools")),
                  "instructions_dir": str((resource_dirs or {}).get("instructions", PROJECT / "instructions"))},
        "mcp": {"servers": effective_mcp_servers},
        "lsp": {"servers": lsp_servers or []},
        # Some hosted CI containers block Landlock; production defaults are strict/fail-closed.
        "sandbox": {"enabled": sandbox_enabled, "require_landlock": sandbox_require_landlock, "allow_network": False,
                    "cpu_seconds": 120, "memory_mb": 512, "file_size_mb": 64, "open_files": 64},
        "ui": {"enabled": ui_enabled, "port": free_port() if ui_enabled else 8766,
               "auth_token": "ui-test-token-with-at-least-32-characters",
               "directory": str(PROJECT / "ui")},
    }
    if project_root is not None:
        config["paths"]["project_root"] = str(Path(project_root))
    config_path.write_text(json.dumps(config), encoding="utf-8")
    launch = [str(BINARY), "-c", str(config_path)]
    launch_cwd = None
    if removed_cwd:
        launch_cwd = db_path.parent / "deleted-process-cwd"
        launch_cwd.mkdir()
        # Remove the subprocess' current directory immediately before exec to
        # reproduce a service launched from a deleted interactive-shell folder.
        prelude = "import os,sys; os.rmdir(os.getcwd()); os.execv(sys.argv[1], sys.argv[1:])"
        launch = [sys.executable, "-c", prelude, *launch]
    child_env = os.environ.copy()
    proc = subprocess.Popen(launch, cwd=launch_cwd, env=child_env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True, start_new_session=True)
    proc.ui_port = config["ui"]["port"] if ui_enabled else None
    proc.test_port = port
    try:
        wait_for(lambda: (rpc(port, {"op": "ping"}) if proc.poll() is None else None),
                 timeout=6, label="server startup")
    except Exception as exc:
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, 15)
            except ProcessLookupError:
                pass
        try:
            _, startup_stderr = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            _, startup_stderr = proc.communicate(timeout=3)
        raise AssertionError(f"server failed to start on port {port}; stderr={startup_stderr.strip()}") from exc
    return proc


def stop_server(proc):
    def signal_group(sig):
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            if proc.poll() is None:
                try:
                    proc.send_signal(sig)
                except ProcessLookupError:
                    pass

    if proc.poll() is None:
        signal_group(15)
    try:
        _, stderr = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        signal_group(9)
        try:
            proc.kill()
        except ProcessLookupError:
            pass
        try:
            _, stderr = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired as exc:
            raise AssertionError("server process group did not close its pipes promptly") from exc
        raise AssertionError("server did not stop promptly")
    stderr = stderr or ""
    if proc.returncode not in (0, -15):
        raise AssertionError(f"server exited with {proc.returncode}: {stderr}")
    return stderr


def run_cli_pty_regression(cli_path, env):
    """Drive the legacy line-oriented C CLI through a real PTY and inspect redraw bytes."""
    import pty
    import select

    master_fd, slave_fd = pty.openpty()
    process = subprocess.Popen([str(cli_path), "--no-tui"], stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
                               close_fds=True, env=env, start_new_session=True)
    os.close(slave_fd)
    captured = bytearray()
    cursor = 0

    def wait_for_pattern(pattern, timeout=40):
        nonlocal cursor
        compiled = re.compile(pattern, re.DOTALL)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = captured.decode("utf-8", errors="replace")
            match = compiled.search(text, cursor)
            if match:
                cursor = match.end()
                return match
            remaining = max(0.0, deadline - time.monotonic())
            ready, _, _ = select.select([master_fd], [], [], min(0.1, remaining))
            if ready:
                try:
                    chunk = os.read(master_fd, 8192)
                except OSError:
                    break
                if not chunk:
                    break
                captured.extend(chunk)
            elif process.poll() is not None:
                break
        excerpt = captured.decode("utf-8", errors="replace")[-6000:]
        raise AssertionError(f"PTY CLI timed out waiting for {pattern!r}; returncode={process.poll()}; output tail:\n{excerpt}")

    def send(line):
        os.write(master_fd, line.encode("utf-8"))

    try:
        wait_for_pattern(r"Выбор>")
        send("2\n")
        wait_for_pattern(r"Опишите задачу:")
        send("NEEDS_CLARIFICATION: PTY regression for live status and streamed assistant output\n")
        wait_for_pattern(r"Приоритет 0–9 \[5\]:")
        send("\n")
        wait_for_pattern(r"Задача создана:")
        wait_for_pattern(r"Уточнение от агента")
        send("1\n")
        wait_for_pattern(r"✓ Задача завершена\.")
        send("q\n")
        process.wait(timeout=5)
        # Read residual bytes after the process exits.
        while True:
            ready, _, _ = select.select([master_fd], [], [], 0)
            if not ready:
                break
            try:
                part = os.read(master_fd, 8192)
            except OSError:
                break
            if not part:
                break
            captured.extend(part)
    except Exception:
        if process.poll() is None:
            try:
                os.killpg(process.pid, 9)
            except ProcessLookupError:
                process.kill()
        process.wait(timeout=3)
        raise
    finally:
        os.close(master_fd)

    text = captured.decode("utf-8", errors="replace")
    if process.returncode != 0:
        raise AssertionError(f"PTY CLI exited {process.returncode}; output tail:\n{text[-6000:]}")
    # The capture is intentionally from the PTY byte stream, so these are real
    # cursor-clear/redraw writes rather than duplicated status lines in logs.
    frames = [glyph for glyph in "|/-\\" if f"\x1b[2K{glyph} Статус:" in text]
    assert len(frames) >= 2, f"expected at least two spinner glyphs via PTY, got {frames}; output tail:\n{text[-8000:]}"
    assert "\x1b[2K" in text, "TTY progress should clear/redraw its status row"
    assert "Уточнение от агента" in text and "Ответ отправлен" in text, text[-8000:]
    # This test drives the legacy line-oriented CLI with --no-tui. A PTY is
    # used to exercise real spinner redraw bytes, but it does not turn this
    # mode into the full-screen TUI and must not require a decorative answer box.
    assert "✓ Задача завершена." in text, text[-8000:]
    assert "Mock model stage output; prior_context=false" in text, text[-8000:]
    assert "╭─ ASGEND · ответ модели" not in text, text[-8000:]
    assert "\x1b[?1049h" not in text, "--no-tui must not enter the alternate screen"
    assert "Статус:" in text and "Результат:" in text, text[-8000:]
    print(f"[integration] PTY C CLI spinner redraw + stream: {len(frames)} glyph types", flush=True)


def make_inline_browser_test_document(project, token, task_one_id, task_two_id):
    # Build the real UI against a deterministic in-page API for locked-down browsers.
    html = (project / "ui" / "index.html").read_text(encoding="utf-8")
    css = (project / "ui" / "style.css").read_text(encoding="utf-8")
    app_js = (project / "ui" / "app.js").read_text(encoding="utf-8")
    mock_api = r'''<script>
(() => {
  const memory = () => { const values = new Map(); return {getItem:k=>values.has(k)?values.get(k):null,setItem:(k,v)=>values.set(k,String(v)),removeItem:k=>values.delete(k),clear:()=>values.clear()}; };
  Object.defineProperty(window,'sessionStorage',{configurable:true,value:memory()});
  Object.defineProperty(window,'localStorage',{configurable:true,value:memory()});
  sessionStorage.setItem('ao_ui_token',__TOKEN__);
  const now = new Date().toISOString();
  const one = {task_id:__TASK_ONE__,title:'First browser task',task:'First browser task',original_task:'First browser task',status:'completed_without_model',priority:8,effective_priority:8,attempts:1,max_attempts:3,created_at:now,updated_at:now,workspace_path:'/mock/tasks/one',result:{}};
  const two = {task_id:__TASK_TWO__,title:'Second browser task',task:'Second browser task',original_task:'Second browser task',status:'completed_without_model',priority:5,effective_priority:5,attempts:1,max_attempts:3,created_at:now,updated_at:now,workspace_path:'/mock/tasks/two',result:{}};
  const state = {tasks:[two,one], files:{}, answers:[], failNextWrite:false, holdNextWrite:false, writeEntered:false, releaseWrite:null};
  state.files[one.task_id]={'from-ui.txt':'editor API','another.txt':'second file'};
  state.files[two.task_id]={'second.txt':'second task file'};
  window.__aoTestState=state;
  const response=(data,status=200)=>({ok:status>=200&&status<300,status,json:async()=>data});
  window.fetch=async(input,options={})=>{
    const path=String(input).split('?')[0], method=options.method||'GET';
    let body={}; try{body=options.body?JSON.parse(options.body):{}}catch{}
    if(path==='/api/health')return response({ok:true,workers:1,tasks_by_status:{completed_without_model:2,queued:0,running:0,failed:0}});
    if(path==='/api/agents')return response({ok:true,agents:[{id:'generalist',name:'Generalist',description:'test',model_id:'',skills:[],tools:[]} ]});
    if(path==='/api/models')return response({ok:true,models:[],decision_models:[],nodes:[],default_model_id:'',decision_model_id:''});
    if(path==='/api/configs')return response({ok:true,main:{},models:{models:[],decision_models:[],nodes:[],defaults:{}},agents:[{id:'generalist',name:'Generalist',system_prompt:'',skills:[],tools:[]}],restart_required:false});
    if(path==='/api/resources')return response({ok:true,directories:{},files:{skills:[],system_prompts:[],tools:[],knowledge:[],instructions:[]},decision_model_id:'',classification_parallel:false,selection_mode:'local_bm25',resource_index_auto_sync:true});
    if(path==='/api/tasks'&&method==='GET')return response({ok:true,tasks:state.tasks});
    if(path==='/api/tasks'&&method==='POST'){
      const id='mock-new-task'; const row={task_id:id,title:body.task||id,task:body.task||id,status:'queued',priority:body.priority||5,effective_priority:body.priority||5,attempts:0,max_attempts:3,created_at:now,updated_at:now,result:{}};
      state.tasks.unshift(row);state.files[id]={};return response({ok:true,...row});
    }
    const match=path.match(/^\/api\/tasks\/([^/]+)(?:\/(events|files|cancel|answer))?$/);
    if(match){
      const id=decodeURIComponent(match[1]), suffix=match[2]||'', row=state.tasks.find(t=>t.task_id===id);
      if(!row)return response({ok:false,error:'task not found'},404);
      if(suffix==='events')return response({ok:true,events:[]});
      if(suffix==='cancel')return response({ok:true});
      if(suffix==='answer'){state.answers.push({taskId:id,answer:String(body.answer||'')});return response({ok:true});}
      if(suffix==='files'){
        const files=state.files[id]||(state.files[id]={});
        if(body.action==='list')return response({ok:true,path:body.path||'',entries:Object.entries(files).map(([name,content])=>({name,kind:'file',size:new TextEncoder().encode(content).length}))});
        if(body.action==='read')return Object.prototype.hasOwnProperty.call(files,body.path)?response({ok:true,path:body.path,content:files[body.path],size:new TextEncoder().encode(files[body.path]).length}):response({ok:false,error:'file not found'},404);
        if(body.action==='write'){
          if(state.failNextWrite){state.failNextWrite=false;return response({ok:false,error:'forced browser save failure'},500);}
          files[body.path]=String(body.content||'');const result={ok:true,path:body.path,size:new TextEncoder().encode(files[body.path]).length};
          if(state.holdNextWrite){state.holdNextWrite=false;state.writeEntered=true;return await new Promise(resolve=>{state.releaseWrite=()=>resolve(response(result));});}
          return response(result);
        }
        return response({ok:false,error:'unsupported file action'},400);
      }
      return response({ok:true,...row});
    }
    return response({ok:false,error:'unknown mock endpoint: '+path},404);
  };
})();
</script>'''
    mock_api = (mock_api.replace('__TOKEN__', json.dumps(token))
                .replace('__TASK_ONE__', json.dumps(task_one_id))
                .replace('__TASK_TWO__', json.dumps(task_two_id)))
    html = html.replace('<link rel="stylesheet" href="/assets/style.css">', '<style>' + css + '</style>')
    html = html.replace('<script src="/assets/app.js" defer></script>', '')
    html = html.replace('</body>', mock_api + '<script>' + app_js + '</script></body>')
    return html


async def run_browser_editor_regression(ui_port, token, task_one_id, task_two_id):
    """Exercise editor navigation and save races in a real headless browser."""
    import os as _os
    from playwright.async_api import async_playwright

    chromium_path = "/usr/bin/chromium"
    if not _os.path.exists(chromium_path):
        print("[integration] browser editor regression skipped (system Chromium unavailable)", flush=True)
        return False

    async with async_playwright() as playwright:
        browser = await playwright.chromium.launch(executable_path=chromium_path, headless=True,
                                                   args=["--no-sandbox", "--disable-dev-shm-usage"])
        context = await browser.new_context()
        page = await context.new_page()
        page_errors = []
        using_mock_api = False
        page.on("pageerror", lambda error: page_errors.append(str(error)))
        await page.add_init_script("sessionStorage.setItem('ao_ui_token', " + json.dumps(token) + ");")
        try:
            await page.goto(f"http://127.0.0.1:{ui_port}/", wait_until="domcontentloaded", timeout=10000)
            await page.wait_for_function("() => document.querySelector('#connection')?.textContent === 'Подключено'", timeout=10000)
        except Exception as exc:
            if "ERR_BLOCKED_BY_ADMINISTRATOR" not in str(exc):
                raise
            await page.close()
            page = await context.new_page()
            page.on("pageerror", lambda error: page_errors.append(str(error)))
            using_mock_api = True
            await page.set_content(make_inline_browser_test_document(PROJECT, token, task_one_id, task_two_id),
                                   wait_until="domcontentloaded", timeout=10000)
            await page.wait_for_function("() => document.querySelector('#connection')?.textContent === 'Подключено'", timeout=10000)
            print("[integration] loopback blocked by Chromium policy; editor browser test uses in-page API fixture", flush=True)
        await page.locator('button[data-view-target="chat"]').click()
        first_task = page.locator(f'#workspaceTaskList button[data-workspace-task="{task_one_id}"]')
        await first_task.wait_for(timeout=10000)
        await first_task.click()
        await page.locator('#workspaceFiles button[data-workspace-file="from-ui.txt"]').wait_for(timeout=10000)
        await page.locator('#workspaceFiles button[data-workspace-file="another.txt"]').wait_for(timeout=10000)
        editor = page.locator("#workspaceFileEditor")
        await page.locator('#workspaceFiles button[data-workspace-file="from-ui.txt"]').click()
        await editor.wait_for(state="visible", timeout=5000)
        await editor.fill("dirty file-switch draft")
        assert await page.locator("#workspaceEditorState").inner_text() == "Не сохранено"

        # Declining to open another file must keep the current file and draft.
        page.once("dialog", lambda dialog: asyncio.create_task(dialog.dismiss()))
        await page.locator('#workspaceFiles button[data-workspace-file="another.txt"]').click()
        await page.wait_for_timeout(150)
        assert await page.locator("#workspaceEditorName").inner_text() == "from-ui.txt"
        assert await editor.input_value() == "dirty file-switch draft"
        # Accepting the next confirmation allows an intentional file change.
        page.once("dialog", lambda dialog: asyncio.create_task(dialog.accept()))
        await page.locator('#workspaceFiles button[data-workspace-file="another.txt"]').click()
        await page.wait_for_function("() => document.querySelector('#workspaceEditorName')?.textContent === 'another.txt'", timeout=5000)

        # Task switch: dismissal preserves the dirty buffer; acceptance switches.
        await page.locator('#workspaceFiles button[data-workspace-file="from-ui.txt"]').click()
        await editor.fill("dirty task-switch draft")
        second_task = page.locator(f'#workspaceTaskList button[data-workspace-task="{task_two_id}"]')
        page.once("dialog", lambda dialog: asyncio.create_task(dialog.dismiss()))
        await second_task.click()
        await page.wait_for_timeout(150)
        assert await page.locator("#detailId").inner_text() == task_one_id
        assert await editor.input_value() == "dirty task-switch draft"
        page.once("dialog", lambda dialog: asyncio.create_task(dialog.accept()))
        await second_task.click()
        await page.wait_for_function("expected => document.querySelector('#detailId')?.textContent === expected", arg=task_two_id, timeout=10000)
        await page.locator('#workspaceFiles button[data-workspace-file="second.txt"]').wait_for(timeout=10000)
        await page.locator('#workspaceFiles button[data-workspace-file="second.txt"]').click()
        await editor.fill("draft on second task")

        # New-file and submit-new-task actions are guarded while the draft is dirty.
        await page.locator("#workspaceNewFileName").fill("must-not-exist.txt")
        page.once("dialog", lambda dialog: asyncio.create_task(dialog.dismiss()))
        await page.locator("#workspaceCreateFile").click()
        await page.wait_for_timeout(150)
        assert await editor.input_value() == "draft on second task"
        assert await page.locator('#workspaceFiles button[data-workspace-file="must-not-exist.txt"]').count() == 0
        await page.locator('button[data-view-target="dashboard"]').click()
        await page.locator("#task").fill("This submit must be cancelled by the editor guard")
        page.once("dialog", lambda dialog: asyncio.create_task(dialog.dismiss()))
        await page.locator("#submitButton").click()
        await page.wait_for_timeout(150)
        assert await page.locator("#task").input_value() == "This submit must be cancelled by the editor guard"
        # Return to the editor; accept a switch back to task one.
        await page.locator('button[data-view-target="chat"]').click()
        page.once("dialog", lambda dialog: asyncio.create_task(dialog.accept()))
        await page.locator(f'#workspaceTaskList button[data-workspace-task="{task_one_id}"]').click()
        await page.wait_for_function("expected => document.querySelector('#detailId')?.textContent === expected", arg=task_one_id, timeout=10000)
        await page.locator('#workspaceFiles button[data-workspace-file="from-ui.txt"]').click()
        await editor.fill("draft survives a failed save")

        # A forced HTTP failure must leave both the text and dirty badge intact.
        async def fail_write(route):
            try:
                data = json.loads(route.request.post_data or "{}")
            except json.JSONDecodeError:
                data = {}
            if data.get("action") == "write":
                await route.fulfill(status=500, content_type="application/json",
                                    body=json.dumps({"ok": False, "error": "forced browser save failure"}))
            else:
                await route.continue_()
        if using_mock_api:
            await page.evaluate("window.__aoTestState.failNextWrite = true")
        else:
            await page.route(f"**/api/tasks/{task_one_id}/files", fail_write)
        await page.locator("#workspaceSaveFile").click()
        await page.wait_for_function("() => document.querySelector('#workspaceFileMessage')?.textContent.includes('forced browser save failure')", timeout=5000)
        assert await editor.input_value() == "draft survives a failed save"
        assert await page.locator("#workspaceEditorState").inner_text() == "Не сохранено"
        if not using_mock_api:
            await page.unroute(f"**/api/tasks/{task_one_id}/files", fail_write)

        # The server stores the first snapshot while the response is held. Editing
        # during the wait must remain dirty after that older snapshot is acknowledged.
        save_observed = asyncio.Event()
        async def delay_write_response(route):
            data = {}
            try:
                data = json.loads(route.request.post_data or "{}")
            except json.JSONDecodeError:
                pass
            if data.get("action") == "write" and data.get("path") == "from-ui.txt":
                response = await route.fetch()
                save_observed.set()
                await asyncio.sleep(0.8)
                await route.fulfill(response=response)
            else:
                await route.continue_()
        if using_mock_api:
            await page.evaluate("window.__aoTestState.holdNextWrite = true; window.__aoTestState.writeEntered = false")
        else:
            await page.route(f"**/api/tasks/{task_one_id}/files", delay_write_response)
        await editor.fill("first snapshot in-flight")
        await page.locator("#workspaceSaveFile").click()
        if using_mock_api:
            await page.wait_for_function("() => window.__aoTestState.writeEntered === true", timeout=5000)
        else:
            await asyncio.wait_for(save_observed.wait(), timeout=5)
        await editor.fill("first snapshot in-flight plus concurrent edit")
        if using_mock_api:
            await page.evaluate("window.__aoTestState.releaseWrite()")
        await page.wait_for_function("() => document.querySelector('#workspaceFileMessage')?.textContent.includes('Сохранено') && !document.querySelector('#workspaceFileMessage')?.textContent.includes('Сохраняем')", timeout=5000)
        await page.wait_for_timeout(100)
        assert await editor.input_value() == "first snapshot in-flight plus concurrent edit"
        assert await page.locator("#workspaceEditorState").inner_text() == "Не сохранено"
        if not using_mock_api:
            await page.unroute(f"**/api/tasks/{task_one_id}/files", delay_write_response)

        await page.locator("#workspaceSaveFile").click()
        await page.wait_for_function("() => document.querySelector('#workspaceEditorState')?.textContent === 'Сохранено'", timeout=5000)
        async def read_saved_file():
            return await page.evaluate("async ({taskId, token}) => { const r = await fetch(`/api/tasks/${encodeURIComponent(taskId)}/files`, {method:'POST',headers:{'Content-Type':'application/json','X-AO-Token':token},body:JSON.stringify({action:'read',path:'from-ui.txt'})}); return await r.json(); }",
                                      {"taskId": task_one_id, "token": token})
        stored = await read_saved_file()
        assert stored.get("content") == "first snapshot in-flight plus concurrent edit", stored

        # Multiple clarification questions must be presented and submitted together.
        await page.evaluate("""() => renderChatEvent({event_id:987654,type:'orchestrator_waiting_input',payload:{questions:[
          {question:'Where should it run?',options:[{id:'local',label:'Local'}]},
          {question:'Which tests?',options:[{id:'full',label:'Full suite'}]}
        ]}})""")
        await page.wait_for_function("() => document.querySelector('#questionDialog')?.open && document.querySelectorAll('.clarification-question').length === 2", timeout=5000)
        answers = page.locator('.clarification-answer')
        await answers.nth(0).fill('Local device')
        await answers.nth(1).fill('Full integration suite')
        await page.locator('#answerSubmit').click()
        await page.wait_for_function("() => !document.querySelector('#questionDialog')?.open && window.__aoTestState.answers.length === 1", timeout=5000)
        batch_answer = (await page.evaluate('window.__aoTestState.answers[0].answer'))
        assert 'Local device' in batch_answer and 'Full integration suite' in batch_answer, batch_answer

        # Internal reasoning is represented as a collapsible progress process,
        # but its private token text must never be rendered into the UI.
        await page.evaluate("""() => renderChatEvent({event_id:987655,type:'llm_thinking_delta',created_at:new Date().toISOString(),payload:{stream_id:'browser-thinking-stream',delta:'PRIVATE_REASONING_SHOULD_NOT_RENDER'}})""")
        thinking_process = page.locator('#taskChatEvents .chat-process').filter(has_text='Thinking/reasoning').last
        await thinking_process.wait_for(timeout=3000)
        assert 'PRIVATE_REASONING_SHOULD_NOT_RENDER' not in await page.locator('#taskChatEvents').inner_text()
        thinking_detail = await thinking_process.inner_text()
        assert 'байт' in thinking_detail and 'содержимое скрыто' in thinking_detail.lower(), thinking_detail
        await page.evaluate("""() => renderChatEvent({event_id:987656,type:'tool_call_stream_delta',created_at:new Date().toISOString(),payload:{stream_id:'browser-tool-stream',call_key:'browser-tool-stream-0',index:0,tool_name:'files.read',phase:'arguments',argument_bytes:28}})""")
        tool_process = page.locator('#taskChatEvents .chat-process').filter(has_text='files.read').last
        await tool_process.wait_for(timeout=3000)
        tool_text = await tool_process.inner_text()
        assert 'Аргументы вызова формируются' in tool_text and '{' not in tool_text, tool_text

        # Node/resource/agent administration is grouped beneath Settings, and the
        # Add LLM dialog exposes the full profile schema rather than a prompt().
        await page.locator('button[data-view-target="settings"]').click()
        for element_id in ('settings-nodes','settings-agents','settings-resources'):
            assert await page.locator(f'#{element_id}').evaluate("el => !!el.closest('#view-settings')"), element_id
        await page.locator('#addModel').click()
        await page.wait_for_function("() => document.querySelector('#modelDialog')?.open", timeout=3000)
        for field in ('id','name','model_name','base_url','api_type','args','first_token_timeout_seconds',
                      'stream_idle_timeout_seconds','total_timeout_seconds','node_id','api_key'):
            assert await page.locator(f'#model-field-{field}').count() == 1, field
        await page.locator('#model-field-id').fill('browser-test-llm')
        await page.locator('#model-field-name').fill('Browser Test LLM')
        await page.locator('#model-field-model_name').fill('test-model')
        await page.locator('#modelFormSave').click()
        assert 'browser-test-llm' in await page.locator('#modelsConfigEditor').input_value()
        assert not page_errors, page_errors
        await browser.close()
    print("[integration] browser editor guards, multi-question dialog, settings and model form", flush=True)
    return True


class MockLLMHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    delay_seconds = 0.0
    keepalive_after_done_seconds = 0.8
    reset_after_partial_remaining = 0
    reasoning_first_token_remaining = 0
    fault_lock = threading.Lock()

    def _write_chunk(self, data: bytes):
        if not data:
            return
        self.wfile.write(f"{len(data):X}\r\n".encode("ascii"))
        self.wfile.write(data)
        self.wfile.write(b"\r\n")
        self.wfile.flush()

    def _send_fault_stream(self, mode):
        if mode in ("http_404", "http_500", "http_500_secret_body"):
            status = 404 if mode == "http_404" else 500
            error_text = ("UPSTREAM_SECRET_MARKER_DO_NOT_PERSIST" if mode == "http_500_secret_body"
                          else f"mock SSE failure {status}")
            body = json.dumps({"error": error_text}).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if mode == "cancel_before_headers":
            # Hold the request before sending status/headers so cancellation is
            # exercised while libcurl is still waiting for response headers.
            time.sleep(5.0)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        if mode in ("no_first_token", "idle_after_token", "cancel_stream"):
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            if mode != "no_first_token":
                payload = {"choices": [{"index": 0, "delta": {"content": "first token"}, "finish_reason": None}]}
                self._write_chunk(b"data: " + json.dumps(payload, separators=(",", ":")).encode() + b"\n\n")
            time.sleep(5.0 if mode == "cancel_stream" else 2.5)
            try:
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            except OSError:
                pass
            return
        if mode == "reset_after_partial":
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            payload = {"choices": [{"index": 0, "delta": {"content": "partial before reset"}, "finish_reason": None}]}
            try:
                self._write_chunk(b"data: " + json.dumps(payload, separators=(",", ":")).encode() + b"\n\n")
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.connection.close()
            except OSError:
                pass
            return

        if mode == "malformed_json":
            body = b"data: {not-valid-json}\n\n"
        elif mode == "missing_done":
            payload = {"choices": [{"index": 0, "delta": {"content": "partial answer"}, "finish_reason": None}]}
            body = b"data: " + json.dumps(payload, separators=(",", ":")).encode() + b"\n\n"
        else:
            body = b"data: [DONE]\n\n"
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_stream(self, response):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        def event(data):
            wire = b"data: " + json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode() + b"\n\n"
            # Force SSE field/JSON boundaries across transport chunks.
            for offset in range(0, len(wire), 11):
                self._write_chunk(wire[offset:offset + 11])

        message = response["choices"][0]["message"]
        event({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
               "choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}]})
        if response.pop("_test_reasoning_first_token", False):
            # Visible answer text is delayed beyond the configured first-token
            # deadline, but reasoning deltas prove generation is active meanwhile.
            event({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                   "choices": [{"index": 0, "delta": {"reasoning_content": "Thinking token one. "}, "finish_reason": None}]})
            time.sleep(0.65)
            event({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                   "choices": [{"index": 0, "delta": {"reasoning_content": "Thinking token two."}, "finish_reason": None}]})
            time.sleep(0.65)
        content = message.get("content")
        if isinstance(content, str) and content:
            width = max(1, min(19, (len(content) + 2) // 3))
            for offset in range(0, len(content), width):
                event({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                       "choices": [{"index": 0, "delta": {"content": content[offset:offset + width]}, "finish_reason": None}]})
        for index, call in enumerate(message.get("tool_calls", [])):
            function = call.get("function", {})
            args = function.get("arguments", "")
            event({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                   "choices": [{"index": 0, "delta": {"tool_calls": [{"index": index, "id": call.get("id", ""),
                       "type": call.get("type", "function"), "function": {"name": function.get("name", ""),
                       "arguments": args[:max(1, len(args) // 2)]}}]}, "finish_reason": None}]})
            if args[max(1, len(args) // 2):]:
                event({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                       "choices": [{"index": 0, "delta": {"tool_calls": [{"index": index,
                           "function": {"arguments": args[max(1, len(args) // 2):]}}]}, "finish_reason": None}]})
        finish = "tool_calls" if "tool_calls" in message else "stop"
        event({"id": "chatcmpl-mock", "object": "chat.completion.chunk",
               "choices": [{"index": 0, "delta": {}, "finish_reason": finish}]})
        done = b"data: [DONE]\n\n"
        for offset in range(0, len(done), 3):
            self._write_chunk(done[offset:offset + 3])
        # The client must treat [DONE] as terminal without waiting for the peer
        # to close its HTTP/1.1 keep-alive connection.
        time.sleep(self.keepalive_after_done_seconds)
        try:
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except OSError:
            pass

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.delay_seconds:
            time.sleep(self.delay_seconds)
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode())
        messages = request.get("messages", [])
        combined_text = "\n".join(str(m.get("content") or "") for m in messages if isinstance(m, dict))
        if request.get("stream") is True:
            for marker, mode in (("__SSE_HTTP_404__", "http_404"), ("__SSE_HTTP_500__", "http_500"),
                                 ("__SSE_HTTP_500_SECRET_BODY__", "http_500_secret_body"),
                                 ("__SSE_MALFORMED_JSON__", "malformed_json"), ("__SSE_MISSING_DONE__", "missing_done"),
                                 ("__SSE_NO_FIRST_TOKEN__", "no_first_token"), ("__SSE_IDLE_AFTER_TOKEN__", "idle_after_token"),
                                 ("__SSE_CANCEL_STREAM__", "cancel_stream"),
                                 ("__SSE_CANCEL_BEFORE_HEADERS__", "cancel_before_headers"),
                                 ("__SSE_RESET_AFTER_PARTIAL__", "reset_after_partial")):
                if marker in combined_text:
                    if mode == "reset_after_partial":
                        with type(self).fault_lock:
                            if type(self).reset_after_partial_remaining > 0:
                                type(self).reset_after_partial_remaining -= 1
                                should_fault = True
                            else:
                                should_fault = False
                        if not should_fault:
                            continue
                    self._send_fault_stream(mode)
                    return
        system_text = next(((m.get("content") or "") for m in messages if m.get("role") == "system"), "")
        available = [x.get("function", {}).get("name") for x in request.get("tools", [])]
        tool_messages = [m for m in messages if m.get("role") == "tool"]
        context_seen = any(m.get("role") == "assistant" and '"selected_agent"' in (m.get("content") or "") for m in messages)
        if "decision model routing" in system_text:
            message = {"role": "assistant", "content": json.dumps({"agent_id": "generalist", "rationale": "Use the configured flexible agent for this request."})}
        elif "clarification stage of a task decomposition loop" in system_text:
            user_text = next((m.get("content", "") for m in reversed(messages) if m.get("role") == "user"), "")
            multi_question = "NEEDS_MULTIPLE_CLARIFICATIONS" in user_text and "User clarifications:" not in user_text
            needs_question = ("NEEDS_CLARIFICATION" in user_text and "User clarifications:" not in user_text) or multi_question
            marker_present = "NEEDS_CLARIFICATION" in user_text or "NEEDS_MULTIPLE_CLARIFICATIONS" in user_text
            questions = ([{"question": "Where should this be deployed?", "options": [
                              {"id": "local", "label": "Locally on this device"}, {"id": "lan", "label": "On a node in the local network"}, {"id": "remote", "label": "On a remote server"}]},
                          {"question": "Which verification level is required?", "options": [
                              {"id": "unit", "label": "Unit tests only"}, {"id": "full", "label": "Full integration tests"}]}] if multi_question else
                         ([{"question": "Where should this be deployed?", "options": [
                              {"id": "local", "label": "Locally on this device"}, {"id": "lan", "label": "On a node in the local network"}, {"id": "remote", "label": "On a remote server"}]}] if needs_question else []))
            structured = {"goal": "Create a tested implementation for the supplied task",
                          "known_information": ["Task text is available", "Execution is asynchronous"],
                          "questions": questions,
                          "assumptions": ["Use repository-local conventions"],
                          "constraints": ["Do not claim external deployment"], "clarification_needed": marker_present}
            message = {"role": "assistant", "content": json.dumps(structured, ensure_ascii=False)}
        elif "stage 2 of a task decomposition loop" in system_text:
            structured = {"steps": [
                {"title": "Inspect inputs and constraints", "instruction": "Review the task, inputs, and repository constraints before editing.", "acceptance_criteria": "Requirements and assumptions are explicit."},
                {"title": "Implement the requested change", "instruction": "Implement the central requested behavior using the configured agent and allow-listed tools.", "acceptance_criteria": "The requested behavior is represented in the deliverable."},
                {"title": "Run verification and report", "instruction": "Run relevant tests, correct defects, and describe remaining limitations.", "acceptance_criteria": "Tests have a recorded outcome and limitations are disclosed."}]}
            message = {"role": "assistant", "content": json.dumps(structured, ensure_ascii=False)}
        elif "independent quality reviewer" in system_text:
            structured = {"steps": [
                {"title": "Inspect inputs and constraints", "instruction": "Review the task, inputs, and repository constraints before editing.", "acceptance_criteria": "Requirements and assumptions are explicit."},
                {"title": "Implement the requested change", "instruction": "Implement the central requested behavior using the configured agent and allow-listed tools.", "acceptance_criteria": "The requested behavior is represented in the deliverable."},
                {"title": "Run verification and report", "instruction": "Run relevant tests, correct defects, and describe remaining limitations.", "acceptance_criteria": "Tests have a recorded outcome and limitations are disclosed."}],
                "review": {"score": 8, "summary": "Coverage and verification criteria are explicit.", "issues": [], "changes": ["Added a concrete verification gate"]}}
            message = {"role": "assistant", "content": json.dumps(structured, ensure_ascii=False)}
        elif "MCP_PROTOCOL_TOOL_TEST" in "\n".join(m.get("content", "") for m in messages if isinstance(m.get("content"), str)):
            called = [tc.get("function", {}).get("name") for m in messages if m.get("role") == "assistant"
                      for tc in m.get("tool_calls", [])]
            sequence = [
                ("mcp__testserver__resources_list", "{}"),
                ("mcp__testserver__resource_read", "{\"uri\":\"file:///mock/usage.md\"}"),
                ("mcp__testserver__prompts_list", "{}"),
                ("mcp__testserver__prompt_get", "{\"name\":\"summarize\",\"arguments\":{\"topic\":\"protocol\"}}"),
            ]
            next_call = next(((name, args) for name, args in sequence if name in available and name not in called), None)
            if next_call:
                name, args = next_call
                message = {"role": "assistant", "tool_calls": [{"id": "call-mcp-protocol-" + str(len(called)), "type": "function",
                            "function": {"name": name, "arguments": args}}]}
            else:
                message = {"role": "assistant", "content": "MCP resource and prompt methods verified: " + " | ".join(m.get("content", "") for m in tool_messages)}
        elif "MCP_CANCEL_STDIO_TEST" in "\n".join(m.get("content", "") for m in messages if isinstance(m.get("content"), str)):
            called = [tc.get("function", {}).get("name") for m in messages if m.get("role") == "assistant"
                      for tc in m.get("tool_calls", [])]
            if "mcp__testserver__echo" in available and "mcp__testserver__echo" not in called:
                message = {"role": "assistant", "tool_calls": [{"id": "call-mcp-cancel-stdio", "type": "function",
                            "function": {"name": "mcp__testserver__echo", "arguments": "{\"text\":\"__MCP_CANCEL__\"}"}}]}
            else:
                message = {"role": "assistant", "content": "MCP stdio cancellation test finished"}
        elif "MCP_CANCEL_HTTP_TEST" in "\n".join(m.get("content", "") for m in messages if isinstance(m.get("content"), str)):
            called = [tc.get("function", {}).get("name") for m in messages if m.get("role") == "assistant"
                      for tc in m.get("tool_calls", [])]
            if "mcp__httpserver__echo" in available and "mcp__httpserver__echo" not in called:
                message = {"role": "assistant", "tool_calls": [{"id": "call-mcp-cancel-http", "type": "function",
                            "function": {"name": "mcp__httpserver__echo",
                                         "arguments": "{\"text\":\"__MCP_CANCEL__\",\"region\":\"us-west1\"}"}}]}
            else:
                message = {"role": "assistant", "content": "MCP HTTP cancellation test finished"}
        elif "MCP_HTTP_TEST" in "\n".join(m.get("content", "") for m in messages if isinstance(m.get("content"), str)):
            called = [tc.get("function", {}).get("name") for m in messages if m.get("role") == "assistant"
                      for tc in m.get("tool_calls", [])]
            if "mcp__httpserver__echo" in available and "mcp__httpserver__echo" not in called:
                message = {"role": "assistant", "tool_calls": [{"id": "call-mcp-http-1", "type": "function",
                            "function": {"name": "mcp__httpserver__echo",
                                         "arguments": "{\"text\":\"hello-from-agent\",\"region\":\"us-west1\"}"}}]}
            else:
                message = {"role": "assistant", "content": "MCP HTTP tool verified: " + " | ".join(m.get("content", "") for m in tool_messages)}
        elif any(name.startswith("mcp__") and name.endswith("__echo") for name in available) and not tool_messages:
            echo_tool = next(name for name in available if name.startswith("mcp__") and name.endswith("__echo"))
            # The HTTP mock's schema binds `region` to the Mcp-Param-Region
            # header. Include the declared argument when that transport is
            # selected; stdio echo tools continue to accept only `text`.
            echo_arguments = {"text": "hello-from-agent"}
            if echo_tool == "mcp__httpserver__echo":
                echo_arguments["region"] = "us-west1"
            message = {"role": "assistant", "tool_calls": [{"id": "call-mcp-1", "type": "function",
                        "function": {"name": echo_tool, "arguments": json.dumps(echo_arguments)}}]}
        elif tool_messages:
            message = {"role": "assistant", "content": "Mock LLM received MCP reply: " + tool_messages[-1].get("content", "") + f"; prior_context={str(context_seen).lower()}"}
        else:
            message = {"role": "assistant", "content": "Mock model stage output; prior_context=" + str(context_seen).lower()}
        response = {"id": "chatcmpl-mock", "object": "chat.completion", "choices": [{"index": 0, "message": message, "finish_reason": "tool_calls" if "tool_calls" in message else "stop"}]}
        if request.get("stream") is True:
            if "__SSE_REASONING_FIRST_TOKEN__" in combined_text:
                with type(self).fault_lock:
                    if type(self).reasoning_first_token_remaining > 0:
                        type(self).reasoning_first_token_remaining -= 1
                        response["_test_reasoning_first_token"] = True
            self._send_stream(response)
            return
        body = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


class MockMCPHTTPHandler(BaseHTTPRequestHandler):
    """Stateless MCP Streamable HTTP peer; exercises JSON, SSE and x-mcp-header."""
    protocol_version = "HTTP/1.1"
    lock = threading.Lock()
    methods = []
    sse_count = 0
    tool_count = 0
    errors = []
    cancel_tool_started = threading.Event()

    @classmethod
    def reset(cls):
        with cls.lock:
            cls.methods = []
            cls.sse_count = 0
            cls.tool_count = 0
            cls.errors = []
            cls.cancel_tool_started.clear()

    def do_POST(self):
        try:
            raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            request = json.loads(raw.decode("utf-8"))
            method = request.get("method")
            params = request.get("params", {})
            meta = params.get("_meta", {}) if isinstance(params, dict) else {}
            errors = []
            if self.path != "/mcp": errors.append("wrong endpoint path")
            if self.headers.get("MCP-Protocol-Version") != "2026-07-28": errors.append("missing MCP-Protocol-Version")
            if self.headers.get("Mcp-Method") != method: errors.append("Mcp-Method mismatch")
            if meta.get("io.modelcontextprotocol/protocolVersion") != "2026-07-28": errors.append("missing protocol metadata")
            if self.headers.get("Authorization") != "Bearer test-token": errors.append("bearer token missing")
            if method == "tools/call":
                if self.headers.get("Mcp-Name") != "echo": errors.append("Mcp-Name mismatch")
                if self.headers.get("Mcp-Param-Region") != "us-west1": errors.append("Mcp-Param-Region mismatch")
                args = params.get("arguments", {})
                if "region" in args: errors.append("x-mcp-header value leaked into JSON body")
                if args.get("text") not in ("hello-from-agent", "__MCP_CANCEL__"): errors.append("tool args mismatch")
            with self.lock:
                self.methods.append(method)
                if errors: self.errors.extend(errors)
                if method == "tools/call": type(self).tool_count += 1
            if errors:
                body = json.dumps({"jsonrpc": "2.0", "id": request.get("id"),
                                   "error": {"code": -32602, "message": "; ".join(errors)}}).encode()
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers(); self.wfile.write(body); return
            if method == "server/discover":
                result = {"resultType": "complete", "supportedVersions": ["2026-07-28"],
                          "capabilities": {"tools": {}, "resources": {}, "prompts": {}, "completions": {}}}
                self._send_json(request.get("id"), result); return
            if method == "tools/list":
                result = {"resultType": "complete", "tools": [{
                    "name": "echo", "description": "Echo a text field; region is forwarded via x-mcp-header",
                    "inputSchema": {"type": "object", "properties": {
                        "text": {"type": "string"}, "region": {"type": "string", "x-mcp-header": "Region"}},
                        "required": ["text", "region"], "additionalProperties": False}}]}
                payload = json.dumps({"jsonrpc": "2.0", "id": request.get("id"), "result": result}, separators=(",", ":"))
                # Include a progress SSE event before the request-scoped response event.
                sse = ("event: message\ndata: " + json.dumps({"jsonrpc": "2.0", "method": "notifications/progress",
                        "params": {"progress": 1, "total": 1}}) + "\n\n" +
                       "event: message\ndata: " + payload + "\n\n").encode()
                with type(self).lock: type(self).sse_count += 1
                self.send_response(200); self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache"); self.send_header("Connection", "close")
                self.close_connection = True
                self.end_headers(); self.wfile.write(sse); self.wfile.flush()
                # Emulate a valid long-lived SSE response: the client should stop
                # reading after the matching JSON-RPC ID rather than waiting for EOF.
                time.sleep(3.0)
                return
            if method == "tools/call":
                text = params.get("arguments", {}).get("text", "")
                result = {"resultType": "complete", "content": [{"type": "text", "text": "HTTP_MCP_EXECUTED:" + text}], "isError": False}
                if text == "__MCP_CANCEL__":
                    type(self).cancel_tool_started.set()
                    time.sleep(6.0)
                    try:
                        self._send_json(request.get("id"), result)
                    except (BrokenPipeError, ConnectionResetError, OSError):
                        pass  # Expected: the client aborted after task cancellation.
                    return
                self._send_json(request.get("id"), result); return
            self.send_response(405); self.send_header("Content-Length", "0"); self.end_headers()
        except Exception as exc:
            with self.lock: self.errors.append("handler exception: " + repr(exc))
            try:
                body = json.dumps({"error": str(exc)}).encode()
                self.send_response(500); self.send_header("Content-Length", str(len(body)))
                self.end_headers(); self.wfile.write(body)
            except Exception: pass

    def _send_json(self, request_id, result):
        body = json.dumps({"jsonrpc": "2.0", "id": request_id, "result": result}, separators=(",", ":")).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


class StrictSystemOneHandler(BaseHTTPRequestHandler):
    """A protocol mock that accepts only POST /v1/systemone and native typed questions."""
    modes = []
    observed = []
    observed_modes = []
    slow_cancel_classifier = False
    lock = threading.Lock()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            body = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            body = None
        with self.lock:
            if self.slow_cancel_classifier:
                mode = "slow_cancel"
                type(self).slow_cancel_classifier = False
            else:
                mode = self.modes.pop(0) if self.modes else "valid"
            self.observed.append((self.path, body))
            self.observed_modes.append(mode)
        if self.path != "/v1/systemone":
            self._reply(404, b'{"error":"strict mock only accepts /v1/systemone"}')
            return
        allowed_fields = {"model", "state", "questions"}
        if (not isinstance(body, dict) or not {"state", "questions"}.issubset(body) or
                not set(body).issubset(allowed_fields) or not isinstance(body.get("questions"), dict) or
                ("model" in body and (not isinstance(body["model"], str) or not body["model"]))):
            self._reply(400, b'{"error":"expected System One state/questions and optional model contract"}')
            return
        if not 1 <= len(body["questions"]) <= 32:
            self._reply(400, b'{"error":"question count must be 1..32"}')
            return
        if mode == "bad_contract":
            self._reply(200, b'{"model":"d1-3b","answers":[]}')
            return
        if mode == "invalid_json":
            self._reply(200, b'{"answers":')
            return
        if mode in ("http_404", "http_500"):
            status = 404 if mode == "http_404" else 500
            self._reply(status, json.dumps({"error": f"mock failure {status}"}).encode())
            return
        if mode == "timeout":
            time.sleep(1.6)
        if mode == "slow_cancel":
            # Long enough to prove cancellation aborts transport rather than merely
            # waiting for the configured HTTP timeout to elapse.
            time.sleep(6.0)
        questions = body["questions"]
        answers = {}
        for key, question in questions.items():
            if not isinstance(question, dict):
                self._reply(400, b'{"error":"question must be an object"}')
                return
            qtype = question.get("type")
            if qtype == "choice":
                criteria = question.get("criteria")
                if not isinstance(criteria, dict) or len(criteria) < 2:
                    self._reply(400, b'{"error":"choice requires at least two criteria"}')
                    return
                choice = next(iter(criteria))
                answers[key] = {"type": "choice", "choice": choice,
                                "probabilities": {name: (0.9 if name == choice else 0.1 / max(1, len(criteria) - 1)) for name in criteria},
                                "confidence": 0.9}
            elif qtype == "noul":
                answers[key] = {"type": "noul", "noul": 0.8}
            else:
                self._reply(400, b'{"error":"unsupported typed question"}')
                return
        self._reply(200, json.dumps({"model": body["model"], "answers": answers, "usage": {"input_tokens": 10, "output_tokens": 0}}).encode())

    def _reply(self, status, payload):
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, fmt, *args):
        pass


def run_systemone_case(binary, project, *, via_lan=False, error_mode=None, cancel_during_request=False):
    server = ThreadingHTTPServer(("127.0.0.1", 0), StrictSystemOneHandler)
    server.daemon_threads = True
    api_port = server.server_address[1]
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    StrictSystemOneHandler.observed = []
    StrictSystemOneHandler.observed_modes = []
    StrictSystemOneHandler.modes = [error_mode] if error_mode else (["valid"] if cancel_during_request else [])
    StrictSystemOneHandler.slow_cancel_classifier = cancel_during_request
    try:
        with tempfile.TemporaryDirectory(prefix="ao-systemone-") as tmp:
            root = Path(tmp)
            agents_dir = root / "agents"
            shutil.copytree(project / "config" / "agents", agents_dir)
            generalist = json.loads((agents_dir / "generalist.json").read_text(encoding="utf-8"))
            for agent_id in ("agent-alpha", "agent-beta"):
                profile = dict(generalist, id=agent_id, name=f"Agent {agent_id}",
                               description=f"Flexible agent profile {agent_id} for System One contract checks.", model_id="")
                (agents_dir / f"{agent_id}.json").write_text(json.dumps(profile), encoding="utf-8")
            endpoint = f"http://127.0.0.1:{api_port}/v1"
            nodes = []
            model_url = endpoint
            decision = {
                "id": "strict-systemone", "name": "Strict System One mock", "enabled": True,
                "auto_start": False, "executable": "", "model_path": "", "working_directory": tmp,
                "args": [], "port": 0, "base_url": endpoint, "api_type": "systemone",
                "health_url": f"http://127.0.0.1:{api_port}/health", "model_name": "d1-3b", "api_key": "",
                "startup_timeout_seconds": 2, "shutdown_timeout_seconds": 1,
                "request_timeout_seconds": 8 if cancel_during_request else (1 if error_mode == "timeout" else 3), "temperature": 0.1,
                "ram_estimate_mb": 0, "vram_estimate_mb": 0,
            }
            if via_lan:
                model_url = f"http://127.0.0.1:{api_port}/v1/systemone"
                decision["node_id"] = "strict-lan"
                nodes = [{"id": "strict-lan", "name": "Strict LAN test endpoint", "type": "lan", "enabled": True,
                          "endpoint": model_url, "health_url": f"http://127.0.0.1:{api_port}/health",
                          "api_type": "systemone", "api_key": "", "api_key_env": ""}]
            registry = {"defaults": {"model_id": "", "analysis_model_id": "", "decision_model_id": "strict-systemone"},
                        "models": [], "decision_models": [decision], "nodes": nodes}
            proc = start_server(free_port(), root / "tasks.sqlite3", agents_dir=agents_dir, model_registry=registry)
            try:
                port = int(json.loads((root / "config.json").read_text(encoding="utf-8"))["server"]["port"])
                accepted = rpc(port, {"op": "submit", "task": "Check System One typed routing and task decision"})
                assert accepted["ok"], accepted
                if cancel_during_request:
                    wait_for(lambda: True if "slow_cancel" in StrictSystemOneHandler.observed_modes else None,
                             timeout=12, label="active System One decision request starts before cancellation")
                    cancel_at = time.monotonic()
                    cancelled_reply = rpc(port, {"op": "cancel", "task_id": accepted["task_id"]})
                    assert cancelled_reply.get("ok"), cancelled_reply
                    final = wait_for(lambda: (lambda current: current if current.get("status") == "cancelled" else None)(
                                      rpc(port, {"op": "status", "task_id": accepted["task_id"]})),
                                     timeout=4, label="System One transport aborts after cancellation")
                    assert time.monotonic() - cancel_at < 4.0, final
                else:
                    final = wait_for(lambda: (lambda current: current if current.get("status") in
                                      ("completed", "completed_without_model", "failed", "cancelled") else None)(
                                      rpc(port, {"op": "status", "task_id": accepted["task_id"]})),
                                     timeout=18, label="System One strict contract task")
                    allowed_final = ("completed", "completed_without_model", "failed") if error_mode else ("completed", "completed_without_model")
                    assert final["status"] in allowed_final, final
                stderr = stop_server(proc)
            finally:
                if proc.poll() is None:
                    stop_server(proc)
            observed = list(StrictSystemOneHandler.observed)
            assert observed, "the configured System One endpoint was never called"
            assert all(path == "/v1/systemone" for path, _ in observed), [path for path, _ in observed]
            assert all(isinstance(body, dict) and {"state", "questions"}.issubset(body) and
                   set(body).issubset({"model", "state", "questions"}) for _, body in observed), observed
            assert all(1 <= len(body["questions"]) <= 32 for _, body in observed), observed
            if cancel_during_request:
                slow_rows = [body for (_, body), mode in zip(observed, StrictSystemOneHandler.observed_modes) if mode == "slow_cancel"]
                assert slow_rows and isinstance(slow_rows[0].get("state"), dict) and \
                    isinstance(slow_rows[0].get("questions"), dict), observed
            if error_mode:
                expected_errors = {
                    "bad_contract": "missing the answers object",
                    "invalid_json": "invalid JSON object",
                    "http_404": "System One HTTP 404",
                    "http_500": "System One HTTP 500",
                    "timeout": "Timeout was reached",
                }
                expected_error = expected_errors[error_mode]
                assert expected_error in stderr, f"expected {expected_error!r}; stderr={stderr}; modes={StrictSystemOneHandler.observed_modes!r}"
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=2)


def run_openai_sse_error_privacy_case(binary, project):
    """Verify upstream error bodies are not echoed into events, DB-facing status or logs."""
    marker_secret = "UPSTREAM_SECRET_MARKER_DO_NOT_PERSIST"
    with tempfile.TemporaryDirectory(prefix="ao-c-provider-error-privacy-") as tmp:
        root = Path(tmp)
        llm_port = free_port()
        handler = ThreadingHTTPServer(("127.0.0.1", llm_port), MockLLMHandler)
        handler.daemon_threads = True
        server_thread = threading.Thread(target=handler.serve_forever, daemon=True)
        server_thread.start()
        proc = None
        try:
            proc = start_server(free_port(), root / "privacy.sqlite3", llm_enabled=True,
                                llm_base_url=f"http://127.0.0.1:{llm_port}/v1", llm_timeout_seconds=3)
            port = int(json.loads((root / "config.json").read_text(encoding="utf-8"))["server"]["port"])
            task = rpc(port, {"op": "submit", "task": "MCP_PROTOCOL_TOOL_TEST: check error redaction __SSE_HTTP_500_SECRET_BODY__"})
            assert task.get("ok"), task
            terminal = wait_for(lambda: (lambda current: current if current.get("status") in
                                         ("completed", "completed_without_model", "failed", "cancelled") else None)(
                                         rpc(port, {"op": "status", "task_id": task["task_id"]})),
                                timeout=12, label="provider error privacy task")
            events = rpc(port, {"op": "events", "task_id": task["task_id"], "limit": 500}).get("events", [])
            status_events = [e for e in events if e.get("type") == "llm_status"]
            assert any("LLM HTTP 500" in str(e.get("message", "")) for e in status_events), status_events
            serialized = json.dumps({"terminal": terminal, "events": events}, ensure_ascii=False)
            assert marker_secret not in serialized, "upstream error body leaked into persisted task status/events"
        finally:
            stderr = ""
            if proc is not None:
                if proc.poll() is None:
                    stderr = stop_server(proc)
                else:
                    try:
                        stderr = proc.communicate(timeout=2)[1] or ""
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        stderr = proc.communicate(timeout=2)[1] or ""
            handler.shutdown()
            handler.server_close()
            server_thread.join(timeout=2)
            assert marker_secret not in stderr, "upstream error body leaked into core stderr logs"


def run_openai_sse_fault_cases(binary, project):
    print("[integration] OpenAI-compatible SSE HTTP/parse/timeouts/cancellation", flush=True)
    with tempfile.TemporaryDirectory(prefix="ao-c-sse-faults-") as tmp:
        root = Path(tmp)
        llm_port = free_port()
        handler = ThreadingHTTPServer(("127.0.0.1", llm_port), MockLLMHandler)
        handler.daemon_threads = True
        server_thread = threading.Thread(target=handler.serve_forever, daemon=True)
        server_thread.start()
        proc = None
        try:
            proc = start_server(free_port(), root / "sse.sqlite3", llm_enabled=True,
                                llm_base_url=f"http://127.0.0.1:{llm_port}/v1", llm_timeout_seconds=1)
            port = int(json.loads((root / "config.json").read_text(encoding="utf-8"))["server"]["port"])
            cases = [
                ("__SSE_HTTP_404__", "HTTP-error", "LLM HTTP 404"),
                ("__SSE_HTTP_500__", "HTTP-error", "LLM HTTP 500"),
                ("__SSE_MALFORMED_JSON__", "HTTP-error", "malformed JSON"),
                ("__SSE_MISSING_DONE__", "HTTP-error", "ended before [DONE]"),
                ("__SSE_NO_FIRST_TOKEN__", "HTTP-error", "first-token timeout"),
                ("__SSE_IDLE_AFTER_TOKEN__", "HTTP-error", "idle timeout"),
            ]
            for marker, expected_status, expected_message in cases:
                print(f"[integration] SSE fault case {marker}", flush=True)
                task = rpc(port, {"op": "submit", "task": f"MCP_PROTOCOL_TOOL_TEST: verify transport handling {marker}"})
                assert task.get("ok"), task
                terminal = wait_for(lambda tid=task["task_id"]: (lambda current: current if current.get("status") in
                         ("completed", "completed_without_model", "failed", "cancelled") else None)(
                         rpc(port, {"op": "status", "task_id": tid})), timeout=15, label=f"SSE fault {marker}")
                events = rpc(port, {"op": "events", "task_id": task["task_id"], "limit": 500})["events"]
                statuses = [event for event in events if event.get("type") == "llm_status"]
                assert any(event.get("payload", {}).get("status") == expected_status and
                           expected_message.lower() in str(event.get("message", "")).lower()
                           for event in statuses), (marker, [(event.get("payload", {}).get("status"), event.get("message")) for event in statuses])
                print(f"[integration] SSE fault case complete {marker}: {terminal.get('status')}", flush=True)
                if terminal.get("status") == "failed" and marker in ("__SSE_NO_FIRST_TOKEN__", "__SSE_IDLE_AFTER_TOKEN__"):
                    diagnostics = [event for event in events if event.get("type") == "timeout_diagnostic"]
                    assert diagnostics, (marker, "failed task missing automatic timeout diagnostic", events[-30:])
                    detail = diagnostics[-1].get("payload", {})
                    assert detail.get("category") in ("first_token_timeout", "stream_idle_timeout", "timeout"), detail
                    assert isinstance(detail.get("log_event_count"), int), detail
                    assert detail.get("logs_sent") is False or isinstance(detail.get("source"), str), detail

            print("[integration] SSE reasoning-first-token case", flush=True)
            MockLLMHandler.reasoning_first_token_remaining = 1
            reasoning_task = rpc(port, {"op": "submit", "task": "Small test with streaming reasoning before the answer __SSE_REASONING_FIRST_TOKEN__"})
            assert reasoning_task.get("ok"), reasoning_task
            reasoning_final = wait_for(lambda: (lambda current: current if current.get("status") in
                ("completed", "completed_without_model", "failed", "cancelled") else None)(
                rpc(port, {"op": "status", "task_id": reasoning_task["task_id"]})),
                timeout=18, label="reasoning tokens count as first-token activity")
            reasoning_events = rpc(port, {"op": "events", "task_id": reasoning_task["task_id"], "limit": 700})["events"]
            thinking_rows = [row for row in reasoning_events if row.get("type") == "llm_thinking_delta"]
            assert any(isinstance(row.get("payload", {}).get("delta_bytes"), int) and row["payload"]["delta_bytes"] > 0
                       for row in thinking_rows), thinking_rows
            assert any(isinstance(row.get("payload", {}).get("delta"), str) and row["payload"]["delta"]
                       for row in thinking_rows), thinking_rows
            assert all(row.get("payload", {}).get("model_name") and row.get("payload", {}).get("stream_id")
                       for row in thinking_rows), thinking_rows
            assert not any(row.get("type") == "llm_status" and row.get("payload", {}).get("status") == "HTTP-error" and "first-token timeout" in str(row.get("message", "")).lower() for row in reasoning_events), reasoning_events
            assert reasoning_final["status"] == "completed", reasoning_final

            print("[integration] SSE stream cancellation case", flush=True)
            task = rpc(port, {"op": "submit", "task": "MCP_PROTOCOL_TOOL_TEST: verify streaming cancellation __SSE_CANCEL_STREAM__"})
            assert task.get("ok"), task
            def has_started_stream():
                rows = rpc(port, {"op": "events", "task_id": task["task_id"], "limit": 500})["events"]
                return rows if any(row.get("type") == "llm_status" and row.get("payload", {}).get("status") == "streaming" for row in rows) else None
            rows = wait_for(has_started_stream, timeout=8, label="SSE stream starts before cancellation")
            cancel_at = time.monotonic()
            cancelled = rpc(port, {"op": "cancel", "task_id": task["task_id"]})
            assert cancelled.get("ok"), cancelled
            final = wait_for(lambda: (lambda current: current if current.get("status") == "cancelled" else None)(
                             rpc(port, {"op": "status", "task_id": task["task_id"]})), timeout=4,
                             label="LLM stream stops after cancellation")
            assert time.monotonic() - cancel_at < 4.0, final
            rows = rpc(port, {"op": "events", "task_id": task["task_id"], "limit": 500})["events"]
            assert any(row.get("type") == "llm_status" and row.get("payload", {}).get("status") == "cancelled"
                       for row in rows), rows

            # A single connection reset after partial content must be recoverable;
            # the mock fails once, then allows the orchestration retry to complete.
            print("[integration] SSE partial reset recovery case", flush=True)
            MockLLMHandler.reset_after_partial_remaining = 1
            reset_task = rpc(port, {"op": "submit", "task": "MCP_PROTOCOL_TOOL_TEST: recover after one transient stream reset __SSE_RESET_AFTER_PARTIAL__"})
            assert reset_task.get("ok"), reset_task
            reset_final = wait_for(lambda: (lambda current: current if current.get("status") in
                                     ("completed", "completed_without_model", "failed", "cancelled") else None)(
                                     rpc(port, {"op": "status", "task_id": reset_task["task_id"]})),
                                     timeout=25, label="one-shot partial SSE reset recovery")
            reset_events = rpc(port, {"op": "events", "task_id": reset_task["task_id"], "limit": 800})["events"]
            reset_statuses = [row for row in reset_events if row.get("type") == "llm_status"]
            assert any(row.get("payload", {}).get("status") == "HTTP-error" for row in reset_statuses), reset_statuses
            assert reset_final["status"] == "completed", reset_final
            assert any(row.get("type") == "assistant_delta" for row in reset_events), reset_events

            # Cancellation must abort a request that has not received response headers.
            print("[integration] SSE cancellation before headers case", flush=True)
            preheader_task = rpc(port, {"op": "submit", "task": "MCP_PROTOCOL_TOOL_TEST: cancel while waiting for response headers __SSE_CANCEL_BEFORE_HEADERS__"})
            assert preheader_task.get("ok"), preheader_task
            wait_for(lambda: (lambda rows: rows if any(row.get("type") == "llm_status" and
                     row.get("payload", {}).get("status") in ("connecting", "waiting-first-token") for row in rows) else None)(
                     rpc(port, {"op": "events", "task_id": preheader_task["task_id"], "limit": 500})["events"]),
                     timeout=8, label="LLM request begins before headers")
            preheader_cancel_time = time.monotonic()
            assert rpc(port, {"op": "cancel", "task_id": preheader_task["task_id"]}).get("ok")
            preheader_final = wait_for(lambda: (lambda current: current if current.get("status") == "cancelled" else None)(
                                       rpc(port, {"op": "status", "task_id": preheader_task["task_id"]})),
                                       timeout=4, label="cancel while waiting for HTTP headers")
            assert time.monotonic() - preheader_cancel_time < 4.0, preheader_final
            preheader_events = rpc(port, {"op": "events", "task_id": preheader_task["task_id"], "limit": 500})["events"]
            assert any(row.get("type") == "llm_status" and row.get("payload", {}).get("status") == "cancelled"
                       for row in preheader_events), preheader_events
        finally:
            if proc is not None and proc.poll() is None:
                stop_server(proc)
            handler.shutdown()
            handler.server_close()
            server_thread.join(timeout=2)


def main():
    print("[integration] config validation", flush=True)
    checked = subprocess.run([str(BINARY), "--check-config", "-c", str(PROJECT / "config" / "config.json")],
                            text=True, capture_output=True, check=False)
    assert checked.returncode == 0, checked.stderr
    active_agents = [line[2:].split(" (", 1)[0] for line in checked.stdout.splitlines() if line.startswith("- ")]
    assert active_agents == ["generalist"], f"unexpected active agent IDs: {active_agents!r}; stdout={checked.stdout!r}"

    run_openai_sse_fault_cases(BINARY, PROJECT)

    print("[integration] legacy SQLite schema migration and title fallback", flush=True)
    with tempfile.TemporaryDirectory(prefix="ao-c-legacy-db-") as tmp:
        root = Path(tmp)
        legacy_db = root / "legacy.sqlite3"
        with sqlite3.connect(legacy_db) as con:
            # Schema from a prior release: no priority/workspace/title and no waiting_input status.
            con.execute("""CREATE TABLE tasks (
                id TEXT PRIMARY KEY, idempotency_key TEXT UNIQUE, task_json TEXT NOT NULL,
                session_id TEXT, status TEXT NOT NULL CHECK(status IN ('queued','running','completed','failed','cancelled')),
                attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL,
                cancel_requested INTEGER NOT NULL DEFAULT 0, result_json TEXT, error TEXT,
                created_at TEXT NOT NULL, updated_at TEXT NOT NULL)""")
            con.execute("""INSERT INTO tasks(id,idempotency_key,task_json,session_id,status,attempts,max_attempts,
                cancel_requested,result_json,error,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)""",
                ("legacyold001", None, json.dumps({"task":"Legacy migration keeps the original task"}), None,
                 "completed", 1, 3, 0, json.dumps({"ok":True}), None,
                 "2026-01-01T00:00:00Z", "2026-01-01T00:00:01Z"))
        port = free_port()
        proc = start_server(port, legacy_db)
        try:
            legacy_status = rpc(port, {"op":"status", "task_id":"legacyold001"})
            assert legacy_status.get("ok") is True, legacy_status
            assert legacy_status.get("title") == "Legacy migration keeps the original task", legacy_status
            task_list = rpc(port, {"op":"list_tasks"})
            assert task_list.get("ok") is True, task_list
            rows = task_list.get("tasks", task_list.get("result", []))
            assert any(row.get("task_id", row.get("id")) == "legacyold001" and
                       row.get("title") == "Legacy migration keeps the original task" for row in rows), task_list
            stop_server(proc)
            proc = None
        finally:
            if proc is not None and proc.poll() is None:
                stop_server(proc)
        with sqlite3.connect(legacy_db) as con:
            columns = {row[1] for row in con.execute("PRAGMA table_info(tasks)")}
            assert {"priority", "workspace_path", "title", "next_attempt_at"}.issubset(columns), columns
            sql = con.execute("SELECT sql FROM sqlite_master WHERE type='table' AND name='tasks'").fetchone()[0]
            assert "waiting_input" in sql, sql
            preserved = con.execute("SELECT task_json,status,attempts FROM tasks WHERE id='legacyold001'").fetchone()
            assert preserved and json.loads(preserved[0])["task"] == "Legacy migration keeps the original task" and preserved[1:] == ("completed", 1), preserved

    print("[integration] retry backoff, dead-letter inspection/redrive and recovery", flush=True)
    with tempfile.TemporaryDirectory(prefix="ao-c-retry-queue-") as tmp:
        root = Path(tmp)
        db_path = root / "retry-queue.sqlite3"
        # On startup a worker abandoned during attempt 2 gets a persisted 2s backoff.
        with sqlite3.connect(db_path) as con:
            con.execute("""CREATE TABLE tasks (
                id TEXT PRIMARY KEY, idempotency_key TEXT UNIQUE, task_json TEXT NOT NULL,
                session_id TEXT, workspace_path TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',
                status TEXT NOT NULL CHECK(status IN ('queued','running','waiting_input','completed','completed_without_model','failed','cancelled')),
                attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL,
                cancel_requested INTEGER NOT NULL DEFAULT 0, priority INTEGER NOT NULL DEFAULT 5,
                result_json TEXT, error TEXT, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
                updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')))""")
            con.execute("INSERT INTO tasks(id,task_json,status,attempts,max_attempts,priority) VALUES(?,?,?,?,?,?)",
                        ("recoverbackoff01", json.dumps({"op":"submit","task":"startup recovery with backoff"}), "running", 2, 4, 5))
            con.execute("INSERT INTO tasks(id,task_json,status,attempts,max_attempts,priority,error) VALUES(?,?,?,?,?,?,?)",
                        ("deadletter0001", json.dumps({"op":"submit","task":"manually rescued dead letter"}), "failed", 2, 2, 5, "test terminal failure"))
        port = free_port()
        proc = start_server(port, db_path)
        try:
            recovered = rpc(port, {"op":"status", "task_id":"recoverbackoff01"})
            assert recovered.get("status") == "queued", recovered
            assert recovered.get("next_attempt_at"), recovered
            dead_letters = rpc(port, {"op":"dead_letters", "limit":10})
            assert dead_letters.get("ok") and any(row.get("task_id") == "deadletter0001" and
                   row.get("error") == "test terminal failure" for row in dead_letters.get("dead_letters", [])), dead_letters
            redrive = rpc(port, {"op":"retry_task", "task_id":"deadletter"})
            assert redrive.get("ok") and redrive.get("status") == "queued", redrive
            redriven = wait_for(lambda: (lambda row: row if row.get("status") in
                      ("completed", "completed_without_model", "failed", "cancelled") else None)(
                      rpc(port, {"op":"status", "task_id":"deadletter0001"})), timeout=10,
                      label="dead-letter manual redrive")
            assert redriven.get("status") == "completed_without_model", redriven
            assert redriven.get("attempts") == 3 and redriven.get("max_attempts") == 3, redriven
            assert redriven.get("next_attempt_at") is None, redriven
            recovered_final = wait_for(lambda: (lambda row: row if row.get("status") in
                      ("completed", "completed_without_model", "failed", "cancelled") else None)(
                      rpc(port, {"op":"status", "task_id":"recoverbackoff01"})), timeout=10,
                      label="startup recovery retry after persisted backoff")
            assert recovered_final.get("status") == "completed_without_model", recovered_final
            assert recovered_final.get("attempts") == 3, recovered_final
            events = rpc(port, {"op":"events", "task_id":"deadletter0001", "limit":100})
            assert events.get("ok") and any(event.get("type") == "task_redriven" for event in events.get("events", [])), events
            assert rpc(port, {"op":"retry_task", "task_id":"deadletter0001"}).get("ok") is False
        finally:
            stop_server(proc)

    print("[integration] core smoke, C CLI menu, task lifecycle and recovery", flush=True)
    with tempfile.TemporaryDirectory(prefix="ao-c-test-") as tmp:
        tmpdir = Path(tmp)
        db_path = tmpdir / "tasks.sqlite3"
        # Simulate an upgraded user config that still contains obsolete fixed-role profiles.
        legacy_agents = tmpdir / "agents-with-legacy-roles"
        shutil.copytree(PROJECT / "config" / "agents", legacy_agents)
        generalist_profile = json.loads((legacy_agents / "generalist.json").read_text(encoding="utf-8"))
        for legacy_id in ("coder", "planner", "reviewer"):
            legacy_profile = dict(generalist_profile, id=legacy_id, name=legacy_id.title())
            (legacy_agents / f"{legacy_id}.json").write_text(json.dumps(legacy_profile), encoding="utf-8")
        port = free_port()
        proc = start_server(port, db_path, agents_dir=legacy_agents)
        try:
            cli_binary = BINARY.parent / "asgend-cli"
            cli_env = dict(os.environ, ASGEND_TOKEN="integration-secret", ASGEND_PORT=str(port))
            fake_editor = tmpdir / "asgend-test-editor.sh"
            fake_editor.write_text(
                "#!/bin/sh\n"
                "{ printf 'EDITED_FROM_TUI_INTEGRATION\\n'; cat \"$1\"; } > \"$1.out\" && "
                "cat \"$1.out\" > \"$1\" && rm -f \"$1.out\"\n",
                encoding="utf-8")
            fake_editor.chmod(0o700)
            cli_smoke = subprocess.run([str(cli_binary)], input="1\nq\n", text=True,
                                       capture_output=True, timeout=8, env=cli_env, check=False)
            assert cli_smoke.returncode == 0, (cli_smoke.returncode, cli_smoke.stdout, cli_smoke.stderr)
            assert "interactive task/chat mode" in cli_smoke.stdout and "Список задач" in cli_smoke.stdout, cli_smoke.stdout

            unauth = rpc(port, {"op": "ping"}, token=None)
            assert unauth["ok"] is False and "unauthorized" in unauth["error"]
            assert rpc(port, {"op": "ping"})["ok"] is True
            # Client-independent core endpoints are used by CLI, web UI and Flutter clients.
            models_reply = rpc(port, {"op": "models"})
            assert models_reply["ok"] and "model_status" in models_reply and "resources" in models_reply
            config_reply = rpc(port, {"op": "config_get"})
            assert config_reply["ok"] and "main" in config_reply and "models" in config_reply and "agents" in config_reply
            assert any(agent.get("id") == "generalist" for agent in config_reply["agents"])
            assert not any(agent.get("id") in {"coder", "planner", "reviewer"} for agent in config_reply["agents"]), config_reply["agents"]
            resource_reply = rpc(port, {"op": "resources"})
            assert resource_reply["ok"] and resource_reply["classification_parallel"] is False
            assert resource_reply["selection_mode"] == "local_bm25" and resource_reply["resource_index_auto_sync"] is True
            assert len(resource_reply["files"]["instructions"]) == 4, resource_reply.get("files")
            run_plan_crud_smoke(port)
            tui_todo_title = "TUI_SMOKE_TODO"
            tui_goal_title = "TUI_SMOKE_GOAL"
            tui_task = rpc(port, {"op": "submit", "task": "TUI workspace browser integration fixture"})
            assert tui_task.get("ok") and tui_task.get("task_id"), tui_task
            tui_task_id = tui_task["task_id"]
            tui_status = rpc(port, {"op": "status", "task_id": tui_task_id})
            assert tui_status.get("ok") and tui_status.get("workspace_path"), tui_status
            tui_local_workspace = Path(tui_status["workspace_path"])
            assert tui_local_workspace.is_dir() and tui_local_workspace.name == tui_task_id, tui_local_workspace
            seeded_todo = rpc(port, {"op": "plan_add", "scope_id": tui_task_id, "kind": "todo", "title": tui_todo_title})
            seeded_goal = rpc(port, {"op": "plan_add", "scope_id": tui_task_id, "kind": "goal", "title": tui_goal_title})
            assert seeded_todo.get("ok") and seeded_goal.get("ok"), (seeded_todo, seeded_goal)
            try:
                tui_filename = "tui-file-smoke.txt"
                (tui_local_workspace / tui_filename).write_text(
                    "created by TUI integration test\n", encoding="utf-8")
                launch_cwd = tmpdir / "tui-launch-directory"
                launch_cwd.mkdir(mode=0o700)
                (launch_cwd / "CLI_CWD_SHOULD_NOT_APPEAR.txt").write_text(
                    "not the task workspace\n", encoding="utf-8")
                nested_dir = tui_local_workspace / "nested"
                nested_dir.mkdir(mode=0o700)
                tui_nested_filename = "tui-nested-smoke.txt"
                tui_nested_content = "opened from nested workspace directory"
                preview_lines = [tui_nested_content] + [f"wheel-line-{index:02d}" for index in range(40)]
                (nested_dir / tui_nested_filename).write_text("\n".join(preview_lines) + "\n", encoding="utf-8")
                tui_drag_filename = "tui-drag-target.txt"
                tui_drag_content = "opened by dragging the mouse selection"
                (nested_dir / tui_drag_filename).write_text(tui_drag_content + "\n", encoding="utf-8")
                root_entries = [
                    {"name": "nested", "kind": "directory"},
                    {"name": tui_filename, "kind": "file"},
                ]
                nested_entries = sorted([tui_nested_filename, tui_drag_filename], key=str.casefold)
                nested_dir_row = next(index for index, entry in enumerate(root_entries)
                                      if entry["name"] == "nested" and entry["kind"] == "directory")
                nested_file_row = nested_entries.index(tui_nested_filename)
                drag_file_row = nested_entries.index(tui_drag_filename)
                assert nested_file_row != drag_file_row, nested_entries
                tui_env = dict(cli_env)
                run_cli_fullscreen_tui(cli_binary, tui_env, tui_task_id, tui_todo_title, tui_goal_title,
                                       tui_filename, tui_local_workspace, launch_cwd, nested_dir_row,
                                       tui_nested_filename, tui_nested_content, nested_file_row,
                                       tui_drag_filename, tui_drag_content, drag_file_row)
                run_cli_two_client_plan_sync(cli_binary, cli_env, port)

            finally:
                rpc(port, {"op": "plan_delete", "scope_id": tui_task_id, "item_id": seeded_todo["item_id"]})
                rpc(port, {"op": "plan_delete", "scope_id": tui_task_id, "item_id": seeded_goal["item_id"]})
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                sock.settimeout(3)
                sock.sendall(b'{"op":"ping","auth_token":"integration-secret"} junk\n')
                trailing_response = sock.recv(4096)
                if trailing_response:
                    parsed_trailing = json.loads(trailing_response.split(b"\n", 1)[0].decode())
                    assert parsed_trailing.get("ok") is not True, parsed_trailing
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                sock.settimeout(3)
                sock.sendall(b'{"op":"ping","auth_token":"integration-secret"}\x00junk\n')
                raw_nul_response = sock.recv(4096)
                if raw_nul_response:
                    try:
                        parsed_raw_nul = json.loads(raw_nul_response.split(b"\n", 1)[0].decode())
                    except (json.JSONDecodeError, UnicodeDecodeError):
                        parsed_raw_nul = {"ok": False}
                    assert parsed_raw_nul.get("ok") is not True, parsed_raw_nul

            agents = rpc(port, {"op": "list_agents"})
            assert agents["ok"] is True
            assert {a["id"] for a in agents["agents"]} >= {"generalist"}

            bad_agent = rpc(port, {"op": "submit", "task": "demo", "agent": "does-not-exist"})
            assert bad_agent["ok"] is False and "unknown agent" in bad_agent["error"]

            invalid_priority = rpc(port, {"op": "submit", "task": "invalid priority", "priority": 10})
            assert invalid_priority["ok"] is False and "priority" in invalid_priority["error"]
            submitted = rpc(port, {"op": "submit", "task": "Планирование и проверка устойчивой очереди", 
                                   "priority": 7, "session_id": "smoke", "idempotency_key": "same-request-1"})
            assert submitted["ok"] and submitted.get("accepted") and submitted.get("status") == "queued", submitted
            task_id = submitted["task_id"]
            assert len(task_id) == 10 and task_id.isalnum(), task_id
            assert submitted["priority"] == 7
            con = sqlite3.connect(db_path)
            stored_payload = con.execute("SELECT task_json,workspace_path FROM tasks WHERE id=?", (task_id,)).fetchone()
            con.close()
            assert "integration-secret" not in stored_payload[0], "transport auth token must not be stored in task payload"
            workspace_path = Path(stored_payload[1])
            assert workspace_path.is_dir() and workspace_path.name == task_id, workspace_path
            assert stat.S_IMODE(workspace_path.stat().st_mode) == 0o700, oct(stat.S_IMODE(workspace_path.stat().st_mode))
            # Workspace editor protocol: UTF-8 files can be listed/read/written, but traversal and symlinks fail closed.
            fallback_status = rpc(port, {"op": "status", "task_id": task_id})
            assert fallback_status.get("title"), fallback_status
            workspace_write = rpc(port, {"op": "workspace_write", "task_id": task_id,
                                         "path": "notes/hello.txt", "content": "Привет, ASGEND\n"})
            assert workspace_write["ok"] is False, workspace_write  # parent directories are not implicitly created
            (workspace_path / "notes").mkdir(mode=0o700)
            workspace_write = rpc(port, {"op": "workspace_write", "task_id": task_id,
                                         "path": "notes/hello.txt", "content": "Привет, ASGEND\n"})
            assert workspace_write["ok"] and workspace_write["size"] > 0, workspace_write
            workspace_read = rpc(port, {"op": "workspace_read", "task_id": task_id, "path": "notes/hello.txt"})
            assert workspace_read["ok"] and workspace_read["content"] == "Привет, ASGEND\n", workspace_read
            workspace_list = rpc(port, {"op": "workspace_list", "task_id": task_id, "path": "notes"})
            assert workspace_list["ok"] and any(x["name"] == "hello.txt" and x["kind"] == "file" for x in workspace_list["entries"]), workspace_list
            traversal = rpc(port, {"op": "workspace_read", "task_id": task_id, "path": "../outside.txt"})
            assert traversal["ok"] is False, traversal
            outside = workspace_path.parent / "outside.txt"
            outside.write_text("do not read", encoding="utf-8")
            (workspace_path / "escape.txt").symlink_to(outside)
            symlink_read = rpc(port, {"op": "workspace_read", "task_id": task_id, "path": "escape.txt"})
            assert symlink_read["ok"] is False, symlink_read
            symlink_write = rpc(port, {"op": "workspace_write", "task_id": task_id,
                                       "path": "escape.txt", "content": "overwrite attempt"})
            assert symlink_write["ok"] is False and outside.read_text(encoding="utf-8") == "do not read", symlink_write
            symlink_listing = rpc(port, {"op": "workspace_list", "task_id": task_id, "path": ""})
            assert symlink_listing["ok"] and all(item["name"] != "escape.txt" for item in symlink_listing["entries"]), symlink_listing
            nul_write = rpc(port, {"op": "workspace_write", "task_id": task_id, "path": "nul.txt", "content": "bad\u0000text"})
            assert nul_write["ok"] is False, nul_write
            (workspace_path / "binary.bin").write_bytes(b"\xff\x00binary")
            binary_read = rpc(port, {"op": "workspace_read", "task_id": task_id, "path": "binary.bin"})
            assert binary_read["ok"] is False and "UTF-8" in binary_read["error"], binary_read
            chunk_binary = rpc(port, {"op": "workspace_read_chunk", "task_id": task_id, "path": "binary.bin", "offset": 0, "limit": 32})
            assert chunk_binary["ok"] is False and "UTF-8" in chunk_binary["error"], chunk_binary
            # Large text files are streamed in bounded chunks while the legacy
            # workspace_read/write editor contract remains capped at 1 MiB.
            large_path = workspace_path / "large-stream.txt"
            large_line = "строка-😀-" + ("x" * 96) + "\n"
            large_path.write_text(large_line * 14000, encoding="utf-8")
            large_size = large_path.stat().st_size
            assert large_size > 1024 * 1024, large_size
            legacy_large_read = rpc(port, {"op": "workspace_read", "task_id": task_id, "path": "large-stream.txt"})
            assert legacy_large_read["ok"] is False, legacy_large_read
            chunk = rpc(port, {"op": "workspace_read_chunk", "task_id": task_id, "path": "large-stream.txt",
                               "offset": 0, "limit": 32768})
            assert chunk.get("ok") and chunk.get("size") == large_size, chunk
            assert chunk.get("offset") == 0 and 0 < chunk.get("next_offset", 0) <= 32768, chunk
            assert not chunk.get("eof") and chunk["content"].endswith("\n"), chunk
            next_chunk = rpc(port, {"op": "workspace_read_chunk", "task_id": task_id, "path": "large-stream.txt",
                                    "offset": chunk["next_offset"], "limit": 32768})
            assert next_chunk.get("ok") and next_chunk.get("offset") == chunk["next_offset"], next_chunk
            assert next_chunk.get("content", "").startswith("строка-") and                    next_chunk.get("next_offset", 0) > chunk["next_offset"], next_chunk
            bad_offset = rpc(port, {"op": "workspace_read_chunk", "task_id": task_id, "path": "large-stream.txt",
                                    "offset": large_size + 1, "limit": 32768})
            assert bad_offset.get("ok") is False, bad_offset
            chunk_traversal = rpc(port, {"op": "workspace_read_chunk", "task_id": task_id,
                                         "path": "../outside.txt", "offset": 0, "limit": 32})
            assert chunk_traversal.get("ok") is False, chunk_traversal
            status_by_prefix = rpc(port, {"op": "status", "task_id": task_id[:6]})
            assert status_by_prefix["ok"] and status_by_prefix["task_id"] == task_id, status_by_prefix
            duplicate = rpc(port, {"op": "submit", "task": "ignore changed payload due idempotency", 
                                   "idempotency_key": "same-request-1"})
            assert duplicate["ok"] and duplicate["duplicate"] is True and duplicate["task_id"] == task_id

            def task_done():
                current = rpc(port, {"op": "status", "task_id": task_id})
                return current if current.get("status") in ("completed", "completed_without_model", "failed", "cancelled") else None
            final = wait_for(task_done, label="submitted task completion")
            assert final["status"] == "completed_without_model", final
            assert final["priority"] == 7
            listed = rpc(port, {"op": "list_tasks", "limit": 10})
            assert listed["ok"] and any(t["task_id"] == task_id and t["priority"] == 7 for t in listed["tasks"])
            result = final["result"]
            assert result["status"] == "completed_without_model"
            assert len(result["decomposition"]["steps"]) == 3
            assert len(result["formalization"]) == 3
            assert len(result["agent_results"]) == 3
            assert all(item["model_used"] is False for item in result["agent_results"])
            assert final["attempts"] == 1

            cancel_done = rpc(port, {"op": "cancel", "task_id": task_id})
            assert cancel_done["ok"] is False, cancel_done
            missing = rpc(port, {"op": "status", "task_id": "not-a-real-id"})
            assert missing["ok"] is False
            health = rpc(port, {"op": "health"})
            assert health["ok"] and health["tasks_by_status"]["completed_without_model"] >= 1
        finally:
            stop_server(proc)

        print("[integration] deleted working-directory recovery", flush=True)
        deleted_cwd_proc = start_server(free_port(), tmpdir / "deleted-cwd.sqlite3", removed_cwd=True)
        try:
            # This triggers the optional nvidia-smi probe, which used to inherit
            # the unlinked working directory through popen(/bin/sh).
            assert rpc(int(json.loads((tmpdir / "deleted-cwd.sqlite3").parent.joinpath("config.json").read_text())
                          ["server"]["port"]), {"op": "models"})["ok"] is True
        finally:
            deleted_cwd_stderr = stop_server(deleted_cwd_proc)
        assert "shell-init: error retrieving current directory" not in deleted_cwd_stderr, deleted_cwd_stderr

        # Simulate a crash after a worker has claimed a task, then verify startup recovery.
        raw_request = json.dumps({"op": "submit", "task": "recovered after restart", "session_id": "recovery"})
        con = sqlite3.connect(db_path)
        con.execute("""INSERT INTO tasks(id,task_json,session_id,status,attempts,max_attempts)
                      VALUES(?,?,?,?,?,?)""",
                    ("recovery-task", raw_request, "recovery", "running", 1, 3))
        con.commit()
        con.close()

        proc = start_server(port, db_path)
        try:
            def recovery_done():
                row = rpc(port, {"op": "status", "task_id": "recovery-task"})
                return row if row.get("status") in ("completed", "completed_without_model", "failed", "cancelled") else None
            recovered = wait_for(recovery_done, label="restart recovery")
            assert recovered["status"] == "completed_without_model", recovered
            assert recovered["attempts"] == 2, recovered
            assert "recovered after server restart" not in recovered.get("error", "")
        finally:
            stop_server(proc)

        print("[integration] mock LLM, clarification, MCP, scheduling and C CLI chat", flush=True)
        # End-to-end MCP stdio discovery + model-generated tool call through agent allow-list.
        mcp_port = free_port()
        llm_port = free_port()
        mcp_db = tmpdir / "mcp-tasks.sqlite3"
        test_agents = tmpdir / "mcp-agents"
        shutil.copytree(PROJECT / "config" / "agents", test_agents)
        generalist_cfg = json.loads((test_agents / "generalist.json").read_text(encoding="utf-8"))
        generalist_cfg["tools"].extend(["mcp__testserver__echo", "mcp__httpserver__echo", "mcp__testserver__resources_list",
                                         "mcp__testserver__resource_read", "mcp__testserver__prompts_list",
                                         "mcp__testserver__prompt_get"])
        (test_agents / "generalist.json").write_text(json.dumps(generalist_cfg), encoding="utf-8")
        MockLLMHandler.delay_seconds = 0.35
        httpd = ThreadingHTTPServer(("127.0.0.1", llm_port), MockLLMHandler)
        http_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        http_thread.start()
        MockMCPHTTPHandler.reset()
        http_mcp_port = free_port()
        http_mcpd = ThreadingHTTPServer(("127.0.0.1", http_mcp_port), MockMCPHTTPHandler)
        http_mcp_thread = threading.Thread(target=http_mcpd.serve_forever, daemon=True)
        http_mcp_thread.start()
        prior_mcp_token = os.environ.get("ASGEND_TEST_MCP_TOKEN")
        os.environ["ASGEND_TEST_MCP_TOKEN"] = "test-token"
        try:
            proc = start_server(mcp_port, mcp_db, mcp_servers=[{
                "name": "testserver", "command": sys.executable,
                "args": [str(PROJECT / "tests" / "mock_mcp_server.py")],
                "read_only_paths": [str(PROJECT / "knowledge")], "timeout_seconds": 5
            }, {
                "name": "httpserver", "transport": "streamable_http",
                "url": f"http://127.0.0.1:{http_mcp_port}/mcp",
                "bearer_token_env": "ASGEND_TEST_MCP_TOKEN", "timeout_seconds": 8
            }], llm_enabled=True, llm_base_url=f"http://127.0.0.1:{llm_port}/v1", agents_dir=test_agents,
                mcp_modern=True)
        finally:
            if prior_mcp_token is None: os.environ.pop("ASGEND_TEST_MCP_TOKEN", None)
            else: os.environ["ASGEND_TEST_MCP_TOKEN"] = prior_mcp_token
        try:
            task = rpc(mcp_port, {"op": "submit", "task": "NEEDS_CLARIFICATION: Implement this small feature using the MCP tool",
                                  "session_id": "mcp-session"})
            assert task["ok"]
            waiting = wait_for(lambda: (lambda current: current if current.get("status") == "waiting_input" else None)(
                               rpc(mcp_port, {"op": "status", "task_id": task["task_id"]})), timeout=8,
                               label="task pauses for user clarification")
            question = waiting["result"]["question"]
            assert "Where should this be deployed?" in question["question"] and len(question["options"]) == 3, waiting
            event_rows = rpc(mcp_port, {"op": "events", "task_id": task["task_id"]})
            assert event_rows["ok"] and any(row["type"] == "question" for row in event_rows["events"]), event_rows
            answer = rpc(mcp_port, {"op": "answer", "task_id": task["task_id"], "option_id": "local", "answer": "Locally on this device"})
            assert answer["ok"] and answer["status"] == "queued", answer
            wait_for(lambda: rpc(mcp_port, {"op": "status", "task_id": task["task_id"]}).get("status") == "running",
                     timeout=5, label="clarified task resumes running")
            low_priority = rpc(mcp_port, {"op": "submit", "task": "low priority scheduled task", "priority": 0})
            urgent_priority = rpc(mcp_port, {"op": "submit", "task": "urgent priority scheduled task", "priority": 9})
            queued = rpc(mcp_port, {"op": "submit", "task": "This queued task will be cancelled", "priority": 5})
            assert low_priority["ok"] and urgent_priority["ok"] and queued["ok"]
            # The single worker is still occupied by the delayed first task. Once it is free,
            # urgent priority must be claimed before low priority.
            def priority_started():
                urgent_status = rpc(mcp_port, {"op": "status", "task_id": urgent_priority["task_id"]})
                low_status = rpc(mcp_port, {"op": "status", "task_id": low_priority["task_id"]})
                if urgent_status.get("status") == "running": return "urgent"
                if low_status.get("status") == "running": return "low"
                if urgent_status.get("attempts", 0) > 0: return "urgent_already_started"
                if low_status.get("attempts", 0) > 0: return "low_already_started"
                return None
            scheduled_first = wait_for(priority_started, timeout=15, label="priority scheduling decision")
            assert scheduled_first.startswith("urgent"), f"priority 9 should run before priority 0, observed {scheduled_first}"
            for queued_item in (low_priority, queued):
                cancel_reply = rpc(mcp_port, {"op": "cancel", "task_id": queued_item["task_id"]})
                assert cancel_reply["ok"] and cancel_reply["status"] == "cancellation_requested"
            cancelled_ids = {low_priority["task_id"], queued["task_id"]}
            for cancelled_id in cancelled_ids:
                cancelled = wait_for(lambda tid=cancelled_id: (lambda current: current if current.get("status") == "cancelled" else None)(
                                     rpc(mcp_port, {"op": "status", "task_id": tid})),
                                     timeout=3, label="queued task cancellation")
                assert cancelled["attempts"] == 0

            def mcp_done():
                current = rpc(mcp_port, {"op": "status", "task_id": task["task_id"]})
                return current if current.get("status") in ("completed", "completed_without_model", "failed", "cancelled") else None
            mcp_result = wait_for(mcp_done, timeout=15, label="MCP tool call")
            assert mcp_result["status"] == "completed", mcp_result
            assert mcp_result["result"]["selected_agent"] == "generalist", mcp_result
            middle_output = mcp_result["result"]["agent_results"][1]["output"]
            assert "MOCK_MCP_EXECUTED:hello-from-agent" in middle_output, middle_output

            protocol_task = rpc(mcp_port, {"op": "submit", "task": "MCP_PROTOCOL_TOOL_TEST: list and read resources, then enumerate and fetch a prompt", "agent": "generalist"})
            assert protocol_task["ok"], protocol_task
            protocol_result = wait_for(lambda: (lambda current: current if current.get("status") in ("completed", "failed", "cancelled", "completed_without_model") else None)(
                                        rpc(mcp_port, {"op": "status", "task_id": protocol_task["task_id"]})),
                                        timeout=20, label="MCP resource and prompt operations")
            assert protocol_result["status"] == "completed", protocol_result
            stream_events = rpc(mcp_port, {"op": "events", "task_id": protocol_task["task_id"], "limit": 500})["events"]
            text_deltas = [event for event in stream_events if event["type"] == "assistant_delta"]
            assert text_deltas, stream_events
            # Every visible content fragment carries correlation metadata so the CLI
            # can append it to the correct model/stream tile without waiting for [DONE].
            delta_sequences = {}
            reconstructed_stream_text = {}
            for event in text_deltas:
                payload = event.get("payload", {})
                delta = payload.get("delta")
                model_name = payload.get("model_name")
                stream_id = payload.get("stream_id")
                sequence = payload.get("sequence")
                assert isinstance(delta, str) and delta, event
                assert isinstance(model_name, str) and model_name, event
                assert isinstance(stream_id, str) and stream_id, event
                assert isinstance(sequence, int) and sequence > 0, event
                stream_key = (model_name, stream_id)
                delta_sequences.setdefault(stream_key, []).append(sequence)
                reconstructed_stream_text[stream_key] = reconstructed_stream_text.get(stream_key, "") + delta
            assert all(seq == sorted(seq) for seq in delta_sequences.values()), delta_sequences
            assert all(text for text in reconstructed_stream_text.values()), reconstructed_stream_text
            tool_text_deltas = [event for event in stream_events if event.get("type") == "tool_call_stream_delta"]
            if tool_text_deltas:
                assert any(isinstance(event.get("payload", {}).get("argument_delta"), str) and
                           event["payload"]["argument_delta"] for event in tool_text_deltas), tool_text_deltas
            stream_statuses = [event.get("payload", {}).get("status") for event in stream_events if event["type"] == "llm_status"]
            assert "waiting-first-token" in stream_statuses and "streaming" in stream_statuses and "completed" in stream_statuses, stream_statuses
            # The fixture leaves HTTP/1.1 keep-alive open after [DONE]; completed
            # status timing should exclude that 0.8s linger when the client exits on [DONE].
            completed_times = [event.get("payload", {}).get("elapsed_ms", 999999) for event in stream_events
                               if event["type"] == "llm_status" and event.get("payload", {}).get("status") == "completed"]
            assert completed_times and min(completed_times) < 750, completed_times
            protocol_output = protocol_result["result"]["agent_results"][1]["output"]
            for expected_fragment in ("usage.md", "MOCK_RESOURCE_CONTENT", "summarize", "verify", "MOCK_PROMPT_CONTENT:summarize"):
                assert expected_fragment in protocol_output, (expected_fragment, protocol_output)
            http_task = rpc(mcp_port, {"op": "submit", "task": "MCP_HTTP_TEST: invoke the HTTP MCP echo tool", "agent": "generalist"})
            assert http_task["ok"], http_task
            http_result = wait_for(lambda: (lambda current: current if current.get("status") in ("completed", "failed", "cancelled", "completed_without_model") else None)(
                                   rpc(mcp_port, {"op": "status", "task_id": http_task["task_id"]})),
                                   timeout=20, label="MCP Streamable HTTP tool call")
            assert http_result["status"] == "completed", http_result
            http_output = http_result["result"]["agent_results"][1]["output"]
            assert "HTTP_MCP_EXECUTED:hello-from-agent" in http_output, http_output
            with MockMCPHTTPHandler.lock:
                assert "server/discover" in MockMCPHTTPHandler.methods, MockMCPHTTPHandler.methods
                assert "tools/list" in MockMCPHTTPHandler.methods, MockMCPHTTPHandler.methods
                assert "tools/call" in MockMCPHTTPHandler.methods, MockMCPHTTPHandler.methods
                assert MockMCPHTTPHandler.sse_count >= 1 and MockMCPHTTPHandler.tool_count >= 1, (MockMCPHTTPHandler.sse_count, MockMCPHTTPHandler.tool_count, MockMCPHTTPHandler.methods, MockMCPHTTPHandler.errors)
                assert not MockMCPHTTPHandler.errors, MockMCPHTTPHandler.errors

            decomposition = mcp_result["result"]["decomposition"]
            assert decomposition["stage_1_clarification"]["model_assisted"] is True, decomposition
            assert decomposition["stage_1_clarification"]["clarification_needed"] is True, decomposition
            assert decomposition["stage_2_step_decomposition"]["model_assisted"] is True, decomposition
            assert decomposition["stage_3_quality_review"]["model_assisted"] is True, decomposition
            assert decomposition["stage_3_quality_review"]["review"]["score"] == 8, decomposition
            assert decomposition["steps"][0]["title"] == "Inspect inputs and constraints", decomposition

            followup = rpc(mcp_port, {"op": "submit", "task": "Continue this work and use the MCP echo tool",
                                      "agent": "generalist", "session_id": "mcp-session"})
            assert followup["ok"]
            followup_result = wait_for(lambda: (lambda current: current if current.get("status") in ("completed", "failed", "cancelled", "completed_without_model") else None)(
                                       rpc(mcp_port, {"op": "status", "task_id": followup["task_id"]})),
                                       timeout=15, label="session-context follow-up")
            assert followup_result["status"] == "completed", followup_result
            followup_output = followup_result["result"]["agent_results"][1]["output"]
            assert "prior_context=true" in followup_output, followup_output
            # The generic follow-up may select either echo transport. If HTTP is
            # selected, its required header-bound region must be propagated and
            # any protocol mismatch must fail the integration test, not just log.
            with MockMCPHTTPHandler.lock:
                assert not MockMCPHTTPHandler.errors, (MockMCPHTTPHandler.errors, followup_output)

            # Streamable HTTP: cancel only after the mock endpoint has received
            # the active tool call, and require completion well before its 8s timeout.
            MockMCPHTTPHandler.cancel_tool_started.clear()
            cancel_http = rpc(mcp_port, {"op": "submit", "task": "MCP_CANCEL_HTTP_TEST: cancel the active HTTP MCP tool call",
                                          "agent": "generalist"})
            assert cancel_http["ok"], cancel_http
            wait_for(lambda: True if MockMCPHTTPHandler.cancel_tool_started.is_set() else None,
                     timeout=25, label="HTTP MCP mock receives the slow cancellable tool call")
            cancel_http_at = time.monotonic()
            cancel_http_reply = rpc(mcp_port, {"op": "cancel", "task_id": cancel_http["task_id"]})
            assert cancel_http_reply.get("ok"), cancel_http_reply
            cancel_http_final = wait_for(lambda: (lambda row: row if row.get("status") == "cancelled" else None)(
                                         rpc(mcp_port, {"op": "status", "task_id": cancel_http["task_id"]})),
                                         timeout=3, label="active HTTP MCP request aborts after cancellation")
            assert time.monotonic() - cancel_http_at < 3, cancel_http_final

            # Stdio: the mock writes a marker in its private sandbox only after
            # it has received tools/call, then waits longer than the MCP timeout.
            for marker_file in Path("/tmp").glob("ao-mcp-sandbox-*/.mcp-cancel-started"):
                try: marker_file.unlink()
                except OSError: pass
            cancel_stdio = rpc(mcp_port, {"op": "submit", "task": "MCP_CANCEL_STDIO_TEST: cancel the active stdio MCP tool call",
                                           "agent": "generalist"})
            assert cancel_stdio["ok"], cancel_stdio
            wait_for(lambda: next(Path("/tmp").glob("ao-mcp-sandbox-*/.mcp-cancel-started"), None),
                     timeout=25, label="stdio MCP mock receives the slow cancellable tool call")
            cancel_stdio_at = time.monotonic()
            cancel_stdio_reply = rpc(mcp_port, {"op": "cancel", "task_id": cancel_stdio["task_id"]})
            assert cancel_stdio_reply.get("ok"), cancel_stdio_reply
            cancel_stdio_final = wait_for(lambda: (lambda row: row if row.get("status") == "cancelled" else None)(
                                           rpc(mcp_port, {"op": "status", "task_id": cancel_stdio["task_id"]})),
                                           timeout=2.5, label="active stdio MCP request aborts after cancellation")
            assert time.monotonic() - cancel_stdio_at < 2.5, cancel_stdio_final

            # Cancel a task while it is running through model-assisted decomposition.
            running_cancel = rpc(mcp_port, {"op": "submit", "task": "Cancel cooperatively while running",
                                           "agent": "generalist"})
            assert running_cancel["ok"]
            wait_for(lambda: rpc(mcp_port, {"op": "status", "task_id": running_cancel["task_id"]}).get("status") == "running",
                     timeout=5, label="cancellable task enters running state")
            cancel_running_reply = rpc(mcp_port, {"op": "cancel", "task_id": running_cancel["task_id"]})
            assert cancel_running_reply["ok"]
            running_cancelled = wait_for(lambda: (lambda current: current if current.get("status") == "cancelled" else None)(
                                         rpc(mcp_port, {"op": "status", "task_id": running_cancel["task_id"]})),
                                         timeout=8, label="running task cancellation")
            assert running_cancelled["cancel_requested"] is True, running_cancelled
            assert running_cancelled["result"]["status"] == "cancelled", running_cancelled

            print("[integration] C CLI create/question/answer/poll flow", flush=True)
            # Exercise the compiled C CLI's create -> question dialog -> answer -> same-task polling path.
            cli_env = dict(os.environ, ASGEND_HOST="127.0.0.1", ASGEND_PORT=str(mcp_port),
                           ASGEND_TOKEN="integration-secret")
            cli_chat = subprocess.run([str(BINARY.parent / "asgend-cli")],
                                      input="2\nNEEDS_MULTIPLE_CLARIFICATIONS: C CLI interactive chat check\n5\n1\n2\nq\n",
                                      text=True, capture_output=True, timeout=40, env=cli_env, check=False)
            assert cli_chat.returncode == 0, (cli_chat.returncode, cli_chat.stdout[-5000:], cli_chat.stderr)
            assert "Задача создана:" in cli_chat.stdout and "Уточнение от агента" in cli_chat.stdout, cli_chat.stdout
            assert "1) Locally on this device" in cli_chat.stdout and "Результат:" in cli_chat.stdout, (cli_chat.stdout[-8000:], cli_chat.stderr[-3000:])
            assert "Уточнение 2" in cli_chat.stdout, cli_chat.stdout[-8000:]
            assert "payload:" not in cli_chat.stdout and '"task_id"' not in cli_chat.stdout, cli_chat.stdout[-8000:]
            assert cli_chat.stdout.count("Статус: выполняется") < 12, cli_chat.stdout[-8000:]
            # This invocation captures stdin/stdout with pipes, so the CLI must
            # stay in its documented line-oriented fallback instead of emitting
            # a full-screen frame. Full-screen TUI and boxed streamed answers
            # are verified by the PTY regressions below and earlier in this suite.
            assert "ASGEND core client · interactive task/chat mode" in cli_chat.stdout, cli_chat.stdout[-4000:]
            assert "\nASGEND\n" in cli_chat.stdout and "1  Список задач" in cli_chat.stdout, cli_chat.stdout[-4000:]
            assert "\x1b[?1049h" not in cli_chat.stdout, cli_chat.stdout[-4000:]

            # Unlike the captured non-TTY test above, this test asserts actual
            # carriage-return/ANSI spinner frames through a pseudo-terminal.
            previous_delay = MockLLMHandler.delay_seconds
            try:
                MockLLMHandler.delay_seconds = 0.6
                run_cli_pty_regression(BINARY.parent / "asgend-cli", cli_env)
            finally:
                MockLLMHandler.delay_seconds = previous_delay
        finally:
            stop_server(proc)
            httpd.shutdown()
            httpd.server_close()
            http_thread.join(timeout=2)
            http_mcpd.shutdown()
            http_mcpd.server_close()
            http_mcp_thread.join(timeout=2)
    print("[integration] legacy MCP initialize compatibility", flush=True)
    with tempfile.TemporaryDirectory(prefix="ao-c-mcp-legacy-") as legacy_tmp:
        legacy_dir = Path(legacy_tmp)
        legacy_port = free_port()
        legacy_proc = start_server(legacy_port, legacy_dir / "legacy.sqlite3", mcp_servers=[{
            "name": "legacyserver", "command": sys.executable,
            "args": [str(PROJECT / "tests" / "mock_mcp_server.py")],
            "read_only_paths": [str(PROJECT / "knowledge")], "timeout_seconds": 3
        }])
        try:
            legacy_status = rpc(legacy_port, {"op": "ping"})
            assert legacy_status.get("ok") is True, legacy_status
        finally:
            stop_server(legacy_proc)

    print("[integration] MCP child environment isolation independent of sandbox", flush=True)
    with tempfile.TemporaryDirectory(prefix="ao-c-mcp-env-") as env_tmp:
        env_dir = Path(env_tmp)
        env_port = free_port()
        env_probe = env_dir / "child-environment.json"
        prior_secret = os.environ.get("ASGEND_TEST_PARENT_SECRET")
        os.environ["ASGEND_TEST_PARENT_SECRET"] = "must-not-leak-from-service"
        try:
            env_proc = start_server(env_port, env_dir / "env.sqlite3", mcp_servers=[{
                "name": "envprobe", "command": sys.executable,
                "args": [str(PROJECT / "tests" / "mock_mcp_env_probe.py"), str(env_probe)],
                "env": {"ASGEND_MCP_EXPLICIT_VALUE": "configured-for-this-server"},
                "timeout_seconds": 3
            }], sandbox_enabled=False)
        finally:
            if prior_secret is None:
                os.environ.pop("ASGEND_TEST_PARENT_SECRET", None)
            else:
                os.environ["ASGEND_TEST_PARENT_SECRET"] = prior_secret
        try:
            assert rpc(env_port, {"op": "ping"}).get("ok") is True
            env_data = wait_for(lambda: json.loads(env_probe.read_text(encoding="utf-8"))
                                if env_probe.exists() else None,
                                timeout=5, label="MCP child writes its effective startup environment")
            assert env_data["parent_secret"] is None, env_data
            assert env_data["explicit_value"] == "configured-for-this-server", env_data
            assert env_data["path"] == "/usr/local/bin:/usr/bin:/bin", env_data
            assert env_data["ld_preload"] is None, env_data
            assert env_data["home"] == "/tmp" and env_data["tmpdir"] == "/tmp", env_data
        finally:
            stop_server(env_proc)

        # Security-sensitive loader/search-path variables must not be overridden by config.
        try:
            start_server(free_port(), env_dir / "invalid-env.sqlite3", mcp_servers=[{
                "name": "invalidenv", "command": sys.executable,
                "args": [str(PROJECT / "tests" / "mock_mcp_env_probe.py"), str(env_dir / "invalid.json")],
                "env": {"PATH": "/tmp/attacker-controlled"}, "timeout_seconds": 3
            }], sandbox_enabled=False)
        except AssertionError as invalid_env_error:
            assert "MCP env cannot override PATH" in str(invalid_env_error), str(invalid_env_error)
        else:
            raise AssertionError("MCP env configuration unexpectedly allowed PATH override")

    print("[integration] MCP child restart and exponential backoff supervision", flush=True)
    with tempfile.TemporaryDirectory(prefix="ao-c-mcp-restart-") as restart_tmp:
        restart_dir = Path(restart_tmp)
        restart_port = free_port()
        restart_db = restart_dir / "restart.sqlite3"
        restart_state = restart_dir / "mcp-state.txt"
        restart_agents = restart_dir / "agents"
        shutil.copytree(PROJECT / "config" / "agents", restart_agents)
        restart_agent_path = restart_agents / "generalist.json"
        restart_agent_cfg = json.loads(restart_agent_path.read_text(encoding="utf-8"))
        restart_agent_cfg.setdefault("tools", []).append("mcp__restartserver__echo")
        restart_agent_path.write_text(json.dumps(restart_agent_cfg, ensure_ascii=False), encoding="utf-8")

        restart_llm_port = free_port()
        restart_llm = ThreadingHTTPServer(("127.0.0.1", restart_llm_port), MockLLMHandler)
        restart_llm_thread = threading.Thread(target=restart_llm.serve_forever, daemon=True)
        restart_llm_thread.start()
        restart_proc = None
        try:
            restart_proc = start_server(restart_port, restart_db, agents_dir=restart_agents,
                llm_enabled=True, llm_base_url=f"http://127.0.0.1:{restart_llm_port}/v1",
                mcp_servers=[{"name": "restartserver", "command": sys.executable,
                              "args": [str(PROJECT / "tests" / "mock_mcp_restart_server.py"), str(restart_state)],
                              "timeout_seconds": 3}], sandbox_enabled=False)
            wait_for(lambda: "restart_ready" in restart_state.read_text(encoding="utf-8")
                     if restart_state.exists() else None,
                     timeout=10, label="MCP process restarts and completes a fresh handshake")
            assert rpc(restart_port, {"op": "ping"}).get("ok") is True
            restart_task = rpc(restart_port, {"op": "submit", "task": "Verify MCP tool call after supervised restart",
                                               "agent": "generalist"})
            assert restart_task.get("ok") is True, restart_task
            restart_result = wait_for(lambda: (lambda current: current if current.get("status") in
                ("completed", "failed", "cancelled", "completed_without_model") else None)(
                    rpc(restart_port, {"op": "status", "task_id": restart_task["task_id"]})),
                timeout=25, label="MCP tool call succeeds after supervised restart")
            assert restart_result["status"] == "completed", restart_result
            restart_output = json.dumps(restart_result.get("result", {}), ensure_ascii=False)
            assert "MCP_RESTART_RECOVERED:hello-from-agent" in restart_output, restart_output
        finally:
            if restart_proc is not None:
                stop_server(restart_proc)
            restart_llm.shutdown()
            restart_llm.server_close()
            restart_llm_thread.join(timeout=2)

    print("[integration] LSP JSON-RPC diagnostics", flush=True)
    with tempfile.TemporaryDirectory(prefix="ao-c-lsp-") as lsp_tmp:
        lsp_dir = Path(lsp_tmp)
        lsp_file = lsp_dir / "diagnostics.lspx"
        lsp_file.write_text("mock lsp diagnostic test\n", encoding="utf-8")
        lsp_db = lsp_dir / "tasks.sqlite3"
        lsp_port = free_port()
        lsp_cfg = [{"language": "plaintext", "command": sys.executable,
                    "args": [str(PROJECT / "tests" / "mock_lsp_server.py")],
                    "extensions": [".lspx"], "root_dir": str(lsp_dir), "timeout_seconds": 3}]
        lsp_proc = start_server(lsp_port, lsp_db, lsp_servers=lsp_cfg)
        try:
            lsp_result = rpc(lsp_port, {"op": "lsp_diagnostics", "file_path": str(lsp_file)})
            assert lsp_result.get("ok") is True, lsp_result
            diagnostic_text = lsp_result.get("diagnostics", "")
            assert "mock error from LSP" in diagnostic_text and ":1:1: ERROR:" in diagnostic_text, lsp_result
        finally:
            stop_server(lsp_proc)

    print("[integration] managed model lifecycle + automatic local resource/project memory sync", flush=True)
    # Managed local LLM starts lazily on demand; a decision model can be pre-launched at server boot.
    with tempfile.TemporaryDirectory(prefix="ao-c-models-") as model_tmp:
        model_dir = Path(model_tmp)
        model_agents = model_dir / "agents"
        shutil.copytree(PROJECT / "config" / "agents", model_agents)
        base_profile = json.loads((model_agents / "generalist.json").read_text(encoding="utf-8"))
        flexible_profiles = [
            ("agent-alpha", ["planning"], ["knowledge_search"]),
            ("agent-beta", ["coding"], ["echo"]),
            ("agent-gamma", ["review"], ["knowledge_search"]),
        ]
        base_profile["model_id"] = "managed-agent"
        (model_agents / "generalist.json").write_text(json.dumps(base_profile), encoding="utf-8")
        for profile_id, skills, tools in flexible_profiles:
            profile = dict(base_profile, id=profile_id, name=f"Flexible profile {profile_id}",
                           description="Flexible multi-purpose agent selected according to the current subtask.",
                           skills=skills, tools=tools)
            (model_agents / f"{profile_id}.json").write_text(json.dumps(profile), encoding="utf-8")
        # Give the selector more than the historical 128-file threshold and isolated live directories.
        resource_dirs = {}
        for dirname in ("skills", "system_prompts", "tools", "knowledge", "instructions"):
            target = model_dir / dirname
            shutil.copytree(PROJECT / dirname, target)
            resource_dirs[dirname] = target
        for index in range(140):
            (resource_dirs["skills"] / f"extra-{index:03d}.md").write_text(
                f"Extra skill catalog entry {index}; test dynamic discovery.\n", encoding="utf-8")
        agent_port, analysis_port, decision_port = free_port(), free_port(), free_port()
        request_log = model_dir / "model-requests.jsonl"
        test_gguf = model_dir / "tiny-test.gguf"
        write_test_gguf(test_gguf)
        base_model = {
            "enabled": True, "auto_start": True, "start_at_boot": False, "executable": sys.executable,
            "model_path": "", "working_directory": str(model_dir), "args": [],
            "startup_timeout_seconds": 8, "shutdown_timeout_seconds": 2,
            "request_timeout_seconds": 5, "temperature": 0.1, "api_key": "",
        }
        agent_model = dict(base_model, id="managed-agent", name="Managed agent model", tier="medium",
                           model_path=str(test_gguf), ram_estimate_mb=0, vram_estimate_mb=0, port=agent_port,
                           base_url=f"http://127.0.0.1:{agent_port}/v1", health_url=f"http://127.0.0.1:{agent_port}/health",
                           model_name="agent-profile-name", args=[str(PROJECT / "tests" / "managed_model_server.py"),
                           "--port", str(agent_port), "--tag", "agent", "--requests", str(request_log)])
        analysis_model = dict(base_model, id="managed-analysis", name="Managed large analysis model", tier="large",
                              ram_estimate_mb=0, vram_estimate_mb=0, port=analysis_port,
                              base_url=f"http://127.0.0.1:{analysis_port}/v1", health_url=f"http://127.0.0.1:{analysis_port}/health",
                              model_name="analysis-profile-name", args=[str(PROJECT / "tests" / "managed_model_server.py"),
                              "--port", str(analysis_port), "--tag", "analysis", "--requests", str(request_log)])
        decision_model = dict(base_model, id="managed-router", name="Managed decision model", tier="decision",
                              ram_estimate_mb=0, vram_estimate_mb=0, port=decision_port,
                              auto_start=False, start_at_boot=True,
                              base_url=f"http://127.0.0.1:{decision_port}/v1", health_url=f"http://127.0.0.1:{decision_port}/health",
                              model_name="decision-profile-name", args=[str(PROJECT / "tests" / "managed_model_server.py"),
                              "--port", str(decision_port), "--tag", "decision", "--requests", str(request_log)])
        registry = {"defaults": {"model_id": "managed-agent", "analysis_model_id": "managed-analysis", "decision_model_id": "managed-router"},
                    "models": [agent_model, analysis_model], "decision_models": [decision_model]}
        models_db = model_dir / "managed-tasks.sqlite3"
        # Project source root is deliberately separate from config/resource root.
        project_root = model_dir / "project-src"
        project_root.mkdir()
        # This project document is deliberately not scanned or extracted here. The first task
        # must create/update project_root/.asgend_mem automatically and inject matching evidence.
        if not MEMORY_CLI.is_file():
            raise AssertionError(f"memory CLI sibling binary is required for retrieval integration test: {MEMORY_CLI}")
        memory_docs = project_root / "memory-docs"
        memory_docs.mkdir()
        (memory_docs / "project-memory-proof.md").write_text(
            "# Persistent project memory probe\n\nASGEND_MEMORY_CONTEXT_INTEGRATION_PROBE "
            "A verified local reference for task architecture and implementation trade-offs.\n", encoding="utf-8")
        managed_proc = start_server(free_port(), models_db, model_registry=registry,
                                    agents_dir=model_agents, agent_model_id="managed-agent", ui_enabled=True,
                                    resource_dirs=resource_dirs, project_root=project_root)
        try:
            # This file is created after startup; the next classifier request must discover it without restart.
            (resource_dirs["knowledge"] / "late-added-after-boot.md").write_text(
                "Late-added reference for project architecture, implementation trade-offs, code review, "
                "and echo tool-call verification. ASGEND_AUTO_RESOURCE_DISCOVERY_V1\n", encoding="utf-8")
            managed_port = int(json.loads((model_dir / "config.json").read_text())["server"]["port"])
            initial_health = rpc(managed_port, {"op": "health"})
            initial_models = initial_health["model_status"]["models"]
            initial_decisions = initial_health["model_status"]["decision_models"]
            assert len(initial_models) == 2 and all(row["running"] is False for row in initial_models), initial_models
            assert len(initial_decisions) == 1 and initial_decisions[0]["running"] is True, initial_decisions
            assert next(row for row in initial_models if row["id"] == "managed-agent")["parallel_cap_by_tier"] == 2, initial_models
            assert next(row for row in initial_models if row["id"] == "managed-analysis")["parallel_cap_by_tier"] == 1, initial_models
            assert initial_decisions[0]["parallel_cap_by_tier"] == 1, initial_decisions
            gguf_model = next(row for row in initial_models if row["id"] == "managed-agent")
            assert gguf_model["disk_size_mb"] == 1, gguf_model
            assert gguf_model["kv_bytes_per_token_estimate"] == 512, gguf_model
            assert gguf_model["estimated_kv_cache_mb"] == 1, gguf_model
            memory_state = rpc(managed_port, {"op": "models"})["resources"]
            assert "ram_available_mb" in memory_state and "ram_without_models_estimated_mb" in memory_state, memory_state
            assert "running_model_pss_total_mb" in memory_state and isinstance(memory_state["running_models"], list), memory_state
            assert "shared_gpu_memory_used_mb" in memory_state and "ram_without_models_estimated_method" in memory_state, memory_state
            assert "vram_used_mb" in memory_state and "vram_without_models_estimated_mb" in memory_state, memory_state
            assert "vram_without_models_estimated_method" in memory_state, memory_state
            assert memory_state["ram_without_models_estimated_mb"] >= memory_state["ram_available_mb"], memory_state
            router_row = next(item for item in memory_state["running_models"] if item["id"] == "managed-router")
            assert router_row["actual_pss_mb"] >= 0 and "actual_gpu_memory_mb" in router_row, router_row
            print("[integration] managed task submit", flush=True)
            task = rpc(managed_port, {"op": "submit", "task": "Implement a small verified feature in code, plan its architecture, compare design trade-offs, consult ASGEND_MEMORY_CONTEXT_INTEGRATION_PROBE, use echo to verify a tool-call flow, and consult project-principles.md and late-added-after-boot.md"})
            assert task["ok"], task
            final = wait_for(lambda: (lambda current: current if current.get("status") in ("completed", "failed", "cancelled", "completed_without_model") else None)(
                             rpc(managed_port, {"op": "status", "task_id": task["task_id"]})), timeout=45,
                             label="managed model routed task completion")
            assert final["status"] == "completed", final
            assert final.get("title") == "Review ASGEND task editor and safely preserve existing", final.get("title")
            print("[integration] managed first task completed", flush=True)
            assert final["result"]["agent_results"][0]["model_used"] is True, final["result"].get("agent_results")
            post_run_models = rpc(managed_port, {"op": "health"})["model_status"]["models"]
            running_agent_model = next(row for row in post_run_models if row["id"] == "managed-agent")
            assert 1 <= running_agent_model["planned_parallel"] <= running_agent_model["parallel_cap_by_tier"] == 2, running_agent_model
            assert running_agent_model["planned_context_tokens"] >= 2048, running_agent_model
            assert 0 <= running_agent_model["loaded_parallel"] <= running_agent_model["parallel_cap_by_tier"], running_agent_model
            formalized = final["result"].get("formalization", [])
            assert len(formalized) == 3, formalized
            # One flexible profile is selected once and reused for every step; each step uses
            # local BM25 against an independently synced resource index, not per-category model calls.
            assigned_profiles = {item.get("agent_id") for item in formalized}
            assert len(assigned_profiles) == 1, formalized
            assert not assigned_profiles.intersection({"coder", "planner", "reviewer"}), formalized
            for item in formalized:
                selected = item.get("resource_selection", {})
                assert selected.get("model_assisted") is False and selected.get("fallback") is False, selected
                assert selected.get("selection_mode") == "local_bm25" and selected.get("decision_model_calls") == 0, selected
                assert selected.get("resource_index_auto_sync") is True, selected
                assert "coding/SKILL.md" in selected.get("skills", []), selected
                assert (resource_dirs or {}).get("skills", PROJECT / "skills").joinpath("coding", "SKILL.md").is_file(), selected
                assert "coding_style.md" in selected.get("system_prompts", []), selected
                assert "echo.md" in selected.get("tools", []), selected
                assert "project-principles.md" in selected.get("knowledge", []), selected
            model_health = rpc(managed_port, {"op": "health"})["model_status"]
            running_rows = model_health["models"] + model_health["decision_models"]
            assert len(running_rows) == 3 and [row for row in running_rows if row["kind"] == "decision" and row["running"]], running_rows
            assert all(not row["running"] for row in running_rows if row["kind"] == "llm"), running_rows
            logged = [json.loads(line) for line in request_log.read_text(encoding="utf-8").splitlines()]
            assert any(item.get("tag") == "agent" and item.get("memory_probe_in_context") is True and
                       item.get("memory_document_in_context") is True for item in logged), logged
            assert any(item["tag"] == "decision" and item["model"] == "decision-profile-name" for item in logged), logged
            assert any(item["tag"] == "analysis" and item["model"] == "analysis-profile-name" for item in logged), logged
            assert any(item["tag"] == "agent" and item["model"] == "agent-profile-name" for item in logged), logged
            assert any(item.get("purpose") == "decomposition" and item["tag"] == "analysis" for item in logged), logged
            assert any(item.get("purpose") == "router" and item["tag"] == "decision" for item in logged), logged
            selectors = [item for item in logged if item.get("purpose") == "resource_classifier"]
            assert selectors == [], "normal resource selection must not call the decision model per category: " + repr(selectors)
            assert any(item.get("tag") == "agent" and item.get("memory_probe_in_context") is True and
                       item.get("memory_document_in_context") is True for item in logged), logged
            assert any(item.get("tag") == "agent" and item.get("auto_resource_v1_in_context") is True for item in logged), logged
            resource_db = model_dir / ".asgend_mem" / "resources.sqlite3"
            project_db = project_root / ".asgend_mem" / "index.sqlite3"
            assert resource_db.is_file() and project_db.is_file(), (resource_db, project_db)
            assert resource_db.parent != project_db.parent, (resource_db, project_db)
            with sqlite3.connect(resource_db) as connection:
                resource_rows = connection.execute("SELECT category,relative_path FROM resources").fetchall()
            assert any(category == "knowledge" and path == "late-added-after-boot.md" for category, path in resource_rows), resource_rows
            with sqlite3.connect(project_db) as connection:
                project_rows = connection.execute("SELECT relative_path FROM files").fetchall()
            project_paths = {row[0] for row in project_rows}
            assert "memory-docs/project-memory-proof.md" in project_paths, sorted(project_paths)[:30]
            assert not any(path.startswith(("skills/", "knowledge/", "tools/", "system_prompts/")) for path in project_paths), project_paths
            first_task_events = rpc(managed_port, {"op": "events", "task_id": task["task_id"], "limit": 500})["events"]
            tier_event = next((event for event in first_task_events if event["type"] == "task_tier_selected"), None)
            assert tier_event and tier_event.get("payload", {}).get("tier") == "high", tier_event

            # Automatic incremental refresh: mutate an already-indexed resource and project file after
            # the service is running, then verify the next task sees the new text without CLI scan,
            # model enrichment, or a service restart.
            resource_refresh_marker = "ASGEND_AUTO_RESOURCE_REFRESH_V2"
            project_refresh_marker = "ASGEND_AUTO_PROJECT_REFRESH_V2"
            (resource_dirs["knowledge"] / "late-added-after-boot.md").write_text(
                "Updated knowledge about project architecture, code implementation and tool invocation. "
                f"{resource_refresh_marker}\n", encoding="utf-8")
            (memory_docs / "project-memory-proof.md").write_text(
                "# Updated project memory probe\n\n"
                f"{project_refresh_marker} verified local source for implementation architecture and trade-offs. "
                "This document intentionally changes size so the incremental scanner observes it.\n", encoding="utf-8")
            refresh_task = rpc(managed_port, {"op": "submit", "task":
                f"Inspect the updated project architecture and code references. Find {resource_refresh_marker} "
                f"from knowledge and {project_refresh_marker} from project memory, and explain implementation trade-offs."})
            assert refresh_task.get("ok"), refresh_task
            refresh_final = wait_for(lambda: (lambda current: current if current.get("status") in
                ("completed", "failed", "cancelled", "completed_without_model") else None)(
                    rpc(managed_port, {"op": "status", "task_id": refresh_task["task_id"]})),
                timeout=45, label="automatic resource/project memory refresh task")
            assert refresh_final["status"] == "completed", refresh_final
            refreshed_log = [json.loads(line) for line in request_log.read_text(encoding="utf-8").splitlines()]
            assert any(item.get("tag") == "agent" and item.get("auto_resource_refresh_v2_in_context") is True and
                       item.get("auto_project_refresh_v2_in_context") is True for item in refreshed_log), refreshed_log[-12:]
            with sqlite3.connect(resource_db) as connection:
                refreshed_content = connection.execute(
                    "SELECT content FROM resources WHERE category='knowledge' AND relative_path='late-added-after-boot.md'").fetchone()
            assert refreshed_content and resource_refresh_marker in refreshed_content[0], refreshed_content
            with sqlite3.connect(project_db) as connection:
                refreshed_project_content = connection.execute(
                    "SELECT content_text FROM file_contents WHERE file_id=(SELECT file_id FROM files WHERE relative_path='memory-docs/project-memory-proof.md')").fetchone()
            assert refreshed_project_content and project_refresh_marker in refreshed_project_content[0], refreshed_project_content

            # A trivial prompt should use the cheapest available execution model and
            # must not start or invoke the configured large analysis model.
            log_count_before_trivial = len(request_log.read_text(encoding="utf-8").splitlines())
            trivial = rpc(managed_port, {"op": "submit", "task": "Say hello briefly."})
            assert trivial["ok"], trivial
            trivial_result = wait_for(lambda: (lambda current: current if current.get("status") in ("completed", "failed", "cancelled", "completed_without_model") else None)(
                                      rpc(managed_port, {"op": "status", "task_id": trivial["task_id"]})), timeout=25,
                                      label="trivial task classification")
            assert trivial_result["status"] == "completed", trivial_result
            trivial_events = rpc(managed_port, {"op": "events", "task_id": trivial["task_id"], "limit": 500})["events"]
            trivial_tier_event = next((event for event in trivial_events if event["type"] == "task_tier_selected"), None)
            assert trivial_tier_event and trivial_tier_event.get("payload", {}).get("tier") == "fast", trivial_tier_event
            logged_after_trivial = [json.loads(line) for line in request_log.read_text(encoding="utf-8").splitlines()]
            assert not any(item["tag"] == "analysis" for item in logged_after_trivial[log_count_before_trivial:]), \
                logged_after_trivial[log_count_before_trivial:]

            # An agent can pause the task and ask the user, then resume from the current subtask checkpoint.
            print("[integration] managed clarification submit", flush=True)
            asked = rpc(managed_port, {"op": "submit", "task": "AGENT_INPUT_NEEDED: implement with a user-selected strategy"})
            assert asked["ok"], asked
            waiting_agent = wait_for(lambda: (lambda current: current if current.get("status") == "waiting_input" else None)(
                                     rpc(managed_port, {"op": "status", "task_id": asked["task_id"]})), timeout=20,
                                     label="agent requests user input")
            agent_question = waiting_agent["result"]["question"]
            assert "implementation strategy" in agent_question["question"].lower(), waiting_agent
            before_answer_events = rpc(managed_port, {"op": "events", "task_id": asked["task_id"]})["events"]
            assert any(event["type"] == "agent_question" for event in before_answer_events), before_answer_events
            answer_reply = rpc(managed_port, {"op": "answer", "task_id": asked["task_id"],
                                                "option_id": "minimal", "answer": "Smallest safe change"})
            assert answer_reply["ok"], answer_reply
            agent_resumed = wait_for(lambda: (lambda current: current if current.get("status") in ("completed", "failed", "cancelled", "completed_without_model") else None)(
                                     rpc(managed_port, {"op": "status", "task_id": asked["task_id"]})), timeout=20,
                                     label="agent-question task resumes and completes")
            assert agent_resumed["status"] == "completed", agent_resumed
            print("[integration] managed clarification completed", flush=True)
            assert len(agent_resumed["result"]["formalization"]) == 3, agent_resumed["result"].get("formalization")
            assert len(agent_resumed["result"]["agent_results"]) == 3, agent_resumed["result"].get("agent_results")
            completed_events = rpc(managed_port, {"op": "events", "task_id": asked["task_id"]})["events"]
            step_one_starts = [event for event in completed_events if event["type"] == "subtask_started" and "1/3" in event["message"]]
            assert len(step_one_starts) == 1, step_one_starts

            # Authenticated web controls can stop, disable, enable, and restart an individual model.
            ui_headers = {"X-AO-Token": "ui-test-token-with-at-least-32-characters", "Content-Type": "application/json"}
            print("[integration] managed model controls begin", flush=True)
            model_stop = urllib.request.Request(f"http://127.0.0.1:{managed_proc.ui_port}/api/models/llm/managed-agent/stop",
                                                data=b"{}", headers=ui_headers, method="POST")
            with urllib.request.urlopen(model_stop, timeout=5) as response:
                assert json.loads(response.read())["ok"] is True
            stopped = rpc(managed_port, {"op": "health"})["model_status"]["models"]
            assert stopped[0]["running"] is False, stopped
            disable_model = urllib.request.Request(f"http://127.0.0.1:{managed_proc.ui_port}/api/models/llm/managed-agent/disable",
                                                   data=b"{}", headers=ui_headers, method="POST")
            with urllib.request.urlopen(disable_model, timeout=5) as response:
                disable_data = json.loads(response.read())
                assert disable_data["ok"] is True
            disabled_rows = rpc(managed_port, {"op": "health"})["model_status"]["models"]
            assert disabled_rows[0]["enabled"] is False and disabled_rows[0]["running"] is False, disabled_rows
            saved_registry = json.loads((model_dir / "models.json").read_text(encoding="utf-8"))
            assert next(m for m in saved_registry["models"] if m["id"] == "managed-agent")["enabled"] is False
            enable_model = urllib.request.Request(f"http://127.0.0.1:{managed_proc.ui_port}/api/models/llm/managed-agent/enable",
                                                  data=b"{}", headers=ui_headers, method="POST")
            with urllib.request.urlopen(enable_model, timeout=5) as response:
                assert json.loads(response.read())["ok"] is True
            enabled_rows = rpc(managed_port, {"op": "health"})["model_status"]["models"]
            assert enabled_rows[0]["enabled"] is True and enabled_rows[0]["running"] is False, enabled_rows
            print("[integration] managed model start endpoint", flush=True)
            model_start = urllib.request.Request(f"http://127.0.0.1:{managed_proc.ui_port}/api/models/llm/managed-agent/start",
                                                 data=b"{}", headers=ui_headers, method="POST")
            with urllib.request.urlopen(model_start, timeout=10) as response:
                assert json.loads(response.read())["ok"] is True
            restarted = rpc(managed_port, {"op": "health"})["model_status"]["models"]
            assert restarted[0]["running"] is True, restarted
        finally:
            print("[integration] managed shutdown begin", flush=True)
            stop_server(managed_proc)
            print("[integration] managed shutdown done", flush=True)
        for check_port in (agent_port, analysis_port, decision_port):
            try:
                with socket.create_connection(("127.0.0.1", check_port), timeout=0.3):
                    raise AssertionError(f"managed model listener {check_port} still alive after orchestrator shutdown")
            except OSError:
                pass

    print("[integration] web UI APIs and task/chat data", flush=True)
    # Local web UI: static assets public only on loopback, API requires a separate high-entropy token.
    with tempfile.TemporaryDirectory(prefix="ao-c-ui-") as ui_tmp:
        ui_db = Path(ui_tmp) / "ui-tasks.sqlite3"
        ui_proc = start_server(free_port(), ui_db, ui_enabled=True)
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{ui_proc.ui_port}/", timeout=3) as response:
                index_html = response.read()
                assert response.status == 200 and b"ASGEND" in index_html
                assert response.headers.get("Cache-Control") == "no-store", response.headers
                assert b'name="asgend-ui-release" content="2026.10.18.5"' in index_html
                assert b'href="/assets/style.css?v=20261018.5"' in index_html
                assert b'src="/assets/app.js?v=20261018.5"' in index_html
                assert b'WebUI 2026.10.18.5' in index_html
                assert b'class="tab active" data-view-target="chat"' in index_html
                assert b'<div class="view active" id="view-chat">' in index_html
                assert b'<div class="view" id="view-dashboard">' in index_html
                assert b'id="taskChatEvents"' in index_html
                assert b'id="taskWorkspaceGrid"' in index_html and b'id="workspaceFileEditor"' in index_html
            with urllib.request.urlopen(f"http://127.0.0.1:{ui_proc.ui_port}/assets/app.js?v=20261018.5", timeout=3) as response:
                assert response.headers.get("Cache-Control") == "no-store", response.headers
                app_js = response.read().decode("utf-8")
                assert "ao_active_view_v2" in app_js and "||'chat'" in app_js and "loadPlan()" in app_js
                assert "async function showDetails" in app_js and "activateView('chat',false)" in app_js
                assert "loadWorkspaceFiles" in app_js and "workspaceRequest('write'" in app_js
                assert "confirmWorkspaceDiscard" in app_js and "Отбросить их" in app_js
                assert "workspaceSaving" in app_js and "event.returnValue=''" in app_js
                assert "JSON.stringify(event.payload" not in app_js
            with urllib.request.urlopen(f"http://127.0.0.1:{ui_proc.ui_port}/assets/style.css?v=20261018.5", timeout=3) as response:
                assert response.headers.get("Cache-Control") == "no-store", response.headers
                style_css = response.read().decode("utf-8")
                assert ".chat-bubble" in style_css and ".chat-activity" in style_css
                assert ".ui-build" in style_css
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{ui_proc.ui_port}/api/health", timeout=3)
                raise AssertionError("UI API must require a token")
            except urllib.error.HTTPError as exc:
                assert exc.code == 401
            token = "ui-test-token-with-at-least-32-characters"
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/health", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                health_data = json.loads(response.read())
                assert health_data["ok"] is True
            headers = {"X-AO-Token": token, "Content-Type": "application/json"}
            plan_url = f"http://127.0.0.1:{ui_proc.ui_port}/api/plan"
            plan_title = "HTTP_PLAN_TODO_SMOKE"
            request = urllib.request.Request(plan_url + "?scope_id=global", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                plan_listing = json.loads(response.read())
            assert plan_listing.get("ok") and isinstance(plan_listing.get("items"), list), plan_listing
            request = urllib.request.Request(plan_url, data=json.dumps({"action":"add","scope_id":"global","kind":"todo","title":plan_title}).encode(),
                                             headers=headers, method="POST")
            with urllib.request.urlopen(request, timeout=3) as response:
                plan_added = json.loads(response.read())
            assert plan_added.get("ok") and isinstance(plan_added.get("item_id"), int), plan_added
            request = urllib.request.Request(plan_url, data=json.dumps({"action":"update","scope_id":"global","item_id":plan_added["item_id"],"status":"done"}).encode(),
                                             headers=headers, method="POST")
            with urllib.request.urlopen(request, timeout=3) as response:
                assert json.loads(response.read()).get("ok") is True
            request = urllib.request.Request(plan_url + "?scope_id=global&kind=todo", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                plan_listing = json.loads(response.read())
            assert any(item.get("item_id") == plan_added["item_id"] and item.get("status") == "done" for item in plan_listing.get("items", [])), plan_listing
            request = urllib.request.Request(plan_url, data=json.dumps({"action":"delete","scope_id":"global","item_id":plan_added["item_id"]}).encode(),
                                             headers=headers, method="POST")
            with urllib.request.urlopen(request, timeout=3) as response:
                assert json.loads(response.read()).get("ok") is True

            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/configs", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                configs = json.loads(response.read())
            assert configs["ok"] and len(configs["agents"]) >= 1
            assert configs["restart_required"] is False, configs
            assert len(configs["models"]["models"]) >= 2 and len(configs["models"]["decision_models"]) >= 1
            updated = {"main": configs["main"], "models": configs["models"], "agents": configs["agents"]}
            updated["agents"].append({"id":"ui-created-agent", "name":"UI created agent", "description":"created by integration test",
                                       "system_prompt":"You are an integration-test agent.", "model_id":"ollama-general",
                                       "skills":[], "tools":[]})
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/configs", data=json.dumps(updated).encode(),
                                             headers=headers, method="PUT")
            with urllib.request.urlopen(request, timeout=5) as response:
                save_result = json.loads(response.read())
            assert save_result["ok"] and save_result["restart_required"] is True
            assert (ui_db.parent / "agents" / "ui-created-agent.json").exists()
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/configs", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                persisted = json.loads(response.read())
            assert any(agent["id"] == "ui-created-agent" and agent["model_id"] == "ollama-general" for agent in persisted["agents"])
            assert persisted["restart_required"] is True, persisted
            invalid = copy.deepcopy({"main": persisted["main"], "models": persisted["models"], "agents": persisted["agents"]})
            invalid["models"]["defaults"]["model_id"] = "model-does-not-exist"
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/configs", data=json.dumps(invalid).encode(),
                                             headers=headers, method="PUT")
            try:
                urllib.request.urlopen(request, timeout=5)
                raise AssertionError("invalid model reference must be rejected before writing config")
            except urllib.error.HTTPError as exc:
                assert exc.code == 400
            # Internet nodes must use HTTPS; reject an insecure endpoint before it reaches disk.
            bad_transport = copy.deepcopy({"main": persisted["main"], "models": persisted["models"], "agents": persisted["agents"]})
            internet_node = next(node for node in bad_transport["models"]["nodes"] if node["id"] == "internet-node")
            internet_node["endpoint"] = "http://llm.example.com/v1"
            internet_node["health_url"] = "http://llm.example.com/v1/models"
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/configs", data=json.dumps(bad_transport).encode(),
                                             headers=headers, method="PUT")
            try:
                urllib.request.urlopen(request, timeout=5)
                raise AssertionError("internet node must reject unencrypted HTTP")
            except urllib.error.HTTPError as exc:
                assert exc.code == 400
            # A model profile cannot be enabled when its configured Node is disabled.
            blocked_node = copy.deepcopy({"main": persisted["main"], "models": persisted["models"], "agents": persisted["agents"]})
            qwen_profile = next(model for model in blocked_node["models"]["models"] if model["id"] == "qwen-coder")
            qwen_profile["node_id"] = "lan-node"
            qwen_profile["enabled"] = False
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/configs", data=json.dumps(blocked_node).encode(),
                                             headers=headers, method="PUT")
            try:
                with urllib.request.urlopen(request, timeout=5) as response:
                    assert json.loads(response.read())["ok"] is True
            except urllib.error.HTTPError as exc:
                raise AssertionError(f"disabled-node profile should be saved while remaining disabled: {exc.read().decode()}") from exc
            enable_blocked = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/models/llm/qwen-coder/enable",
                                                    data=b"{}", headers=headers, method="POST")
            try:
                urllib.request.urlopen(enable_blocked, timeout=5)
                raise AssertionError("model must not be enabled on a disabled Node")
            except urllib.error.HTTPError as exc:
                assert exc.code == 400
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/models", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                model_status = json.loads(response.read())
            assert model_status["ok"] and len(model_status["models"]) >= 2 and model_status["decision_models"]
            node_ids = {node["id"] for node in model_status.get("nodes", [])}
            assert {"local", "localhost", "lan-node", "internet-node"}.issubset(node_ids), model_status.get("nodes")
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/resources", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                resource_status = json.loads(response.read())
            assert resource_status["ok"] and resource_status["classification_parallel"] is False
            assert resource_status["selection_mode"] == "local_bm25" and resource_status["resource_index_auto_sync"] is True
            assert resource_status["decision_model_id"] == "liquid-d1-3b", resource_status
            assert "coding/SKILL.md" in resource_status["files"]["skills"]
            assert "coding_style.md" in resource_status["files"]["system_prompts"]
            assert "echo.md" in resource_status["files"]["tools"]
            assert len(resource_status["files"]["instructions"]) == 4, resource_status["files"].get("instructions")
            payload = json.dumps({"task":"Submitted from browser UI", "priority":8, "idempotency_key":"ui-task-1"}).encode()
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/tasks", data=payload, headers=headers, method="POST")
            with urllib.request.urlopen(request, timeout=3) as response:
                accepted = json.loads(response.read())
                assert accepted["ok"] and accepted["priority"] == 8
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/tasks", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                task_list = json.loads(response.read())
                assert any(row["task_id"] == accepted["task_id"] and row["priority"] == 8 for row in task_list["tasks"])
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/tasks/{accepted['task_id']}", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                detail = json.loads(response.read())
                assert detail["ok"] and detail["priority"] == 8 and detail.get("title")
            files_url = f"http://127.0.0.1:{ui_proc.ui_port}/api/tasks/{accepted['task_id']}/files"
            file_write_request = urllib.request.Request(files_url, data=json.dumps({"action":"write","path":"from-ui.txt","content":"editor API"}).encode(),
                                                       headers=headers, method="POST")
            with urllib.request.urlopen(file_write_request, timeout=3) as response:
                file_saved = json.loads(response.read())
                assert file_saved["ok"] and file_saved["path"] == "from-ui.txt", file_saved
            file_read_request = urllib.request.Request(files_url, data=json.dumps({"action":"read","path":"from-ui.txt"}).encode(),
                                                      headers=headers, method="POST")
            with urllib.request.urlopen(file_read_request, timeout=3) as response:
                file_loaded = json.loads(response.read())
                assert file_loaded["ok"] and file_loaded["content"] == "editor API", file_loaded
            file_list_request = urllib.request.Request(files_url, data=json.dumps({"action":"list","path":""}).encode(),
                                                      headers=headers, method="POST")
            with urllib.request.urlopen(file_list_request, timeout=3) as response:
                file_listing = json.loads(response.read())
                assert file_listing["ok"] and any(item["name"] == "from-ui.txt" for item in file_listing["entries"]), file_listing
            another_write = urllib.request.Request(files_url, data=json.dumps({"action":"write","path":"another.txt","content":"second file"}).encode(),
                                                  headers=headers, method="POST")
            with urllib.request.urlopen(another_write, timeout=3) as response:
                assert json.loads(response.read())["ok"] is True

            second_payload = json.dumps({"task":"Second task for real-browser editor navigation", "priority":5,
                                          "idempotency_key":"ui-task-switch-2"}).encode()
            second_request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/tasks", data=second_payload,
                                                    headers=headers, method="POST")
            with urllib.request.urlopen(second_request, timeout=3) as response:
                second_accepted = json.loads(response.read())
            second_files_url = f"http://127.0.0.1:{ui_proc.ui_port}/api/tasks/{second_accepted['task_id']}/files"
            second_file_write = urllib.request.Request(second_files_url, data=json.dumps({"action":"write","path":"second.txt","content":"second task file"}).encode(),
                                                       headers=headers, method="POST")
            with urllib.request.urlopen(second_file_write, timeout=3) as response:
                assert json.loads(response.read())["ok"] is True

            try:
                asyncio.run(run_browser_editor_regression(ui_proc.ui_port, token, accepted["task_id"], second_accepted["task_id"]))
            except ImportError as exc:
                print(f"[integration] browser editor regression skipped (Playwright unavailable: {exc})", flush=True)

            # DNS-rebinding guard: Host must be localhost/127.0.0.1 for the configured port.
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/health", headers={"X-AO-Token": token, "Host":"attacker.example"})
            try:
                urllib.request.urlopen(request, timeout=3)
                raise AssertionError("UI should reject a hostile Host header")
            except urllib.error.HTTPError as exc:
                assert exc.code == 400

            # Restart from the edited files: the added agent is loaded and the disk/runtime fingerprint converges.
            ui_port = ui_proc.ui_port
            ui_config_path = ui_db.parent / "config.json"
            stop_server(ui_proc)
            ui_proc = subprocess.Popen([str(BINARY), "-c", str(ui_config_path)], stdout=subprocess.DEVNULL,
                                       stderr=subprocess.PIPE, text=True, start_new_session=True)
            ui_proc.ui_port = ui_port
            restart_port = int(json.loads(ui_config_path.read_text())["server"]["port"])
            wait_for(lambda: rpc(restart_port, {"op":"ping"}) if ui_proc.poll() is None else None,
                     timeout=8, label="restart after config editor save")
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/configs", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                after_restart = json.loads(response.read())
            assert after_restart["restart_required"] is False, after_restart
            assert any(agent["id"] == "ui-created-agent" for agent in after_restart["agents"]), after_restart["agents"]
            request = urllib.request.Request(f"http://127.0.0.1:{ui_proc.ui_port}/api/agents", headers={"X-AO-Token": token})
            with urllib.request.urlopen(request, timeout=3) as response:
                active_agents = json.loads(response.read())
            assert any(agent["id"] == "ui-created-agent" for agent in active_agents["agents"]), active_agents
        finally:
            stop_server(ui_proc)

    print("[integration] strict System One local/LAN paths, error handling and active cancellation", flush=True)
    run_systemone_case(BINARY, PROJECT, via_lan=False, error_mode=None)
    run_systemone_case(BINARY, PROJECT, via_lan=True, error_mode=None)
    for failure_mode in ("bad_contract", "invalid_json", "http_404", "http_500", "timeout"):
        run_systemone_case(BINARY, PROJECT, via_lan=False, error_mode=failure_mode)
    run_systemone_case(BINARY, PROJECT, via_lan=False, cancel_during_request=True)
    print("integration tests passed: config/auth/strict JSON, C CLI, secure Unix socket, task lifecycle/queue, model-assisted decomposition, System One local/LAN contracts and independent failure modes, automatic incremental resource/project memory sync, local BM25 resource selection without per-category decision calls, model lifecycle, child shutdown, MCP, session context, node/resource UI APIs, UI config validation/persistence and restart convergence")


if __name__ == "__main__":
    main()
