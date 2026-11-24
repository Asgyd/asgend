from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "audit_performance_inventory.py"
SPEC = importlib.util.spec_from_file_location("performance_inventory", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class InventoryTests(unittest.TestCase):
    def test_groups_sources_and_ctest_targets_deterministically(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "src" / "memory").mkdir(parents=True)
            (root / "tests").mkdir()
            (root / "include").mkdir()
            (root / "clients" / "asgend_android").mkdir(parents=True)
            (root / "src" / "asgend_tui.c").write_text("int tui(void) { return 0; }\n", encoding="utf-8")
            (root / "include" / "asgend_tui.h").write_text("int tui(void);\n", encoding="utf-8")
            (root / "src" / "memory" / "ann.c").write_text("int ann(void) { return 0; }\n", encoding="utf-8")
            (root / "tests" / "test.py").write_text("print('ok')\n", encoding="utf-8")
            (root / "clients" / "asgend_android" / "Example.java").write_text(
                "final class Example {}\n", encoding="utf-8")
            (root / "clients" / "asgend_android" / "AndroidManifest.xml").write_text(
                "<manifest />\n", encoding="utf-8")
            (root / "clients" / "asgend_android" / "build.gradle.kts").write_text(
                "plugins {}\n", encoding="utf-8")
            (root / "config" ).mkdir()
            (root / "config" / "fixture.json").write_text("{\"fixture\": true}\n", encoding="utf-8")
            (root / "docs").mkdir()
            (root / "docs" / "PERFORMANCE_REPEAT_INVENTORY.json").write_text(
                "{\"generated_audit\": true}\n", encoding="utf-8")
            (root / "node_modules").mkdir()
            (root / "node_modules" / "generated.js").write_text("generated();\n", encoding="utf-8")
            (root / ".venv").mkdir()
            (root / ".venv" / "installed.py").write_text("installed = True\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                "add_test(NAME fast COMMAND x)\n"
                "set_tests_properties(fast PROPERTIES TIMEOUT 10)\n", encoding="utf-8")
            result = MODULE.collect(root)
            self.assertEqual(result["schema_version"], 2)
            self.assertEqual(result["source_files_total"], 5)
            self.assertEqual(result["components"]["memory_index_retrieval"]["files"], 1)
            self.assertEqual(result["components"]["cli_tui"]["files"], 2)
            self.assertEqual(result["components"]["tests_and_contracts"]["files"], 1)
            self.assertEqual(result["components"]["native_android"]["files"], 1)
            self.assertEqual(result["configuration_inventory"]["files_total"], 3)
            self.assertEqual(result["configuration_inventory"]["by_extension"][".xml"]["files"], 1)
            self.assertEqual(result["configuration_inventory"]["by_extension"]["<CMakeLists.txt>"]["files"], 1)
            self.assertEqual(result["configuration_inventory"]["by_extension"][".gradle.kts"]["files"], 1)
            self.assertEqual(result["structured_data_inventory"]["files_total"], 1)
            self.assertEqual(result["structured_data_inventory"]["by_extension"][".json"]["files"], 1)
            self.assertEqual(result["ctest"]["registered_tests"], 1)
            self.assertEqual(result["ctest"]["explicit_timeouts"], {"fast": 10})
            self.assertEqual(result["ctest"]["effective_timeouts_seconds"], {"fast": 10})
            self.assertEqual(result["ctest"]["tests_without_effective_timeout"], [])
            self.assertEqual(result, MODULE.collect(root))


    def test_counts_only_actual_jobs_in_workflow(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            workflow = root / ".github" / "workflows" / "ci.yml"
            workflow.parent.mkdir(parents=True)
            (root / "CMakeLists.txt").write_text(
                "add_test(NAME fast COMMAND x)\n"
                "get_property(ASGEND_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)\n"
                "foreach(ASGEND_TEST_NAME IN LISTS ASGEND_REGISTERED_TESTS)\n"
                "  get_test_property(\"${ASGEND_TEST_NAME}\" TIMEOUT ASGEND_TEST_TIMEOUT)\n"
                "  if(NOT ASGEND_TEST_TIMEOUT OR \"${ASGEND_TEST_TIMEOUT}\" MATCHES \"NOTFOUND$\" OR \"${ASGEND_TEST_TIMEOUT}\" STREQUAL \"\")\n"
                "    set_tests_properties(\"${ASGEND_TEST_NAME}\" PROPERTIES TIMEOUT 60)\n"
                "  endif()\n"
                "endforeach()\n", encoding="utf-8")
            workflow.write_text(
                "name: CI\n"
                "on:\n"
                "  push:\n"
                "  pull_request:\n"
                "permissions:\n"
                "  contents: read\n"
                "jobs:\n"
                "  build:\n"
                "    timeout-minutes: 10\n"
                "    steps:\n"
                "      - run: ctest --output-on-failure\n"
                "  client:\n"
                "    steps:\n"
                "      - run: flutter test\n",
                encoding="utf-8",
            )
            result = MODULE.collect(root)
            info = result["workflows"]["ci.yml"]
            self.assertEqual(info["job_count"], 2)
            self.assertEqual(info["timeouts_minutes"], [10])
            self.assertTrue(info["has_ctest"])
            self.assertTrue(info["has_flutter_test"])

    def test_reads_finite_default_timeout_for_tests_without_explicit_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "CMakeLists.txt").write_text(
                "add_test(NAME short COMMAND x)\n"
                "get_property(ASGEND_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)\n"
                "foreach(ASGEND_TEST_NAME IN LISTS ASGEND_REGISTERED_TESTS)\n"
                "  get_test_property(\"${ASGEND_TEST_NAME}\" TIMEOUT ASGEND_TEST_TIMEOUT)\n"
                "  if(\"${ASGEND_TEST_TIMEOUT}\" STREQUAL \"NOTFOUND\")\n"
                "    set_tests_properties(\"${ASGEND_TEST_NAME}\" PROPERTIES TIMEOUT 60)\n"
                "  endif()\n"
                "endforeach()\n", encoding="utf-8")
            result = MODULE.collect(root)
            self.assertEqual(result["ctest"]["default_timeout_seconds"], 60)
            self.assertEqual(result["ctest"]["effective_timeouts_seconds"], {"short": 60})
            self.assertEqual(result["ctest"]["tests_without_effective_timeout"], [])
            self.assertNotIn("${ASGEND_TEST_NAME}", result["ctest"]["explicit_timeouts"])

    def test_rejects_non_project_root(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaises(ValueError):
                MODULE.collect(Path(td))


if __name__ == "__main__":
    unittest.main()
