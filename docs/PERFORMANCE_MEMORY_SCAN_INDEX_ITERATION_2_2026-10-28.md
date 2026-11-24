# Stage 31 — Critical review iteration 2: timeout, process, pipe, and RSS safety

## Scope

Review the benchmark child-process supervisor and its failure path, including timeout cleanup and resource observation. This is a local critical review, not an external or subagent review.

## Findings and disposition

- The runner launches each scanner in a separate process group/session, continuously drains stdout/stderr, and samples `VmHWM` from `/proc/<pid>/status` while the process exists.
- At the deadline, the runner sends `SIGTERM` to the whole process group and escalates to `SIGKILL`; it waits for the direct child and closes both streams on every exit path.
- **Fixed in the regression-test calibration.** A 0.30 s test deadline could kill the Python helper during interpreter startup, before the grandchild PID was printed; that made the assertion race with startup rather than test cleanup. The test deadline was raised to 1.0 s while the deliberate parent/grandchild sleep remains 30 s. This gives the process tree time to start and still requires bounded timeout cleanup.
- The regression test deliberately makes a grandchild inherit stdout/stderr, so a test implementation that kills only the direct child would leave the pipe open or the descendant running.

## Executed check

`python3 tests/test_memory_scan_timeout.py` passed: the timed-out process group was terminated, the grandchild was no longer running (a transient zombie state is treated as terminated), the direct child was reaped, and stdout/stderr pipes were closed. The test is registered as `performance_memory_scan_timeout_guard` with CTest timeout 10 seconds.

## Status

**PASS.** The timeout regression passed both focused execution and the complete suite. The full Release CTest returned exit code 0: **54/54 PASS in 270.30 s**. All 54 registered timeout values were positive and finite (10–300 s); all 54 test results were observed in the final log. The post-run process checker found no live CTest, integration/test, benchmark, scanner, relevant agent-orchestrator or `sleep 30` process. See the Stage 31 CTest, timeout metadata and process-cleanup artifacts.
