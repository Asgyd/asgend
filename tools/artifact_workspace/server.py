#!/usr/bin/env python3
"""ASGEND task-workspace MCP server for safe file work and visual QA.

The trusted Core injects __asgend_workspace_root into each MCP tool call.
The MCP child is sandboxed by the Core and only receives write access to the
ASGEND task-workspaces parent. Every file path is resolved relative to the
injected per-task root; absolute paths and symlinks are rejected.
"""
from __future__ import annotations

import base64
import hashlib
import html
import json
import mimetypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
from pathlib import Path, PurePosixPath
from typing import Any

MAX_TEXT_BYTES = 1_500_000
MAX_RENDER_IMAGE_BYTES = 2_000_000
MAX_LIST_ITEMS = 500
SKIP_DIRS = {".git", "node_modules", ".dart_tool", ".gradle", "build", "dist", "__pycache__", ".idea"}
IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp"}
OFFICE_EXTS = {".docx", ".xlsx", ".pptx", ".odt", ".ods", ".odp", ".rtf"}
CODE_EXTS = {
    ".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".rs", ".go", ".java", ".kt",
    ".dart", ".js", ".ts", ".tsx", ".jsx", ".css", ".scss", ".json", ".yaml",
    ".yml", ".toml", ".xml", ".sh", ".sql", ".md", ".txt", ".html", ".htm", ".svg",
}


def _error(message: str) -> dict[str, Any]:
    return {"content": [{"type": "text", "text": "ERROR: " + message}], "isError": True}


def _text(message: str) -> dict[str, Any]:
    return {"content": [{"type": "text", "text": message}], "isError": False}


def _workspace_root(args: dict[str, Any], tasks_root: Path) -> Path:
    raw = args.get("__asgend_workspace_root")
    if not isinstance(raw, str) or not raw:
        raise ValueError("Core did not provide the trusted task workspace context")
    raw_root = Path(raw)
    tasks = tasks_root.resolve(strict=True)
    try:
        raw_stat = raw_root.lstat()
    except OSError as exc:
        raise ValueError("Task workspace is unavailable") from exc
    if raw_root.is_symlink() or not raw_root.is_absolute() or not raw_root.is_dir():
        raise ValueError("Task workspace must be an existing, non-symlink directory")
    root = raw_root.resolve(strict=True)
    if root.parent != tasks or not root.is_dir():
        raise ValueError("Task workspace must be a direct child of the configured ASGEND tasks directory")
    if raw_stat.st_uid not in {os.getuid(), 0}:
        raise ValueError("Task workspace owner is not trusted")
    return root


def _relative_path(root: Path, raw: Any) -> Path:
    if not isinstance(raw, str) or not raw or len(raw) > 1024 or "\x00" in raw:
        raise ValueError("path must be a non-empty relative path")
    if "\\" in raw:
        raise ValueError("path must use forward slashes")
    raw_parts = raw.split("/")
    if raw.startswith("/") or any(part in {"", ".", ".."} for part in raw_parts):
        raise ValueError("absolute paths, empty segments, and traversal segments are not allowed")
    rel = PurePosixPath(raw)
    current = root
    for part in rel.parts[:-1]:
        current = current / part
        try:
            st = current.lstat()
            if current.is_symlink() or not current.is_dir():
                raise ValueError("path contains a symlink or non-directory component")
        except FileNotFoundError:
            current.mkdir(mode=0o700)
    target = current / rel.parts[-1]
    try:
        if target.is_symlink():
            raise ValueError("symbolic-link files are not allowed")
    except OSError as exc:
        raise ValueError("cannot inspect requested path") from exc
    resolved_parent = current.resolve(strict=True)
    if resolved_parent != root and root not in resolved_parent.parents:
        raise ValueError("path escapes the task workspace")
    return target


def list_workspace_files(root: Path) -> dict[str, Any]:
    items: list[dict[str, Any]] = []
    for current, dirs, files in os.walk(root, followlinks=False):
        current_path = Path(current)
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith(".") and not (current_path / d).is_symlink()]
        for name in files:
            path = current_path / name
            try:
                st = path.lstat()
                if path.is_symlink() or not path.is_file() or st.st_size > 128 * 1024 * 1024:
                    continue
                rel = path.relative_to(root).as_posix()
                items.append({"path": rel, "size": st.st_size, "mime": mimetypes.guess_type(name)[0] or "application/octet-stream"})
            except OSError:
                continue
            if len(items) >= MAX_LIST_ITEMS:
                break
        if len(items) >= MAX_LIST_ITEMS:
            break
    items.sort(key=lambda item: item["path"].lower())
    return {"workspace": str(root), "files": items, "truncated": len(items) >= MAX_LIST_ITEMS}


def read_workspace_file(root: Path, rel: str) -> dict[str, Any]:
    path = _relative_path(root, rel)
    if not path.exists() or not path.is_file():
        raise ValueError("file does not exist")
    size = path.stat().st_size
    if size > MAX_TEXT_BYTES:
        raise ValueError(f"file is {size} bytes; text preview limit is {MAX_TEXT_BYTES} bytes")
    data = path.read_bytes()
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return {"path": rel, "size": size, "mime": mimetypes.guess_type(path.name)[0] or "application/octet-stream", "binary": True, "sha256": hashlib.sha256(data).hexdigest()}
    return {"path": rel, "size": size, "sha256": hashlib.sha256(data).hexdigest(), "content": text}


def write_workspace_file(root: Path, rel: str, content: str) -> dict[str, Any]:
    path = _relative_path(root, rel)
    if not isinstance(content, str):
        raise ValueError("content must be a UTF-8 string")
    data = content.encode("utf-8")
    if len(data) > MAX_TEXT_BYTES:
        raise ValueError(f"content exceeds {MAX_TEXT_BYTES} bytes")
    if path.suffix.lower() in {".exe", ".dll", ".so", ".dylib", ".sh", ".bat", ".cmd", ".ps1", ".app", ".apk", ".ipa"}:
        raise ValueError("writing executable/package artifacts through this tool is disabled")
    temp = path.with_name(path.name + ".asgend-tmp-" + os.urandom(6).hex())
    try:
        with temp.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass
    return {"path": rel, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "written": True}


def create_office_file(root: Path, kind: str, rel: str, spec_json: str) -> dict[str, Any]:
    """Create a real DOCX/XLSX/PPTX/PDF artifact from a bounded JSON specification."""
    if not isinstance(kind, str) or kind not in {"word", "excel", "powerpoint", "pdf"}:
        raise ValueError("kind must be word, excel, powerpoint, or pdf")
    if not isinstance(spec_json, str) or len(spec_json.encode("utf-8")) > MAX_TEXT_BYTES:
        raise ValueError("spec_json must be a JSON string up to 1.5 MiB")
    spec = json.loads(spec_json)
    if not isinstance(spec, dict):
        raise ValueError("spec_json must contain a JSON object")
    path = _relative_path(root, rel)
    expected = {"word": ".docx", "excel": ".xlsx", "powerpoint": ".pptx", "pdf": ".pdf"}[kind]
    if path.suffix.lower() != expected:
        raise ValueError(f"{kind} output must use the {expected} extension")
    if path.exists() and not path.is_file():
        raise ValueError("output path is not a regular file")
    temp = path.with_name(path.stem + ".asgend-tmp-" + os.urandom(6).hex() + expected)
    try:
        if kind in {"word", "pdf"}:
            try:
                from docx import Document
            except ImportError as exc:
                raise RuntimeError("python-docx is not installed; install tools/artifact_workspace/requirements.txt") from exc
            doc = Document()
            title = spec.get("title")
            if isinstance(title, str) and title.strip():
                doc.add_heading(title.strip()[:300], 0)
            blocks = spec.get("blocks", [])
            if not isinstance(blocks, list) or len(blocks) > 500:
                raise ValueError("Word blocks must be an array of at most 500 entries")
            for block in blocks:
                if not isinstance(block, dict):
                    raise ValueError("each Word block must be an object")
                block_type = block.get("type", "paragraph")
                text = block.get("text", "")
                if block_type in {"heading", "paragraph", "bullet", "number"}:
                    if not isinstance(text, str) or len(text) > 20000:
                        raise ValueError("Word block text must be a string up to 20000 characters")
                    if block_type == "heading":
                        level = block.get("level", 1)
                        if not isinstance(level, int) or level not in {1, 2, 3}:
                            raise ValueError("Word heading level must be 1, 2, or 3")
                        doc.add_heading(text, level=level)
                    elif block_type == "bullet":
                        doc.add_paragraph(text, style="List Bullet")
                    elif block_type == "number":
                        doc.add_paragraph(text, style="List Number")
                    else:
                        doc.add_paragraph(text)
                elif block_type == "table":
                    headers = block.get("headers", [])
                    rows = block.get("rows", [])
                    if not isinstance(headers, list) or not headers or len(headers) > 20 or not isinstance(rows, list) or len(rows) > 1000:
                        raise ValueError("Word table needs 1..20 headers and at most 1000 rows")
                    table = doc.add_table(rows=1, cols=len(headers))
                    table.style = "Table Grid"
                    for index, header in enumerate(headers):
                        table.rows[0].cells[index].text = str(header)[:500]
                    for row in rows:
                        if not isinstance(row, list) or len(row) != len(headers):
                            raise ValueError("each Word table row must match the header column count")
                        cells = table.add_row().cells
                        for index, value in enumerate(row):
                            cells[index].text = str(value)[:2000]
                else:
                    raise ValueError("Word block type must be heading, paragraph, bullet, number, or table")
            if kind == "word":
                doc.save(temp)
            else:
                lo = shutil.which("libreoffice") or shutil.which("soffice")
                if not lo:
                    raise RuntimeError("LibreOffice is required to create PDF files")
                with tempfile.TemporaryDirectory(prefix="asgend-pdf-create-") as tmpdir:
                    tmp_root = Path(tmpdir)
                    docx_path = tmp_root / "source.docx"
                    out_dir = tmp_root / "pdf"
                    out_dir.mkdir()
                    profile = (tmp_root / "lo-profile").as_uri()
                    doc.save(docx_path)
                    _run([lo, "--headless", f"-env:UserInstallation={profile}", "--convert-to", "pdf", "--outdir", str(out_dir), str(docx_path)], timeout=60)
                    generated = out_dir / "source.pdf"
                    if not generated.is_file():
                        raise RuntimeError("LibreOffice did not produce the requested PDF")
                    shutil.copyfile(generated, temp)
        elif kind == "excel":
            try:
                from openpyxl import Workbook
                from openpyxl.styles import Font, PatternFill, Alignment
            except ImportError as exc:
                raise RuntimeError("openpyxl is not installed; install tools/artifact_workspace/requirements.txt") from exc
            sheets = spec.get("sheets", [])
            if not isinstance(sheets, list) or not sheets or len(sheets) > 20:
                raise ValueError("Excel must contain 1..20 sheets")
            workbook = Workbook()
            workbook.remove(workbook.active)
            for sheet_spec in sheets:
                if not isinstance(sheet_spec, dict):
                    raise ValueError("each Excel sheet must be an object")
                name = sheet_spec.get("name", "Sheet")
                rows = sheet_spec.get("rows", [])
                if not isinstance(name, str) or not name.strip() or not isinstance(rows, list) or len(rows) > 5000:
                    raise ValueError("Excel sheet needs a name and at most 5000 rows")
                worksheet = workbook.create_sheet(name[:31])
                row_width = 0
                for row_index, row in enumerate(rows, start=1):
                    if not isinstance(row, list) or len(row) > 100:
                        raise ValueError("Excel rows must be arrays with at most 100 columns")
                    row_width = max(row_width, len(row))
                    for col_index, value in enumerate(row, start=1):
                        if isinstance(value, (str, int, float, bool)) or value is None:
                            cell = worksheet.cell(row=row_index, column=col_index, value=value)
                        else:
                            raise ValueError("Excel cells must be strings, numbers, booleans, or null")
                        if row_index == 1:
                            cell.font = Font(bold=True, color="FFFFFF")
                            cell.fill = PatternFill("solid", fgColor="243B53")
                            cell.alignment = Alignment(vertical="center", wrap_text=True)
                if rows:
                    worksheet.freeze_panes = "A2"
                    if row_width:
                        worksheet.auto_filter.ref = worksheet.dimensions
                        for col in range(1, row_width + 1):
                            letter = worksheet.cell(row=1, column=col).column_letter
                            values = [len(str(worksheet.cell(row=r, column=col).value or "")) for r in range(1, min(100, worksheet.max_row) + 1)]
                            worksheet.column_dimensions[letter].width = min(48, max(12, max(values, default=10) + 2))
            workbook.save(temp)
        else:
            try:
                from pptx import Presentation
                from pptx.util import Inches, Pt
            except ImportError as exc:
                raise RuntimeError("python-pptx is not installed; install tools/artifact_workspace/requirements.txt") from exc
            slides = spec.get("slides", [])
            if not isinstance(slides, list) or not slides or len(slides) > 100:
                raise ValueError("PowerPoint must contain 1..100 slides")
            presentation = Presentation()
            presentation.slide_width = Inches(13.333)
            presentation.slide_height = Inches(7.5)
            for index, slide_spec in enumerate(slides):
                if not isinstance(slide_spec, dict):
                    raise ValueError("each PowerPoint slide must be an object")
                layout = presentation.slide_layouts[0] if index == 0 and slide_spec.get("title_slide") else presentation.slide_layouts[1]
                slide = presentation.slides.add_slide(layout)
                title = slide_spec.get("title", "")
                if not isinstance(title, str) or len(title) > 300:
                    raise ValueError("slide title must be a string up to 300 characters")
                if slide.shapes.title:
                    slide.shapes.title.text = title
                bullets = slide_spec.get("bullets", [])
                body = slide_spec.get("body", "")
                if not isinstance(bullets, list) or len(bullets) > 20 or not isinstance(body, str):
                    raise ValueError("slide body must be text and bullets must contain at most 20 items")
                if layout != presentation.slide_layouts[0]:
                    placeholder = slide.placeholders[1]
                    frame = placeholder.text_frame
                    frame.clear()
                    if body:
                        frame.text = body[:4000]
                        frame.paragraphs[0].font.size = Pt(20)
                    for bullet in bullets:
                        if not isinstance(bullet, str) or len(bullet) > 1000:
                            raise ValueError("PowerPoint bullet must be a string up to 1000 characters")
                        paragraph = frame.add_paragraph()
                        paragraph.text = bullet
                        paragraph.level = 0
                        paragraph.font.size = Pt(18)
            presentation.save(temp)
        os.replace(temp, path)
    finally:
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass
    return {"path": rel, "size": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "created": True, "office_kind": kind}


def _run(command: list[str], timeout: int = 40) -> None:
    try:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=timeout, check=False,
                                env={"PATH": "/usr/local/bin:/usr/bin:/bin", "HOME": os.environ.get("HOME", "/tmp"),
                                     "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"})
    except FileNotFoundError as exc:
        raise RuntimeError(f"required renderer is not installed: {command[0]}") from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"renderer timed out after {timeout} seconds") from exc
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace")[-1200:]
        raise RuntimeError(f"renderer failed ({result.returncode}): {detail}")


def _render_html_with_chromium(html_path: Path, output: Path, profile: Path, viewport: str = "desktop") -> None:
    chrome = shutil.which("chromium") or shutil.which("chromium-browser") or shutil.which("google-chrome")
    if not chrome:
        raise RuntimeError("Chromium is not installed; cannot render HTML/UI previews")
    sizes = {"desktop": "1440,1000", "tablet": "1024,768", "mobile": "390,844"}
    if viewport not in sizes:
        raise ValueError("viewport must be desktop, tablet, or mobile")
    _run([
        chrome, "--headless", "--no-sandbox", "--disable-gpu", "--disable-background-networking",
        "--disable-extensions", "--disable-default-apps", "--disable-sync", "--no-first-run",
        "--hide-scrollbars", "--disable-javascript", "--host-resolver-rules=MAP * ~NOTFOUND",
        f"--window-size={sizes[viewport]}", "--force-device-scale-factor=1",
        f"--user-data-dir={profile}", f"--screenshot={output}", html_path.resolve().as_uri(),
    ], timeout=20)


def _code_preview_html(source: Path, temp: Path) -> Path:
    ext = source.suffix.lower()
    content = source.read_text(encoding="utf-8", errors="replace")[:180_000]
    if ext == ".css":
        body = """<header><h1>Component / site stylesheet preview</h1></header><main><nav>Navigation</nav><section><h2>Page title</h2><p>Sample page content to evaluate typography, spacing, colors and layout.</p><button>Primary action</button><div class=\"cards\"><article>Card one</article><article>Card two</article><article>Card three</article></div></section></main>"""
        css = f"<style>{content}</style>"
        escaped = ""
    else:
        escaped = html.escape(content)
        css = ""
        body = f"<h1>{html.escape(source.name)}</h1><pre><code>{escaped}</code></pre>"
    page = temp / "code-preview.html"
    page.write_text("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>ASGEND artifact preview</title><style>body{margin:0;padding:28px;background:#101820;color:#e7edf5;font:14px system-ui}main{display:flex;gap:24px}nav,section,article{padding:22px;background:#1e293b;border-radius:12px}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#0b1220;padding:18px;border-radius:10px;font:12px/1.5 ui-monospace,monospace}button{padding:10px 16px} .cards{display:flex;gap:12px;margin-top:20px}</style>" + css + "</head><body>" + body + "</body></html>", encoding="utf-8")
    return page


def render_and_inspect(root: Path, rel: str, viewport: str = "desktop", page: int = 1) -> tuple[dict[str, Any], bytes]:
    if type(page) is not int or page < 1 or page > 100:
        raise ValueError("page must be an integer between 1 and 100")
    source = _relative_path(root, rel)
    if not source.exists() or not source.is_file():
        raise ValueError("artifact does not exist")
    if source.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("artifact exceeds the 64 MiB render limit")
    ext = source.suffix.lower()
    digest = hashlib.sha256(source.read_bytes()).hexdigest()[:12]
    preview_name = f"preview_{source.stem[:48]}_{digest}_p{page}.png"
    preview_path = _relative_path(root, preview_name)

    if ext in IMAGE_EXTS:
        if page != 1:
            raise ValueError("image artifacts only support page 1")
        data = source.read_bytes()
        if len(data) > MAX_RENDER_IMAGE_BYTES:
            # Downscale large image artifacts through ImageMagick when available.
            convert = shutil.which("magick") or shutil.which("convert")
            if not convert:
                raise ValueError("image exceeds inline preview limit and ImageMagick is not installed")
            with tempfile.TemporaryDirectory(prefix="asgend-render-") as tmpdir:
                output = Path(tmpdir) / "preview.png"
                _run([convert, str(source), "-resize", "1600x1200>", str(output)])
                data = output.read_bytes()
        if source.resolve() != preview_path.resolve(strict=False):
            preview_path.write_bytes(data)
    else:
        with tempfile.TemporaryDirectory(prefix="asgend-render-") as tmpdir_s:
            tmp = Path(tmpdir_s)
            rendered = tmp / "rendered.png"
            if ext == ".pdf" or ext in OFFICE_EXTS:
                pdf_path = source
                if ext in OFFICE_EXTS:
                    lo = shutil.which("libreoffice") or shutil.which("soffice")
                    if not lo:
                        raise RuntimeError("LibreOffice is not installed; Office document render unavailable")
                    out_dir = tmp / "office-out"
                    out_dir.mkdir()
                    profile = (tmp / "lo-profile").as_uri()
                    _run([lo, "--headless", f"-env:UserInstallation={profile}", "--convert-to", "pdf", "--outdir", str(out_dir), str(source)], timeout=60)
                    converted = out_dir / (source.stem + ".pdf")
                    if not converted.is_file():
                        raise RuntimeError("LibreOffice did not produce a PDF preview")
                    pdf_path = converted
                pdftoppm = shutil.which("pdftoppm")
                if not pdftoppm:
                    raise RuntimeError("pdftoppm is not installed; PDF preview unavailable")
                prefix = tmp / "page"
                _run([pdftoppm, "-f", str(page), "-l", str(page), "-scale-to", "1600", "-singlefile", "-png", str(pdf_path), str(prefix)], timeout=45)
                rendered = Path(str(prefix) + ".png")
            elif ext in {".html", ".htm", ".svg"}:
                if page != 1:
                    raise ValueError("HTML and SVG previews only support page 1; use viewport to inspect responsive layouts")
                profile = tmp / "chromium-profile"
                profile.mkdir()
                _render_html_with_chromium(source, rendered, profile, viewport)
            elif ext == ".css" or ext in CODE_EXTS:
                if page != 1:
                    raise ValueError("source code previews only support page 1")
                preview_html = _code_preview_html(source, tmp)
                profile = tmp / "chromium-profile"
                profile.mkdir()
                _render_html_with_chromium(preview_html, rendered, profile, viewport)
            else:
                raise ValueError(f"no visual renderer is registered for {ext or 'this file type'}")
            data = rendered.read_bytes()
            if not data or len(data) > MAX_RENDER_IMAGE_BYTES:
                raise RuntimeError("rendered image is empty or exceeds the 2 MiB inline limit")
            preview_path.write_bytes(data)

    data = preview_path.read_bytes()
    if len(data) > MAX_RENDER_IMAGE_BYTES:
        raise RuntimeError("preview PNG exceeds the MCP inline image limit")
    result = {
        "source_path": rel,
        "preview_path": preview_name,
        "page": page,
        "source_sha256": digest,
        "preview_size": len(data),
        "visual_check": "Screenshot generated. Inspect the image content in this tool response, then revise and render again if layout/content is incorrect.",
    }
    return result, data


TOOLS = [
    {"name": "list_workspace_files", "description": "List files in the current task's isolated workspace. Use this before editing to understand the project structure.",
     "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False}},
    {"name": "read_workspace_file", "description": "Read a UTF-8 source/config/document file from the current task workspace (up to 1.5 MiB). Binary files return metadata only.",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"], "additionalProperties": False}},
    {"name": "write_workspace_file", "description": "Create or update a UTF-8 project file in the current isolated task workspace. Supports code, HTML/CSS, Markdown, JSON, and source files; use the dedicated preview tool to visually inspect output. Do not write outside the task workspace.",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}}, "required": ["path", "content"], "additionalProperties": False}},
    {"name": "create_office_file", "description": "Create a real Word (.docx), Excel (.xlsx), PowerPoint (.pptx), or PDF (.pdf) file from a structured JSON specification. PDF is built from the same structured document blocks using LibreOffice. After creating, call render_and_inspect to receive a screenshot and visually check the result.",
     "inputSchema": {"type": "object", "properties": {"kind": {"type": "string", "enum": ["word", "excel", "powerpoint", "pdf"]}, "path": {"type": "string"}, "spec_json": {"type": "string"}}, "required": ["kind", "path", "spec_json"], "additionalProperties": False}},
    {"name": "render_and_inspect", "description": "Render and visually inspect an artifact from the current task workspace. Supports PDF, DOCX, XLSX, PPTX, ODT/ODS/ODP/RTF, HTML/CSS/SVG, common code and image files. HTML/CSS can be captured at desktop, tablet or mobile viewport. Saves a preview PNG as a task artifact and returns the screenshot as an image so the model can inspect it. Call after creation and after revisions; verify layout, clipping, text, visual hierarchy and errors.",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "viewport": {"type": "string", "enum": ["desktop", "tablet", "mobile"]}, "page": {"type": "integer", "minimum": 1, "maximum": 100}}, "required": ["path"], "additionalProperties": False}},
]


def call_tool(name: str, args: dict[str, Any], tasks_root: Path) -> dict[str, Any]:
    try:
        root = _workspace_root(args, tasks_root)
        if name == "list_workspace_files":
            return _text(json.dumps(list_workspace_files(root), ensure_ascii=False, indent=2))
        if name == "read_workspace_file":
            result = read_workspace_file(root, args.get("path"))
            return _text(json.dumps(result, ensure_ascii=False, indent=2))
        if name == "write_workspace_file":
            result = write_workspace_file(root, args.get("path"), args.get("content"))
            return _text(json.dumps(result, ensure_ascii=False, indent=2))
        if name == "create_office_file":
            result = create_office_file(root, args.get("kind"), args.get("path"), args.get("spec_json"))
            return _text(json.dumps(result, ensure_ascii=False, indent=2))
        if name == "render_and_inspect":
            result, image_data = render_and_inspect(root, args.get("path"), args.get("viewport", "desktop"), args.get("page", 1))
            return {"content": [
                {"type": "text", "text": json.dumps(result, ensure_ascii=False, indent=2)},
                {"type": "image", "mimeType": "image/png", "data": base64.b64encode(image_data).decode("ascii")},
            ], "isError": False}
        return _error("unknown tool: " + name)
    except Exception as exc:
        return _error(str(exc)[:1500])


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--tasks-root", required=True)
    tasks_root = Path(parser.parse_args().tasks_root).resolve()
    if not tasks_root.is_dir():
        print("ASGEND tasks root is unavailable", file=sys.stderr)
        return 2

    for raw in sys.stdin:
        try:
            request = json.loads(raw)
        except Exception:
            continue
        if not isinstance(request, dict):
            continue
        method = request.get("method")
        request_id = request.get("id")
        if method == "notifications/initialized" or method == "notifications/cancelled":
            continue
        if method == "initialize":
            result = {"protocolVersion": request.get("params", {}).get("protocolVersion", "2024-11-05"),
                      "capabilities": {"tools": {}},
                      "serverInfo": {"name": "asgend-artifact-workspace", "version": "1.0.0"}}
        elif method == "ping":
            result = {}
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            params = request.get("params") or {}
            args = params.get("arguments") or {}
            if not isinstance(args, dict):
                tool_result = _error("arguments must be an object")
            else:
                tool_result = call_tool(str(params.get("name", "")), args, tasks_root)
            result = tool_result
        else:
            if request_id is None:
                continue
            response = {"jsonrpc": "2.0", "id": request_id,
                        "error": {"code": -32601, "message": "Method not found"}}
            sys.stdout.write(json.dumps(response, separators=(",", ":")) + "\n")
            sys.stdout.flush()
            continue
        if request_id is None:
            continue
        response = {"jsonrpc": "2.0", "id": request_id, "result": result}
        sys.stdout.write(json.dumps(response, ensure_ascii=False, separators=(",", ":")) + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
