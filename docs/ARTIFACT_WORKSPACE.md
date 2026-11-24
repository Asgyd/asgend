# ASGEND artifact workspace and visual QA

The `artifact_workspace` MCP server gives the project agent a constrained set of tools for producing and inspecting task artifacts. Core injects the trusted `__asgend_workspace_root` for each call. The agent cannot choose another root. All paths are relative, traversal and symbolic links are rejected, and writes are confined by the Core's filesystem sandbox to the configured task workspaces.

## Tools

- `list_workspace_files()` — list current task files.
- `read_workspace_file(path)` — read UTF-8 text (bounded); binary files return metadata.
- `write_workspace_file(path, content)` — atomically write UTF-8 code, HTML/CSS, JSON, Markdown and other supported source files. Executable binaries/packages/scripts are not accepted by this tool.
- `create_office_file(kind, path, spec_json)` — create actual `.docx`, `.xlsx`, `.pptx`, and `.pdf` files from structured input.
- `render_and_inspect(path, viewport, page)` — produce a PNG render, save it as a task artifact, and return the image itself in MCP `image` content. The Core forwards that image to the active vision-capable model as an OpenAI-compatible `image_url` part. Supported viewports: `desktop`, `tablet`, `mobile`. Use `page=1..100` for a specific PDF page, Office page/sheet print page, or presentation slide; use a new render call for each page to inspect. HTML/CSS/SVG and source-code captures use page 1 and the selected viewport.

### Office JSON examples

Word:

```json
{"title":"Project brief","blocks":[{"type":"heading","level":1,"text":"Summary"},{"type":"paragraph","text":"A short introduction."},{"type":"bullet","text":"First point"},{"type":"table","headers":["Item","Status"],"rows":[["Voice","Ready"]]}]}
```

Excel:

```json
{"sheets":[{"name":"Metrics","rows":[["Metric","Value"],["Latency (ms)",120],["Pass rate",0.98]]}]}
```

PDF (same structured document blocks): use `kind: "pdf"`, a `.pdf` path, and the same `title`/`blocks` schema as Word. PDF is generated from an intermediate DOCX through LibreOffice.

PowerPoint:

```json
{"slides":[{"title_slide":true,"title":"Project overview","body":"Prepared with ASGEND"},{"title":"Milestones","bullets":["Create","Render","Inspect"]}]}
```

Call `render_and_inspect` after document creation and after each substantial revision. Office rendering converts documents to PDF with LibreOffice and rasterizes the requested page to PNG. For XLSX, use page numbers corresponding to printed sheet pages (set print areas/page breaks for predictable output); for PPTX, the page index is the slide index. PDF inspection rasterizes the requested page. Call once per page/slide/sheet that must be visually proven.

## Runtime dependencies

Install Python document libraries (these are **not** model inference libraries):

```bash
python3 -m pip install -r tools/artifact_workspace/requirements.txt
```

Renderer dependencies:

- LibreOffice (`libreoffice` or `soffice`) for DOCX/XLSX/PPTX/ODT/ODS/ODP/RTF to PDF.
- Poppler `pdftoppm` for PDF page rasterization.
- Chromium/Chrome for HTML/CSS/SVG and code previews.
- ImageMagick (`magick` or `convert`) is optional for resizing oversized image files.

The inference model remains `llama-server` (GGUF + multimodal projector). The artifact tool does not load models and does not depend on PyTorch or Transformers.

## Render security

The renderer is launched by the sandboxed MCP child. The child has task-workspace write access, read-only project access needed for its runtime, no IP sockets or external network connects, and only the local Unix socketpair IPC exception needed by Chromium. Rendered pages must not depend on remote assets. Keep previews self-contained (inline CSS, local task assets). JavaScript is disabled in the renderer: the screenshot is a static layout/visual QA proof, not a live application runtime or an interaction test. Do not pass untrusted external URLs to the renderer.

Run ASGEND Core as its dedicated non-root service account so task workspace ownership and MCP privilege dropping stay aligned. Deploy LibreOffice/Poppler/Chromium on the same worker that runs the MCP child.

## Verification

```bash
python3 -m py_compile tools/artifact_workspace/server.py
python3 -m unittest discover -s tests/artifact_workspace -v
```

HTML screenshot tests use a deterministic PNG renderer stub in restricted CI containers. Office document creation and LibreOffice/Poppler previews are exercised with real files when those binaries and packages are installed.

For Flutter/Dart work, the isolated Chromium renderer does not compile or execute native Dart widgets. The agent should create a self-contained HTML/CSS responsive companion preview for visual design review, then render it at the requested desktop/tablet/mobile viewport and inspect the returned PNG. Keep Dart source as the production implementation and do not label the HTML companion as a native Flutter screenshot. Native Flutter capture requires an explicitly configured Flutter SDK and emulator/device runner.
