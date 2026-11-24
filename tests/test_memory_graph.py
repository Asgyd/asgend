#!/usr/bin/env python3
"""Integration coverage for project-memory graph and graph-aware retrieval."""
from __future__ import annotations
import json
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cli: str, *args: str) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run([cli, *args], text=True, capture_output=True, timeout=30)
    if proc.returncode != 0:
        raise AssertionError(f"command failed: {proc.args}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    return proc


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_graph.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="asgend-memory-graph-") as tmp:
        root = Path(tmp)
        (root / "docs").mkdir()
        (root / "src").mkdir()
        (root / "README.md").write_text(
            "# Start\n\nThis file contains rareentryword and points to [the guide](docs/guide.md).\n"
            "External links do not resolve: [web](https://example.test/path).\n"
            "Wiki reference: [[docs/api]].\n", encoding="utf-8")
        (root / "docs" / "guide.md").write_text(
            "# Guide\n\nGuidance about local dependency indexing.\n", encoding="utf-8")
        (root / "docs" / "api.md").write_text("# API\n\nPublic API description.\n", encoding="utf-8")
        (root / "src" / "main.c").write_text('#include "api.h"\nint main(void) { return 0; }\n', encoding="utf-8")
        (root / "src" / "api.h").write_text("#define ANSWER 42\n", encoding="utf-8")
        run(cli, "scan", str(root))
        run(cli, "extract", str(root))
        graph = json.loads(run(cli, "graph", str(root)).stdout)
        assert graph["status"] == "complete", graph
        assert graph["nodes"] == 5, graph
        assert graph["edges"] >= 3, graph
        with sqlite3.connect(root / ".asgend_mem" / "index.sqlite3") as db:
            schema = db.execute("PRAGMA user_version").fetchone()[0]
            assert schema == 9, schema
            rows = set(db.execute("SELECT s.relative_path,t.relative_path,e.edge_type FROM file_edges e JOIN files s ON s.file_id=e.source_file_id JOIN files t ON t.file_id=e.target_file_id"))
            assert ("README.md", "docs/guide.md", "markdown_link") in rows, rows
            assert ("README.md", "docs/api.md", "wiki_link") in rows, rows
            assert ("src/main.c", "src/api.h", "source_include") in rows, rows
            assert not any("example.test" in row[1] for row in rows), rows
        query = json.loads(run(cli, "query", str(root), "rareentryword", "--lexical-only", "--limit", "10").stdout)
        assert query["mode"] in ("bm25", "bm25+graph"), query
        assert query["direct_count"] >= 1, query
        docs = {row["path"]: row for row in query["results"]}
        assert "README.md" in docs, docs
        assert "rareentryword" in docs["README.md"]["excerpt"], docs["README.md"]
        assert any(edge["path"] == "docs/guide.md" for edge in docs["README.md"]["related"]), docs["README.md"]
        assert "docs/guide.md" in docs, "linked document should be graph-expanded: " + json.dumps(query)
        classified_docs = json.loads(run(cli, "query", str(root), "how to explain rareentryword guide", "--lexical-only").stdout)
        assert classified_docs["query_classification"]["intent"] == "documentation", classified_docs
        assert classified_docs["query_classification"]["method"] == "local_heuristic", classified_docs
        classified_deps = json.loads(run(cli, "query", str(root), "what files depend on rareentryword", "--lexical-only").stdout)
        assert classified_deps["query_classification"]["intent"] == "dependency", classified_deps
        assert classified_deps["query_classification"]["graph_edge_family"] == "any", classified_deps
        guide_hit = docs["docs/guide.md"]
        assert guide_hit.get("retrieved_via") == "README.md", guide_hit
        assert guide_hit.get("graph_edge_type") == "markdown_link", guide_hit
        # Idempotent rebuild should not accumulate duplicate edges.
        first_count = graph["edges"]
        again = json.loads(run(cli, "graph", str(root)).stdout)
        assert again["edges"] == first_count, (graph, again)
        # Changing source links and rebuilding must remove stale graph edges.
        (root / "README.md").write_text("# Start\n\nrareentryword, no more local links.\n", encoding="utf-8")
        run(cli, "scan", str(root))
        run(cli, "extract", str(root))
        after = json.loads(run(cli, "graph", str(root)).stdout)
        with sqlite3.connect(root / ".asgend_mem" / "index.sqlite3") as db:
            stale = db.execute("SELECT count(*) FROM file_edges e JOIN files s ON s.file_id=e.source_file_id WHERE s.relative_path='README.md'").fetchone()[0]
            assert stale == 0, stale
        assert after["edges"] < first_count, (after, first_count)


if __name__ == "__main__":
    main()
