#!/usr/bin/env python3
"""Scale/ANN regression test: migration, signature backfill, candidate reduction and exact fallback."""
from __future__ import annotations

import hashlib
import json
import math
import os
import random
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import statistics
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from performance_metrics import path_recall_at_k, summarize_latency  # noqa: E402

DIMENSIONS = 128
DOC_COUNT = 5200
MODEL_ID = "ann-fixture-128"


def sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def run(cli: str, *args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run([cli, *args], text=True, capture_output=True, env=env, timeout=90)
    if proc.returncode != 0:
        raise AssertionError(f"failed: {proc.args}\ncode={proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}")
    return proc


class Handler(BaseHTTPRequestHandler):
    def log_message(self, _format: str, *args: object) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        payload: dict[str, Any] = json.loads(self.rfile.read(length))
        if payload.get("model") != MODEL_ID:
            self.send_error(400, "unexpected embedding model")
            return
        vector = [1.0] + [0.0] * (DIMENSIONS - 1)
        body = json.dumps({"data": [{"object": "embedding", "index": 0, "embedding": vector}],
                           "model": MODEL_ID, "usage": {"prompt_tokens": 1, "total_tokens": 1}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_memory_ann.py ASGEND_MEMORY_CLI")
    cli = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="asgend-ann-test-") as td:
        root = Path(td) / "project"
        root.mkdir()
        (root / "seed.md").write_text("# ANN seed\nThis physical file creates the memory schema.\n", encoding="utf-8")
        run(cli, "scan", str(root))
        database = root / ".asgend_mem" / "index.sqlite3"
        connection = sqlite3.connect(database)
        try:
            schema = connection.execute("PRAGMA user_version").fetchone()[0]
            assert schema == 9, f"expected schema v9, got {schema}"
            rng = random.Random(334455)
            files = []
            descriptions = []
            vectors = []
            base_id = connection.execute("SELECT COALESCE(MAX(file_id),0) FROM files").fetchone()[0]
            for i in range(DOC_COUNT):
                relative = f"synthetic/{i:05d}.md"
                source_hash = sha(f"synthetic-source-{i}")
                title = f"Synthetic vector document {i:05d}"
                description = f"ANN scaling fixture document number {i:05d} about vector nearest neighbour search."
                file_id = base_id + i + 1
                files.append((file_id, relative, "md", "text/markdown", len(description), i + 1,
                              source_hash, "present", 0))
                descriptions.append((file_id, source_hash, "test-generator", "test-v1", title, description, "complete"))
                if i == 0:
                    vector = [1.0] + [0.0] * (DIMENSIONS - 1)
                else:
                    raw = [rng.gauss(0.0, 1.0) for _ in range(DIMENSIONS)]
                    norm = math.sqrt(sum(value * value for value in raw))
                    vector = [value / norm for value in raw]
                vectors.append((file_id, sha(title + "\n" + description), MODEL_ID, DIMENSIONS,
                                json.dumps(vector, separators=(",", ":"))))
            connection.execute("BEGIN")
            connection.executemany(
                "INSERT INTO files(file_id,relative_path,extension,mime_type,file_size,mtime_ns,sha256,status,last_seen_scan_id) VALUES(?,?,?,?,?,?,?,?,?)",
                files,
            )
            connection.executemany(
                "INSERT INTO descriptions(file_id,source_sha256,model_id,prompt_version,title,description,status) VALUES(?,?,?,?,?,?,?)",
                descriptions,
            )
            desc_ids = connection.execute(
                "SELECT description_id,file_id,input_sha256 FROM (SELECT d.description_id,d.file_id,'' AS input_sha256 FROM descriptions d WHERE d.model_id='test-generator' ORDER BY d.file_id)"
            ).fetchall()
            # Map description rows by file ID and attach vectors; the source/input hashes are independently verified at query time.
            desc_id_by_file = {int(row[1]): int(row[0]) for row in desc_ids}
            connection.executemany(
                "INSERT INTO embeddings(description_id,file_id,input_sha256,model_id,dimensions,vector_json,ann_signature) VALUES(?,?,?,?,?,?,NULL)",
                [(desc_id_by_file[file_id], file_id, input_hash, model, dims, vector_json)
                 for file_id, input_hash, model, dims, vector_json in vectors],
            )
            connection.commit()
        finally:
            connection.close()

        indexed = run(cli, "ann-index", str(root))
        index_report = json.loads(indexed.stdout)
        assert index_report["algorithm"] == "random-hyperplane-lsh16-radius4", index_report
        assert index_report["document_vectors_indexed"] == DOC_COUNT, index_report
        assert index_report["document_vectors_remaining"] == 0, index_report
        connection = sqlite3.connect(database)
        try:
            null_count = connection.execute("SELECT COUNT(*) FROM embeddings WHERE ann_signature IS NULL").fetchone()[0]
            signature_count = connection.execute("SELECT COUNT(*) FROM embeddings WHERE length(ann_signature)=4").fetchone()[0]
            assert null_count == 0 and signature_count == DOC_COUNT, (null_count, signature_count)
        finally:
            connection.close()

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = os.environ.copy()
        env["ASGEND_MEMORY_EMBEDDING_BASE_URL"] = f"http://127.0.0.1:{server.server_port}/v1"
        env["ASGEND_MEMORY_EMBEDDING_MODEL"] = MODEL_ID
        try:
            # One warm-up per mode, followed by alternating paired order to reduce
            # fixed-order/cache effects. These are end-to-end CLI timings and include
            # local mock-embedding HTTP round trips, not pure search-kernel times.
            for mode, exact in (("exact", True), ("ann", False)):
                run(cli, "query", str(root), "query has no lexical matches", "--limit", "5",
                    *( ["--exact-vectors"] if exact else [] ), env=env)
            samples: dict[str, list[float]] = {"exact": [], "ann": []}
            measured: dict[str, dict[str, Any]] = {}
            paired_speedups: list[float] = []
            for repetition in range(5):
                order = (("exact", True), ("ann", False)) if repetition % 2 == 0 else (("ann", False), ("exact", True))
                pair: dict[str, float] = {}
                for mode, exact in order:
                    args = ["query", str(root), "query has no lexical matches", "--limit", "5"]
                    if exact:
                        args.append("--exact-vectors")
                    started = time.perf_counter()
                    result = json.loads(run(cli, *args, env=env).stdout)
                    elapsed_ms = (time.perf_counter() - started) * 1000.0
                    samples[mode].append(elapsed_ms)
                    pair[mode] = elapsed_ms
                    measured[mode] = result
                paired_speedups.append(pair["exact"] / pair["ann"] if pair["ann"] > 0 else 0.0)

            ann_result = measured["ann"]
            exact_result = measured["exact"]
            assert ann_result["vector_search"] == "ann_lsh16", ann_result.get("vector_search")
            assert ann_result["document_ann_used"] is True, ann_result
            assert 32 <= ann_result["document_ann_candidates"] < DOC_COUNT // 5, ann_result["document_ann_candidates"]
            assert ann_result["results"] and ann_result["results"][0]["path"] == "synthetic/00000.md", ann_result["results"][:1]
            assert exact_result["vector_search"] == "exact_cosine", exact_result.get("vector_search")
            assert exact_result["document_ann_used"] is False
            assert exact_result["results"] and exact_result["results"][0]["path"] == "synthetic/00000.md", exact_result["results"][:1]

            def summary(values: list[float]) -> dict[str, Any]:
                return summarize_latency(values, decimals=3)
            exact_paths = {str(item.get("path", "")) for item in exact_result.get("results", []) if isinstance(item, dict)}
            ann_paths = {str(item.get("path", "")) for item in ann_result.get("results", []) if isinstance(item, dict)}
            print(json.dumps({
                "benchmark": "memory_ann_synthetic_fixture",
                "documents": DOC_COUNT, "dimensions": DIMENSIONS, "top_k": 5,
                "warmup_per_mode": 1, "measured_pairs": len(paired_speedups),
                "latency_scope": "end-to-end CLI wall time including process startup and local mock embedding HTTP round trip",
                "exact_latency_ms": summary(samples["exact"]), "ann_latency_ms": summary(samples["ann"]),
                "median_paired_speedup": round(statistics.median(paired_speedups), 3),
                "ann_candidates": ann_result["document_ann_candidates"],
                "candidate_ratio": round(ann_result["document_ann_candidates"] / DOC_COUNT, 4),
                "path_recall_at_5": round(path_recall_at_k(list(exact_paths), list(ann_paths), 5), 4),
                "correct_top_result": True,
            }, ensure_ascii=False, sort_keys=True))
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    main()
