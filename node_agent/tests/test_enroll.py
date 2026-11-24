import json
import os
import stat
import unittest
from unittest import mock

from helpers import NodeTestCase

from asgend_node import state as state_mod
from asgend_node.cp_client import CPClient, validate_server_url
from asgend_node.logutil import redact


class EnrollTest(NodeTestCase):
    def test_enroll_stores_private_credentials_and_never_logs_token(self):
        code = self.cp.add_enrollment()
        chmod_modes = []
        real_chmod = os.chmod

        def spy_chmod(path, mode, *a, **kw):
            chmod_modes.append((os.path.basename(str(path)), mode))
            return real_chmod(path, mode, *a, **kw)

        with mock.patch.object(state_mod.os, "chmod", side_effect=spy_chmod):
            rc, out, err = self.run_cli("enroll", "--server", self.cp.url, "--code", code, "--name", "box-1",
                                        "--state-dir", self.state_dir, "--core-port", str(self.core.port),
                                        "--core-token-file", self.token_file, "--log-level", "DEBUG")
        self.assertEqual(rc, 0, err)
        creds_path = os.path.join(self.state_dir, "credentials.json")
        with open(creds_path, encoding="utf-8") as fh:
            data = json.load(fh)
        token = data["node_token"]
        self.assertTrue(token.startswith("ncp_"))
        self.assertTrue(data["node_id"].startswith("node_"))
        self.assertEqual(data["workspace_id"], "ws_TEST")
        self.assertEqual(data["heartbeat_interval_seconds"], 15)
        # file mode 0600 (checked on POSIX; elsewhere verify the requested mode)
        if os.name == "posix":
            self.assertEqual(stat.S_IMODE(os.stat(creds_path).st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(os.stat(self.state_dir).st_mode), 0o700)
        self.assertTrue(any(name.startswith(".tmp-") and mode == 0o600 for name, mode in chmod_modes),
                        chmod_modes)
        # no temp files left behind (atomic replace)
        self.assertEqual([f for f in os.listdir(self.state_dir) if f.startswith(".tmp-")], [])
        # token and enrollment code never in stdout/stderr/logs
        for blob in (out, err, self.logs.text):
            self.assertNotIn(token, blob)
            self.assertNotIn(code, blob)
            self.assertNotIn(self.core_token, blob)
        self.assertIn(data["node_id"], out)
        # the core version was reported
        node = self.cp.nodes[data["node_id"]]
        self.assertEqual(node["core_version"], "0.9.9-mock")
        # status never prints the token
        rc, out, err = self.run_cli("status", "--state-dir", self.state_dir, "--check-core",
                                    "--core-port", str(self.core.port), "--core-token-file", self.token_file)
        self.assertEqual(rc, 0, err)
        self.assertNotIn(token, out)
        status = json.loads(out)
        self.assertEqual(status["node_id"], data["node_id"])
        self.assertTrue(status["core"]["reachable"])

    def test_reenroll_requires_force_and_bad_code_fails(self):
        self.enroll()
        code = self.cp.add_enrollment()
        rc, _, err = self.run_cli("enroll", "--server", self.cp.url, "--code", code, "--name", "x",
                                  "--state-dir", self.state_dir)
        self.assertEqual(rc, 2)
        self.assertIn("already enrolled", err)
        rc, _, err = self.run_cli("enroll", "--server", self.cp.url, "--code", "WRONG-CODE", "--name", "x",
                                  "--state-dir", self.state_dir, "--force")
        self.assertEqual(rc, 1)
        self.assertIn("unauthorized", err)

    def test_code_from_stdin_and_env(self):
        code = self.cp.add_enrollment()
        with mock.patch.dict(os.environ, {"ASGEND_NODE_ENROLL_CODE": code}):
            rc, out, err = self.run_cli("enroll", "--server", self.cp.url, "--name", "env-node",
                                        "--state-dir", self.state_dir, "--core-port", "1")
        self.assertEqual(rc, 0, err)
        self.assertNotIn(code, err + out + self.logs.text)

    def test_http_only_for_loopback_unless_explicit(self):
        validate_server_url("https://cp.example.com")
        validate_server_url("http://127.0.0.1:8080")
        validate_server_url("http://[::1]:8080")
        validate_server_url("http://localhost:8080")
        with self.assertRaises(ValueError):
            validate_server_url("http://cp.example.com")
        validate_server_url("http://cp.example.com", allow_insecure_http=True)
        with self.assertRaises(ValueError):
            validate_server_url("ftp://cp.example.com")
        with self.assertRaises(ValueError):
            validate_server_url("https://user:pw@cp.example.com")
        rc, _, err = self.run_cli("enroll", "--server", "http://cp.example.com", "--code", "C0DE-1234",
                                  "--name", "x", "--state-dir", self.state_dir)
        self.assertEqual(rc, 2)
        self.assertIn("loopback", err)

    def test_tls_verification_enabled_by_default(self):
        import ssl

        client = CPClient("https://cp.example.com")
        self.assertEqual(client._ssl_context.verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(client._ssl_context.check_hostname)

    def test_redaction(self):
        self.assertNotIn("abcdef123456", redact("token ncp_abcdef123456 here"))
        self.assertNotIn("XYZ", redact("Authorization: Bearer XYZ"))


if __name__ == "__main__":
    unittest.main()
