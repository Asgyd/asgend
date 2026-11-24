from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "benchmark_memory_vectors.py"
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("benchmark_memory_vectors", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class BenchmarkMethodologyTests(unittest.TestCase):
    def test_execution_order_is_balanced(self) -> None:
        self.assertEqual(MODULE.query_order(0), (("exact", True), ("ann", False)))
        self.assertEqual(MODULE.query_order(1), (("ann", False), ("exact", True)))
        self.assertEqual(MODULE.query_order(2), MODULE.query_order(0))
        first_modes = [MODULE.query_order(i)[0][0] for i in range(10)]
        self.assertEqual(first_modes.count("exact"), 5)
        self.assertEqual(first_modes.count("ann"), 5)

    def test_percentiles_and_summary(self) -> None:
        self.assertEqual(MODULE.percentile([1, 2, 3, 4, 5], 0.50), 3)
        self.assertEqual(MODULE.percentile([1, 2, 3, 4, 5], 0.95), 5)
        short = MODULE.latency_summary([1, 2, 3, 4, 5])
        self.assertEqual(short["median"], 3)
        self.assertIsNone(short["p95"])
        self.assertIsNone(short["p99"])
        self.assertEqual(short["tail_percentile_status"]["p95"], "not_reported_requires_20_samples")
        p95_ready = MODULE.latency_summary([float(n) for n in range(1, 21)])
        self.assertIsNotNone(p95_ready["p95"])
        self.assertIsNone(p95_ready["p99"])
        p99_ready = MODULE.latency_summary([float(n) for n in range(1, 101)])
        self.assertIsNotNone(p99_ready["p99"])
        with self.assertRaises(ValueError):
            MODULE.latency_summary([float("nan")])
        with self.assertRaises(ValueError):
            MODULE.latency_summary([-1.0])
        with self.assertRaises(ValueError):
            MODULE.latency_summary([])

    def test_path_recall_uses_unique_exact_paths(self) -> None:
        self.assertAlmostEqual(MODULE.path_recall_at_k(["a", "a", "b", "c"], ["a", "c"], 4), 2 / 3)
        self.assertEqual(MODULE.path_recall_at_k([], ["a"], 5), 0.0)
        with self.assertRaises(ValueError):
            MODULE.path_recall_at_k(["a"], ["a"], 0)


if __name__ == "__main__":
    unittest.main()
