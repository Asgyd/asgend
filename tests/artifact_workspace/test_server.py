import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "artifact_workspace" / "server.py"
spec = importlib.util.spec_from_file_location("asgend_artifact_workspace", MODULE_PATH)
server = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(server)


class ArtifactWorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="asgend-artifact-test-")
        self.base = Path(self.temp.name)
        self.tasks = self.base / "tasks"
        self.workspace = self.tasks / "task-123"
        self.workspace.mkdir(parents=True)
        self.args = {"__asgend_workspace_root": str(self.workspace)}

    def tearDown(self):
        self.temp.cleanup()

    def test_write_read_list_roundtrip(self):
        root = server._workspace_root(self.args, self.tasks)
        written = server.write_workspace_file(root, "src/main.py", "print('hello')\n")
        self.assertTrue(written["written"])
        read = server.read_workspace_file(root, "src/main.py")
        self.assertEqual(read["content"], "print('hello')\n")
        self.assertEqual(read["sha256"], written["sha256"])
        listed = server.list_workspace_files(root)
        self.assertEqual([x["path"] for x in listed["files"]], ["src/main.py"])

    def test_traversal_absolute_empty_segment_and_dot_rejected(self):
        root = server._workspace_root(self.args, self.tasks)
        for path in ("../outside.txt", "/etc/passwd", "src//main.py", "src/./main.py", "src\\main.py"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                server._relative_path(root, path)

    def test_symlink_file_and_directory_rejected(self):
        root = server._workspace_root(self.args, self.tasks)
        outside = self.base / "outside.txt"
        outside.write_text("secret", encoding="utf-8")
        (root / "linked.txt").symlink_to(outside)
        (root / "linkdir").symlink_to(self.base, target_is_directory=True)
        with self.assertRaises(ValueError):
            server.read_workspace_file(root, "linked.txt")
        with self.assertRaises(ValueError):
            server.write_workspace_file(root, "linkdir/pwn.txt", "no")

    def test_workspace_must_be_direct_child_and_non_symlink(self):
        nested = self.tasks / "nested" / "task"
        nested.mkdir(parents=True)
        with self.assertRaises(ValueError):
            server._workspace_root({"__asgend_workspace_root": str(nested)}, self.tasks)
        alias = self.tasks / "alias"
        alias.symlink_to(self.workspace, target_is_directory=True)
        with self.assertRaises(ValueError):
            server._workspace_root({"__asgend_workspace_root": str(alias)}, self.tasks)

    def test_executable_payloads_are_rejected(self):
        root = server._workspace_root(self.args, self.tasks)
        for path in ("run.sh", "application.apk", "native.so"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                server.write_workspace_file(root, path, "danger")

    def test_html_render_creates_png_and_mcp_image_content(self):
        root = server._workspace_root(self.args, self.tasks)
        server.write_workspace_file(root, "site/index.html", """<!doctype html><html><head><style>body{font:30px sans-serif;background:#eef}</style></head><body><h1>Visual QA sample</h1></body></html>""")
        # Exercise MCP image serialization separately from the browser runtime:
        # this container's system Chromium cannot complete headless startup.
        import base64
        from unittest.mock import patch
        pixel = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/2ioAAAAASUVORK5CYII=")
        def fake_browser(_html_path, output, _profile, _viewport="desktop"):
            output.write_bytes(pixel)
        with patch.object(server, "_render_html_with_chromium", side_effect=fake_browser):
            tool_result = server.call_tool("render_and_inspect", {**self.args, "path": "site/index.html"}, self.tasks)
        self.assertFalse(tool_result["isError"])
        image = next(item for item in tool_result["content"] if item["type"] == "image")
        self.assertEqual(image["mimeType"], "image/png")
        png = __import__("base64").b64decode(image["data"])
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        result_text = json.loads(tool_result["content"][0]["text"])
        self.assertEqual(result_text["source_path"], "site/index.html")
        self.assertTrue((root / result_text["preview_path"]).is_file())

    def test_docx_xlsx_and_pptx_visual_preview(self):
        root = server._workspace_root(self.args, self.tasks)
        from docx import Document
        from openpyxl import Workbook
        from pptx import Presentation
        from pptx.util import Inches

        doc = Document()
        doc.add_heading("Document preview", 0)
        doc.add_paragraph("Created for ASGEND visual inspection.")
        doc.save(root / "report.docx")

        book = Workbook()
        book.active["A1"] = "Quarter"
        book.active["B1"] = "Revenue"
        book.active["A2"] = "Q1"
        book.active["B2"] = 125
        book.save(root / "budget.xlsx")

        deck = Presentation()
        slide = deck.slides.add_slide(deck.slide_layouts[6])
        box = slide.shapes.add_textbox(Inches(1), Inches(1), Inches(7), Inches(1))
        box.text = "Product design review"
        deck.save(root / "design.pptx")

        for name in ("report.docx", "budget.xlsx", "design.pptx"):
            with self.subTest(name=name):
                result, png = server.render_and_inspect(root, name)
                self.assertTrue(result["preview_path"].endswith(".png"))
                self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
                self.assertTrue((root / result["preview_path"]).is_file())

    def test_create_office_files_and_visual_preview(self):
        root = server._workspace_root(self.args, self.tasks)
        examples = [
            ("word", "brief.docx", {"title": "Project brief", "blocks": [
                {"type": "heading", "level": 1, "text": "Goals"},
                {"type": "paragraph", "text": "Create a reviewed artifact."},
                {"type": "table", "headers": ["Item", "Status"], "rows": [["Voice", "Ready"]]},
            ]}),
            ("excel", "metrics.xlsx", {"sheets": [{"name": "Metrics", "rows": [["Metric", "Value"], ["Latency", 120]]}]}),
            ("powerpoint", "overview.pptx", {"slides": [
                {"title_slide": True, "title": "ASGEND overview", "body": "Agent workspace"},
                {"title": "Milestones", "bullets": ["Create", "Render", "Inspect"]},
            ]}),
            ("pdf", "brief.pdf", {"title": "PDF summary", "blocks": [
                {"type": "heading", "level": 1, "text": "PDF output"},
                *[{"type": "paragraph", "text": f"Detail paragraph {index}: visual proof page content."} for index in range(80)],
            ]}),
        ]
        for kind, path, spec in examples:
            with self.subTest(kind=kind):
                result = server.create_office_file(root, kind, path, json.dumps(spec))
                self.assertTrue(result["created"])
                self.assertTrue((root / path).is_file())
                preview, image = server.render_and_inspect(root, path)
                self.assertTrue(preview["preview_path"].endswith(".png"))
                self.assertEqual(preview["page"], 1)
                self.assertTrue(image.startswith(b"\x89PNG\r\n\x1a\n"))
                if kind == "pdf":
                    second, second_image = server.render_and_inspect(root, path, page=2)
                    self.assertEqual(second["page"], 2)
                    self.assertNotEqual(second["preview_path"], preview["preview_path"])
                    self.assertTrue(second_image.startswith(b"\x89PNG\r\n\x1a\n"))

    def test_create_office_rejects_wrong_extensions_and_invalid_specs(self):
        root = server._workspace_root(self.args, self.tasks)
        with self.assertRaises(ValueError):
            server.create_office_file(root, "word", "report.xlsx", json.dumps({"blocks": []}))
        with self.assertRaises(ValueError):
            server.create_office_file(root, "excel", "book.xlsx", json.dumps({"sheets": []}))
        with self.assertRaises(ValueError):
            server.create_office_file(root, "unknown", "file.docx", "{}")
        with self.assertRaises(ValueError):
            server.render_and_inspect(root, "missing.pdf", page=0)

    def test_mcp_jsonrpc_tools_list_and_call(self):
        self.assertEqual({tool["name"] for tool in server.TOOLS}, {
            "list_workspace_files", "read_workspace_file", "write_workspace_file", "create_office_file", "render_and_inspect"
        })
        result = server.call_tool("write_workspace_file", {**self.args, "path": "note.txt", "content": "ok"}, self.tasks)
        self.assertFalse(result["isError"])
        self.assertEqual(json.loads(result["content"][0]["text"])["path"], "note.txt")


if __name__ == "__main__":
    unittest.main()
