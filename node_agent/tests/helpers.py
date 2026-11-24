"""Shared fixtures for the node agent tests."""

from __future__ import annotations

import io
import logging
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
NODE_ROOT = os.path.dirname(HERE)
if NODE_ROOT not in sys.path:
    sys.path.insert(0, NODE_ROOT)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from asgend_node import cli  # noqa: E402
from asgend_node.agent import NodeAgent  # noqa: E402
from asgend_node.config import load_config  # noqa: E402
from asgend_node.core_client import CoreClient, CoreEndpoint  # noqa: E402
from asgend_node.cp_client import CPClient  # noqa: E402
from asgend_node.logutil import LOGGER_NAME  # noqa: E402
from asgend_node.state import Credentials, Store  # noqa: E402
from mock_core import MockCore  # noqa: E402
from mock_cp import MockCP  # noqa: E402


class LogCapture(logging.Handler):
    def __init__(self):
        super().__init__(logging.DEBUG)
        self.lines: list[str] = []

    def emit(self, record):
        self.lines.append(self.format(record))

    @property
    def text(self) -> str:
        return "\n".join(self.lines)


class NodeTestCase(unittest.TestCase):
    core_script: str | None = None
    core_token: str | None = "core-secret-token"
    chunk_size = 8 * 1024 * 1024

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="asgend-node-test-")
        self.state_dir = os.path.join(self.tmp, "state")
        self.cp = MockCP(chunk_size=self.chunk_size).start()
        self.core = MockCore(auth_token=self.core_token, script=self.core_script).start()
        self.token_file = os.path.join(self.tmp, "core.token")
        with open(self.token_file, "w", encoding="utf-8") as fh:
            fh.write(self.core_token or "")
        self.logs = LogCapture()
        logger = logging.getLogger(LOGGER_NAME)
        logger.addHandler(self.logs)
        logger.setLevel(logging.DEBUG)
        self.agents: list[NodeAgent] = []

    def tearDown(self):
        for a in self.agents:
            a.stop()
            a.store.close()
        logging.getLogger(LOGGER_NAME).removeHandler(self.logs)
        self.cp.stop()
        self.core.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_cli(self, *argv: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = cli.main(list(argv))
        return rc, out.getvalue(), err.getvalue()

    def enroll(self, name: str = "node-a") -> dict:
        code = self.cp.add_enrollment()
        rc, out, err = self.run_cli("enroll", "--server", self.cp.url, "--code", code, "--name", name,
                                    "--state-dir", self.state_dir, "--core-port", str(self.core.port),
                                    "--core-token-file", self.token_file, "--labels", "gpu,linux")
        self.assertEqual(rc, 0, err)
        return Credentials(self.state_dir).load()

    def config(self, **overrides):
        base = dict(state_dir=self.state_dir, core_host="127.0.0.1", core_port=self.core.port,
                    core_auth_token_file=self.token_file, lease_wait_ms=0, event_poll_interval=0.05,
                    backoff_initial=0.05, backoff_max=0.2, http_timeout=5.0, core_timeout=5.0)
        base.update(overrides)
        return load_config(None, base)

    def make_agent(self, state_dir: str | None = None, **overrides) -> NodeAgent:
        state_dir = state_dir or self.state_dir
        cfg = self.config(state_dir=state_dir, **overrides)
        creds_store = Credentials(self.state_dir)
        creds = creds_store.load()
        if state_dir != self.state_dir:
            creds_store = Credentials(state_dir)
            creds_store.save(creds)
        cp = CPClient(creds["server_url"], token=creds["node_token"], allow_insecure_http=False, timeout=5.0)
        core = CoreClient(CoreEndpoint(port=self.core.port, auth_token=self.core_token, timeout=5.0))
        agent = NodeAgent(cfg, creds, Store(state_dir), core, cp, credentials_store=creds_store)
        self.agents.append(agent)
        return agent

    def drain(self, agent: NodeAgent, rounds: int = 3) -> None:
        for _ in range(rounds):
            agent.poll_commands_once(wait_ms=0)
            agent.sync_events_once()
            agent.upload_artifacts_once()
        agent.sync_events_once()
