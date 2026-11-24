#!/usr/bin/env python3
"""Repeatable local load benchmark for Core RPC/task lifecycle.

Default run: 5 independent server/database runs, 20 samples per method/run,
clients 1/4/16. Samples are aggregated across runs (100 per method/concurrency
by default) so p95/p99 are only reported when the shared metrics policy allows it.

The target is an isolated local Core process with a deterministic local mock LLM.
Results do not certify production/provider latency or remote network capacity.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import os
import platform
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter, defaultdict
from datetime import datetime, timezone
from http.server import ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable

from performance_metrics import summarize_latency

TERMINAL = {"completed", "completed_without_model", "failed", "cancelled"}
TOKEN = "core-rpc-performance-benchmark-local-token"
METHODS = ("ping", "submit", "status", "list_tasks", "events", "answer", "cancel", "retry_task", "submit_to_terminal")


def load_integration(binary: Path, project: Path):
    integration_path = project / "tests" / "integration.py"
    saved_argv = sys.argv[:]
    try:
        sys.argv = [str(integration_path), str(binary), str(project)]
        spec = importlib.util.spec_from_file_location("asgend_core_rpc_performance_integration", integration_path)
        if spec is None or spec.loader is None:
            raise RuntimeError("cannot load integration helpers")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.argv = saved_argv


def require(condition: bool, message: str, value: Any = None) -> None:
    if not condition:
        raise RuntimeError(f"{message}: {value!r}")


def read_text_if_exists(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError):
        return None


def command_first_line(command: list[str]) -> str | None:
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=3, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    output = (completed.stdout or completed.stderr).strip().splitlines()
    return output[0].strip() if completed.returncode == 0 and output else None


def read_cmake_build_metadata(binary: Path) -> dict[str, str | None]:
    cache = binary.parent / "CMakeCache.txt"
    result: dict[str, str | None] = {"build_type": None, "c_compiler": None, "c_compiler_version": None,
                                     "cmake_version": None, "compiler_flags_release": None}
    if not cache.is_file():
        return result
    wanted = {
        "CMAKE_BUILD_TYPE": "build_type",
        "CMAKE_C_COMPILER": "c_compiler",
        "CMAKE_C_COMPILER_VERSION": "c_compiler_version",
        "CMAKE_VERSION": "cmake_version",
        "CMAKE_C_FLAGS_RELEASE": "compiler_flags_release",
    }
    try:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("//") or line.startswith("#") or "=" not in line:
                continue
            left, value = line.split("=", 1)
            key = left.split(":", 1)[0]
            if key in wanted:
                result[wanted[key]] = value
    except OSError:
        pass
    if result["c_compiler"]:
        result["c_compiler_identity"] = command_first_line([str(result["c_compiler"]), "--version"])
    cmake_executable = shutil.which("cmake")
    result["cmake_identity"] = command_first_line([cmake_executable, "--version"]) if cmake_executable else None
    return result


def host_memory_total_bytes() -> int | None:
    text = read_text_if_exists(Path("/proc/meminfo"))
    if not text:
        return None
    for line in text.splitlines():
        if line.startswith("MemTotal:"):
            parts = line.split()
            if len(parts) >= 2 and parts[1].isdigit():
                return int(parts[1]) * 1024
    return None


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rpc_once(integration, port: int, payload: dict[str, Any], timeout: float = 5.0) -> dict[str, Any]:
    result = integration.rpc(port, payload, token=TOKEN, timeout=timeout)
    if not isinstance(result, dict) or not isinstance(result.get("ok"), bool):
        raise RuntimeError(f"malformed RPC response for op={payload.get('op')}")
    return result


def timed_batch(integration, port: int, op: str, payloads: list[dict[str, Any]], clients: int,
                *, expected: Callable[[dict[str, Any]], bool] | None = None) -> dict[str, Any]:
    """Run an operation batch and time each complete TCP request independently."""
    def one(payload: dict[str, Any]) -> dict[str, Any]:
        started = time.perf_counter()
        try:
            response = rpc_once(integration, port, payload)
            latency = (time.perf_counter() - started) * 1000.0
            ok = response.get("ok") is True and (expected(response) if expected else True)
            return {"latency_ms": latency, "ok": ok,
                    "error_kind": None if ok else "rpc_rejected_or_shape_mismatch"}
        except Exception as exc:  # capture type only, never request/token/response content
            return {"latency_ms": (time.perf_counter() - started) * 1000.0,
                    "ok": False, "error_kind": type(exc).__name__}

    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(clients, len(payloads) or 1)) as pool:
        samples = list(pool.map(one, payloads))
    elapsed = max(time.perf_counter() - started, 1e-9)
    latencies = [sample["latency_ms"] for sample in samples]
    failures = Counter(sample["error_kind"] for sample in samples if not sample["ok"])
    return {
        "operation": op,
        "requests": len(samples),
        "elapsed_seconds": round(elapsed, 6),
        "requests_per_second": round(len(samples) / elapsed, 3),
        "error_count": sum(failures.values()),
        "error_rate": round(sum(failures.values()) / len(samples), 6) if samples else 0.0,
        "error_types": dict(sorted(failures.items())),
        "latency_ms": latencies,
        "sample_ok": [sample["ok"] for sample in samples],
    }


def wait_task(integration, port: int, task_id: str, *, acceptable: set[str], timeout: float = 20.0) -> dict[str, Any]:
    def lookup():
        response = rpc_once(integration, port, {"op": "status", "task_id": task_id}, timeout=4.0)
        if response.get("ok") and response.get("status") in acceptable:
            return response
        return None
    return integration.wait_for(lookup, timeout=timeout, label=f"task {task_id} state in {sorted(acceptable)}")


def timed_batch_with_replies(integration, port: int, op: str, payloads: list[dict[str, Any]], clients: int,
                             *, expected: Callable[[dict[str, Any]], bool] | None = None) -> dict[str, Any]:
    def one(payload: dict[str, Any]) -> dict[str, Any]:
        started = time.perf_counter()
        try:
            response = rpc_once(integration, port, payload)
            latency = (time.perf_counter() - started) * 1000.0
            ok = response.get("ok") is True and (expected(response) if expected else True)
            return {"latency_ms": latency, "ok": ok,
                    "error_kind": None if ok else "rpc_rejected_or_shape_mismatch", "reply": response}
        except Exception as exc:
            return {"latency_ms": (time.perf_counter() - started) * 1000.0,
                    "ok": False, "error_kind": type(exc).__name__, "reply": {}}

    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(clients, len(payloads) or 1)) as pool:
        samples = list(pool.map(one, payloads))
    elapsed = max(time.perf_counter() - started, 1e-9)
    failures = Counter(sample["error_kind"] for sample in samples if not sample["ok"])
    return {
        "operation": op,
        "requests": len(samples),
        "elapsed_seconds": round(elapsed, 6),
        "requests_per_second": round(len(samples) / elapsed, 3),
        "error_count": sum(failures.values()),
        "error_rate": round(sum(failures.values()) / len(samples), 6) if samples else 0.0,
        "error_types": dict(sorted(failures.items())),
        "latency_ms": [sample["latency_ms"] for sample in samples],
        "sample_ok": [sample["ok"] for sample in samples],
        "replies": [sample["reply"] for sample in samples],
    }


def make_prompts(prefix: str, count: int, *, marker: str = "") -> list[str]:
    return [f"Core RPC performance workload {prefix} item {index} {marker}".strip() for index in range(count)]


def wait_all(integration, port: int, task_ids: list[str], *, states: set[str], timeout_each: float = 25.0) -> list[dict[str, Any]]:
    # Poll tasks concurrently: serial waits would turn N independent completions
    # into an artificial N * task-latency wall time in the benchmark harness.
    if not task_ids:
        return []
    def wait_one(task_id: str) -> dict[str, Any]:
        return wait_task(integration, port, task_id, acceptable=states, timeout=timeout_each)
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(16, len(task_ids))) as pool:
        return list(pool.map(wait_one, task_ids))


def run_one(integration, binary: Path, project: Path, *, run_index: int, clients: int,
            samples_per_run: int, terminal_timeout: float) -> dict[str, Any]:
    run_started_utc = datetime.now(timezone.utc).isoformat()
    with tempfile.TemporaryDirectory(prefix=f"asgend-rpc-perf-r{run_index}-c{clients}-") as temp:
        root = Path(temp)
        db_path = root / "core-rpc-benchmark.sqlite3"
        llm_port = integration.free_port()
        class QuietMockLLMHandler(integration.MockLLMHandler):
            # Cancellation intentionally closes upstream sockets; suppress the
            # standard-library traceback for these expected transport resets.
            def handle(self):
                try:
                    super().handle()
                except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
                    return

        QuietMockLLMHandler.delay_seconds = 0.0
        QuietMockLLMHandler.keepalive_after_done_seconds = 0.001
        llm_server = ThreadingHTTPServer(("127.0.0.1", llm_port), QuietMockLLMHandler)
        llm_server.daemon_threads = True
        llm_thread = threading.Thread(target=llm_server.serve_forever, daemon=True)
        llm_thread.start()
        proc = None
        operations: list[dict[str, Any]] = []
        try:
            proc = integration.start_server(
                integration.free_port(), db_path, token=TOKEN,
                llm_enabled=True, llm_base_url=f"http://127.0.0.1:{llm_port}/v1",
                llm_timeout_seconds=8, sandbox_enabled=False, runtime_workers=clients,
            )
            port = proc.test_port
            batch_prefix = f"rpc-perf-{run_index}-c{clients}"

            ping = timed_batch(integration, port, "ping", [{"op": "ping"} for _ in range(samples_per_run)], clients,
                               expected=lambda r: isinstance(r.get("version"), str))
            operations.append(ping)

            submit_payloads = [{"op": "submit", "task": prompt,
                                "idempotency_key": f"{batch_prefix}-simple-{i}"}
                               for i, prompt in enumerate(make_prompts(batch_prefix + " simple", samples_per_run))]
            submitted = timed_batch_with_replies(
                integration, port, "submit", submit_payloads, clients,
                expected=lambda r: r.get("accepted") is True and isinstance(r.get("task_id"), str))
            operations.append(submitted)
            task_ids = [reply.get("task_id") for reply, ok in zip(submitted["replies"], submitted["sample_ok"])
                        if ok and isinstance(reply.get("task_id"), str)]
            require(len(task_ids) == samples_per_run, "submit returned fewer IDs than requested", len(task_ids))

            status = timed_batch(integration, port, "status", [{"op": "status", "task_id": task_id} for task_id in task_ids], clients,
                                 expected=lambda r: isinstance(r.get("status"), str))
            operations.append(status)
            listing = timed_batch(integration, port, "list_tasks", [{"op": "list_tasks", "limit": 100} for _ in range(samples_per_run)], clients,
                                  expected=lambda r: isinstance(r.get("tasks"), list))
            operations.append(listing)
            events = timed_batch(integration, port, "events", [{"op": "events", "task_id": task_id, "limit": 100} for task_id in task_ids], clients,
                                 expected=lambda r: isinstance(r.get("events"), list))
            operations.append(events)

            print(f"[core-rpc-benchmark] c={clients} run={run_index}: basic RPC complete", file=sys.stderr, flush=True)

            # Measure submit-to-terminal independently of request latency. Each
            # worker owns one task at a time, so outstanding task count is bounded
            # by the requested concurrency level.
            def submit_until_terminal(index: int) -> dict[str, Any]:
                started = time.perf_counter()
                task_prompt = f"{batch_prefix} terminal workload {index}"
                reply = rpc_once(integration, port, {"op": "submit", "task": task_prompt,
                                                     "idempotency_key": f"{batch_prefix}-terminal-{index}"}, timeout=5.0)
                require(reply.get("accepted") is True and isinstance(reply.get("task_id"), str), "terminal submit failed", reply)
                terminal = wait_task(integration, port, reply["task_id"], acceptable=TERMINAL,
                                     timeout=terminal_timeout)
                latency = (time.perf_counter() - started) * 1000.0
                success = terminal.get("status") in {"completed", "completed_without_model"}
                return {"latency_ms": latency, "ok": success, "status": terminal.get("status")}

            terminal_started = time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
                terminal_samples = list(pool.map(submit_until_terminal, range(samples_per_run)))
            terminal_elapsed = max(time.perf_counter() - terminal_started, 1e-9)
            terminal_failures = Counter(str(row.get("status")) for row in terminal_samples if not row["ok"])
            operations.append({
                "operation": "submit_to_terminal", "requests": len(terminal_samples),
                "elapsed_seconds": round(terminal_elapsed, 6),
                "requests_per_second": round(len(terminal_samples) / terminal_elapsed, 3),
                "error_count": sum(terminal_failures.values()),
                "error_rate": round(sum(terminal_failures.values()) / len(terminal_samples), 6),
                "error_types": dict(sorted(terminal_failures.items())),
                "latency_ms": [row["latency_ms"] for row in terminal_samples],
                "sample_ok": [row["ok"] for row in terminal_samples],
                "terminal_status_counts": dict(Counter(str(row.get("status")) for row in terminal_samples)),
            })

            print(f"[core-rpc-benchmark] c={clients} run={run_index}: submit-to-terminal complete", file=sys.stderr, flush=True)

            # Prepare deterministic waiting_input tasks, then time the answer RPC
            # itself. The post-answer executions are awaited before the next phase.
            answer_payloads = [{"op": "submit", "task": prompt,
                                "idempotency_key": f"{batch_prefix}-answer-{i}"}
                               for i, prompt in enumerate(make_prompts(batch_prefix + " answer", samples_per_run,
                                                                       marker="NEEDS_CLARIFICATION:"))]
            answer_setup = timed_batch_with_replies(integration, port, "answer_setup", answer_payloads, clients,
                                                    expected=lambda r: r.get("accepted") is True and isinstance(r.get("task_id"), str))
            require(answer_setup["error_count"] == 0, "could not submit answer precondition tasks", answer_setup["error_types"])
            answer_ids = [reply["task_id"] for reply in answer_setup["replies"]]
            wait_all(integration, port, answer_ids, states={"waiting_input"}, timeout_each=25.0)
            answers = timed_batch(integration, port, "answer", [{"op": "answer", "task_id": task_id,
                                                                    "option_id": "local", "answer": "local deterministic benchmark answer"}
                                                                   for task_id in answer_ids], clients,
                                  expected=lambda r: r.get("status") == "queued")
            operations.append(answers)
            require(answers["error_count"] == 0, "answer RPC rejected a prepared waiting_input task", answers["error_types"])
            answered_terminal = wait_all(integration, port, answer_ids, states=TERMINAL, timeout_each=terminal_timeout)
            print(f"[core-rpc-benchmark] c={clients} run={run_index}: answer lifecycle complete", file=sys.stderr, flush=True)
            require(all(row.get("status") in {"completed", "completed_without_model"} for row in answered_terminal),
                    "answered tasks did not complete successfully", Counter(str(row.get("status")) for row in answered_terminal))

            # The mock holds cancellation-marked SSE requests open. Tasks beyond
            # the worker count remain queued; the batch deliberately includes both
            # active and queued cancellation requests as happens in real traffic.
            cancel_payloads = [{"op": "submit", "task": prompt,
                                "idempotency_key": f"{batch_prefix}-cancel-{i}"}
                               for i, prompt in enumerate(make_prompts(batch_prefix + " cancel", samples_per_run,
                                                                       marker="MCP_PROTOCOL_TOOL_TEST __SSE_CANCEL_STREAM__"))]
            cancel_setup = timed_batch_with_replies(integration, port, "cancel_setup", cancel_payloads, clients,
                                                    expected=lambda r: r.get("accepted") is True and isinstance(r.get("task_id"), str))
            require(cancel_setup["error_count"] == 0, "could not submit cancellation precondition tasks", cancel_setup["error_types"])
            cancel_ids = [reply["task_id"] for reply in cancel_setup["replies"]]
            # Wait until at least one cancellation-marker task is active when the
            # mock server is blocked, otherwise cancellation would only benchmark
            # queue deletion and not active-task signalling.
            if cancel_ids:
                def active_cancel_fixture():
                    for task_id in cancel_ids[:min(clients, len(cancel_ids))]:
                        ev = rpc_once(integration, port, {"op": "events", "task_id": task_id, "limit": 100}, timeout=4.0)
                        if any(isinstance(row, dict) and row.get("type") == "llm_status" and
                               row.get("payload", {}).get("status") == "streaming" for row in ev.get("events", [])):
                            return True
                    return False
                integration.wait_for(active_cancel_fixture, timeout=10.0, label="active cancellation fixture")
            cancel_batch = timed_batch(integration, port, "cancel", [{"op": "cancel", "task_id": task_id} for task_id in cancel_ids], clients,
                                       expected=lambda r: r.get("status") == "cancellation_requested")
            operations.append(cancel_batch)
            require(cancel_batch["error_count"] == 0, "cancel RPC rejected a prepared task", cancel_batch["error_types"])
            cancelled_terminal = wait_all(integration, port, cancel_ids, states=TERMINAL, timeout_each=12.0)
            print(f"[core-rpc-benchmark] c={clients} run={run_index}: cancel lifecycle complete", file=sys.stderr, flush=True)
            require(all(row.get("status") == "cancelled" for row in cancelled_terminal),
                    "cancelled workload did not end in cancelled state", Counter(str(row.get("status")) for row in cancelled_terminal))

            # Seed disposable dead-letter rows so retry_task timing is not mixed
            # with retries' terminal latency. Retry operations are still sent over
            # the real TCP listener, against the server's own database.
            retry_ids = [f"rpcperf-r{run_index}-c{clients}-retry-{i}" for i in range(samples_per_run)]
            with sqlite3.connect(db_path) as connection:
                for index, task_id in enumerate(retry_ids):
                    request_json = json.dumps({"op": "submit", "task": f"{batch_prefix} retry {index}"}, separators=(",", ":"))
                    connection.execute(
                        "INSERT INTO tasks(id, task_json, status, attempts, max_attempts, priority, error) "
                        "VALUES(?, ?, 'failed', 2, 2, 5, ?)",
                        (task_id, request_json, "deterministic performance benchmark dead-letter fixture"),
                    )
            retry = timed_batch(integration, port, "retry_task", [{"op": "retry_task", "task_id": task_id} for task_id in retry_ids], clients,
                                expected=lambda r: r.get("status") == "queued")
            operations.append(retry)
            require(retry["error_count"] == 0, "retry_task RPC rejected a prepared dead-letter", retry["error_types"])
            retried_terminal = wait_all(integration, port, retry_ids, states=TERMINAL, timeout_each=terminal_timeout)
            print(f"[core-rpc-benchmark] c={clients} run={run_index}: retry lifecycle complete", file=sys.stderr, flush=True)
            require(all(row.get("status") in {"completed", "completed_without_model"} for row in retried_terminal),
                    "retried tasks did not complete successfully", Counter(str(row.get("status")) for row in retried_terminal))

            for operation in operations:
                # Do not retain task IDs or payload-bearing RPC responses in reports.
                operation.pop("replies", None)
                operation["run_index"] = run_index
                operation["concurrency"] = clients
            return {"run_index": run_index, "concurrency": clients, "samples_per_method": samples_per_run,
                    "run_started_utc": run_started_utc, "run_completed_utc": datetime.now(timezone.utc).isoformat(),
                    "operations": operations}
        finally:
            if proc is not None:
                integration.stop_server(proc)
            llm_server.shutdown()
            llm_server.server_close()
            llm_thread.join(timeout=2)


def parse_concurrencies(value: str) -> list[int]:
    try:
        levels = [int(part.strip()) for part in value.split(",") if part.strip()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("concurrency must be comma-separated positive integers") from exc
    if not levels or len(set(levels)) != len(levels) or any(n < 1 or n > 16 for n in levels):
        raise argparse.ArgumentTypeError("concurrency levels must be unique integers in [1, 16]")
    return levels


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to built agent-orchestrator")
    parser.add_argument("project", type=Path, help="project source root")
    parser.add_argument("--runs", type=int, default=5, help="independent server/database runs per concurrency (default: 5)")
    parser.add_argument("--samples-per-run", type=int, default=20,
                        help="samples per method/run; default 5x20 yields 100 samples per method/concurrency")
    parser.add_argument("--concurrencies", type=parse_concurrencies, default=parse_concurrencies("1,4,16"))
    parser.add_argument("--terminal-timeout", type=float, default=25.0)
    parser.add_argument("--output", type=Path, help="write full JSON report to this path")
    args = parser.parse_args(argv)
    binary, project = args.binary.resolve(), args.project.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"Core binary is not executable: {binary}")
    if not (project / "tests" / "integration.py").is_file():
        parser.error(f"project root is missing tests/integration.py: {project}")
    if isinstance(args.runs, bool) or not 1 <= args.runs <= 20:
        parser.error("--runs must be in [1, 20]")
    if isinstance(args.samples_per_run, bool) or not 1 <= args.samples_per_run <= 100:
        parser.error("--samples-per-run must be in [1, 100]")
    if not 1.0 <= args.terminal_timeout <= 120.0:
        parser.error("--terminal-timeout must be in [1, 120] seconds")

    integration = load_integration(binary, project)
    start_utc = datetime.now(timezone.utc).isoformat()
    runs: list[dict[str, Any]] = []
    try:
        for concurrency in args.concurrencies:
            for run_index in range(1, args.runs + 1):
                print(f"[core-rpc-benchmark] concurrency={concurrency} run={run_index}/{args.runs}",
                      file=sys.stderr, flush=True)
                runs.append(run_one(integration, binary, project, run_index=run_index,
                                    clients=concurrency, samples_per_run=args.samples_per_run,
                                    terminal_timeout=args.terminal_timeout))
    except Exception as exc:
        print(f"core-rpc-benchmark failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    all_samples: dict[tuple[int, str], list[float]] = defaultdict(list)
    all_ok: dict[tuple[int, str], list[bool]] = defaultdict(list)
    durations: dict[tuple[int, str], list[float]] = defaultdict(list)
    errors: dict[tuple[int, str], Counter[str]] = defaultdict(Counter)
    raw: list[dict[str, Any]] = []
    for run in runs:
        for operation in run["operations"]:
            key = (run["concurrency"], operation["operation"])
            all_samples[key].extend(operation["latency_ms"])
            all_ok[key].extend(operation["sample_ok"])
            durations[key].append(operation["elapsed_seconds"])
            errors[key].update(operation.get("error_types", {}))
            for latency, ok in zip(operation["latency_ms"], operation["sample_ok"]):
                raw.append({"run_index": run["run_index"], "concurrency": run["concurrency"],
                            "operation": operation["operation"], "latency_ms": round(latency, 4), "ok": ok})

    result_by_concurrency: dict[str, Any] = {}
    total_error_count = 0
    for concurrency in args.concurrencies:
        method_rows: dict[str, Any] = {}
        for method in METHODS:
            key = (concurrency, method)
            values = all_samples.get(key, [])
            if not values:
                continue
            ok = all_ok[key]
            fails = len(ok) - sum(ok)
            total_error_count += fails
            total_elapsed = sum(durations[key])
            method_rows[method] = {
                "samples": len(values),
                "runs": args.runs,
                "samples_per_run": args.samples_per_run,
                "latency_ms": summarize_latency(values, decimals=3),
                "requests_per_second_aggregate": round(len(values) / total_elapsed, 3) if total_elapsed > 0 else 0.0,
                "error_count": fails,
                "error_rate": round(fails / len(values), 6),
                "error_types": dict(sorted(errors[key].items())),
                "quality_note": "local TCP Core RPC with deterministic local mock LLM; not production/network/provider latency",
            }
        result_by_concurrency[str(concurrency)] = method_rows

    report = {
        "schema_version": 1,
        "benchmark": "asgend-core-rpc-task-lifecycle",
        "started_utc": start_utc,
        "completed_utc": datetime.now(timezone.utc).isoformat(),
        "passed": total_error_count == 0,
        "summary": {"concurrency_levels": args.concurrencies, "runs_per_concurrency": args.runs,
                    "samples_per_run": args.samples_per_run,
                    "samples_per_method_per_concurrency": args.runs * args.samples_per_run,
                    "methods": list(METHODS), "error_count": total_error_count},
        "environment": {
            "platform": platform.platform(), "kernel": platform.release(), "machine": platform.machine(),
            "python": platform.python_version(), "cpu_count_logical": os.cpu_count(),
            "host_memory_total_bytes": host_memory_total_bytes(),
            "cgroup_cpu_max": read_text_if_exists(Path("/sys/fs/cgroup/cpu.max")),
            "cgroup_memory_max": read_text_if_exists(Path("/sys/fs/cgroup/memory.max")),
            "build": read_cmake_build_metadata(binary),
            "core_binary_sha256": sha256_file(binary), "benchmark_script_sha256": sha256_file(Path(__file__).resolve()),
            "mock_llm": True, "network_scope": "loopback only",
            "runtime_workers_equal_clients": True, "server_database": "fresh disposable SQLite per run",
            "server_max_connections": 24, "server_queue_max_pending": 100,
        },
        "latency_scope": "request wall time includes local TCP connect, JSON serialization, Core dispatch, response serialization and socket teardown; submit_to_terminal includes bounded status polling",
        "results": result_by_concurrency,
        "raw_samples": raw,
        "run_details": runs,
        "notes": [
            "This measures a local Core RPC process with an in-process test setup helper and deterministic localhost mock LLM; no production service, remote provider or real database service is included.",
            "Task methods use isolated disposable databases. answer samples are prepared in waiting_input; cancel samples are a mix of active/queued tasks and active task state is checked before timing; retry samples are pre-seeded dead-letter rows.",
            "p95/p99 are returned only when aggregate sample counts satisfy performance_metrics.py thresholds; otherwise they are null by policy.",
            "This is a diagnostic baseline, not a production capacity certification or CI absolute-latency gate.",
        ],
    }
    if args.output:
        output = args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"passed": report["passed"], "output": str(output), "summary": report["summary"]}, ensure_ascii=False))
    else:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
