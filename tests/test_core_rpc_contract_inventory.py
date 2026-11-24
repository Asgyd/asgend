#!/usr/bin/env python3
"""Keep the local C-core JSON-line RPC inventory, docs and clients in sync."""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else Path(__file__).resolve().parents[1]
BEGIN = "<!-- ASGEND_CORE_RPC_INVENTORY_BEGIN -->"
END = "<!-- ASGEND_CORE_RPC_INVENTORY_END -->"
MODEL_OPS = {"model_enable", "model_disable", "model_start", "model_stop"}


def extract_c_function(text: str, signature: str) -> str:
    """Extract a C function body while ignoring braces inside comments and literals."""
    pos = text.find(signature)
    if pos < 0:
        raise AssertionError(f"C function signature not found: {signature}")
    opening = text.find("{", pos + len(signature))
    if opening < 0:
        raise AssertionError(f"C function has no body: {signature}")
    depth = 0
    i = opening
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == "/" and nxt == "*":
                state = "block_comment"; i += 2; continue
            if ch == "/" and nxt == "/":
                state = "line_comment"; i += 2; continue
            if ch == '"':
                state = "string"; i += 1; continue
            if ch == "'":
                state = "char"; i += 1; continue
            if ch == "{": depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return text[opening + 1:i]
        elif state == "block_comment":
            if ch == "*" and nxt == "/": state = "code"; i += 2; continue
        elif state == "line_comment":
            if ch == "\n": state = "code"
        elif state in ("string", "char"):
            if ch == "\\": i += 2; continue
            if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        i += 1
    raise AssertionError(f"unterminated C function body: {signature}")


def runtime_ops(root: Path = ROOT) -> set[str]:
    text = (root / "src/server.c").read_text(encoding="utf-8")
    body = extract_c_function(text, "json_object *server_handle_request(Runtime *rt, json_object *request)")
    return set(re.findall(r'strcmp\(op,\s*"([a-z_]+)"\)\s*==\s*0', body))


def parse_documented_ops(text: str) -> list[str]:
    if text.count(BEGIN) != 1 or text.count(END) != 1:
        raise AssertionError("protocol inventory markers must each occur exactly once")
    block = text.split(BEGIN, 1)[1].split(END, 1)[0]
    match = re.search(r"(?s)```text\s*\n(.*?)\n```", block)
    if not match:
        raise AssertionError("protocol inventory must contain a text code fence")
    return [line.strip() for line in match.group(1).splitlines() if line.strip()]


def documented_ops(root: Path = ROOT) -> list[str]:
    text = (root / "docs/PROTOCOL.md").read_text(encoding="utf-8")
    return parse_documented_ops(text)


def scan_text_files(root: Path, relpaths: list[str]) -> str:
    chunks = []
    for rel in relpaths:
        path = root / rel
        if path.is_file():
            chunks.append(path.read_text(encoding="utf-8", errors="replace"))
        elif path.is_dir():
            for f in sorted(path.rglob("*")):
                if f.is_file() and f.suffix in {".kt", ".kts", ".dart", ".cpp", ".cc", ".h", ".hpp", ".py"}:
                    chunks.append(f.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(chunks)


def client_operation_sets(root: Path = ROOT) -> dict[str, set[str]]:
    android = scan_text_files(root, ["clients/asgend_android/app/src/main/kotlin"])
    flutter = scan_text_files(root, ["clients/asgend_flutter/lib/src", "clients/asgend_flutter/lib/main.dart"])
    qt = scan_text_files(root, ["clients/asgend_qt/src/core"])
    node = scan_text_files(root, ["node_agent/asgend_node/core_client.py"])
    # Static literal RPC values plus explicit allow-lists for operations selected dynamically
    # by UI action enums/arguments. Tests assert that the dynamic strings really exist in source.
    sets = {
        "native Android": set(re.findall(r'put\("op",\s*"([a-z_]+)"', android)),
        "Flutter": set(re.findall(r"['\"]op['\"]\s*:\s*['\"]([a-z_]+)['\"]", flutter)),
        "Qt": set(re.findall(r'QStringLiteral\("op"\)\s*,\s*QStringLiteral\("([a-z_]+)"\)', qt)),
        "Node Agent": set(re.findall(r'\.call\("([a-z_]+)"', node)),
    }
    for name, source in (("native Android", android), ("Flutter", flutter), ("Qt", qt)):
        dynamic = {op for op in MODEL_OPS if op in source}
        if dynamic:
            sets[name].update(dynamic)
    return sets


class CoreRpcInventoryTests(unittest.TestCase):
    def test_documented_inventory_exactly_matches_dispatch(self) -> None:
        actual = runtime_ops()
        documented = documented_ops()
        self.assertEqual(len(documented), len(set(documented)), "duplicate operation in docs inventory")
        self.assertEqual(documented, sorted(documented), "docs inventory must stay alphabetically sorted")
        self.assertEqual(set(documented), actual,
                         f"TCP RPC inventory differs: undocumented={sorted(actual-set(documented))}; "
                         f"not-in-dispatch={sorted(set(documented)-actual)}")

    def test_dispatch_inventory_has_reasonable_size(self) -> None:
        # Guard against an extractor silently returning only a tiny subset after refactoring.
        self.assertGreaterEqual(len(runtime_ops()), 30)

    def test_clients_only_reference_supported_operations(self) -> None:
        actual = runtime_ops()
        clients = client_operation_sets()
        minimum_literals = {"native Android": 25, "Flutter": 10, "Qt": 14, "Node Agent": 8}
        for client, ops in clients.items():
            with self.subTest(client=client):
                self.assertGreaterEqual(len(ops), minimum_literals[client],
                                        f"scanner unexpectedly found too few {client} operations")
                self.assertLessEqual(ops, actual,
                                     f"{client} references unknown RPC operations: {sorted(ops-actual)}")
        for client in ("native Android", "Flutter", "Qt"):
            with self.subTest(dynamic_model_operations=client):
                self.assertTrue(MODEL_OPS <= clients[client],
                                f"{client} dynamic model actions incomplete: {sorted(MODEL_OPS-clients[client])}")

    def test_inventory_parser_rejects_duplicate_and_missing_markers(self) -> None:
        good = f"{BEGIN}\n```text\nping\n```\n{END}"
        self.assertEqual(parse_documented_ops(good), ["ping"])
        with self.assertRaisesRegex(AssertionError, "markers"):
            parse_documented_ops(f"{BEGIN}\n{good}\n{END}")
        with self.assertRaisesRegex(AssertionError, "markers"):
            parse_documented_ops(good.replace(END, ""))
        with self.assertRaisesRegex(AssertionError, "code fence"):
            parse_documented_ops(f"{BEGIN}\nping\n{END}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
