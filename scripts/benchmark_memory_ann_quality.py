#!/usr/bin/env python3
"""Benchmark labelled ANN retrieval quality and end-to-end latency on a deterministic corpus.

The 5,200-vector corpus is synthetic. Eight query topics have four manually judged relevant
paths each; 5,168 deterministic distractors provide scale. A local loopback embedding server
maps each unique query to a known dense prototype. Exact cosine ranks are the approximate
search reference, while the independent judged labels are the quality ground truth.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import statistics
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path, PurePosixPath
from typing import Any

MODEL_ID = "asgend-ann-quality-fixture-v1"
DIMENSIONS = 128
DEFAULT_SEED = 340034


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def normalize(values: list[float]) -> list[float]:
    norm = math.sqrt(sum(value * value for value in values))
    if not math.isfinite(norm) or norm <= 0.0:
        raise ValueError("fixture generator produced an invalid vector norm")
    return [value / norm for value in values]


def orthonormal_basis(count: int, dimensions: int, rng: random.Random) -> list[list[float]]:
    basis: list[list[float]] = []
    for _ in range(count):
        vector = [rng.gauss(0.0, 1.0) for _ in range(dimensions)]
        for previous in basis:
            dot = sum(a * b for a, b in zip(vector, previous))
            vector = [a - dot * b for a, b in zip(vector, previous)]
        basis.append(normalize(vector))
    return basis


def topic_prototypes(seed: int, topic_count: int) -> list[list[float]]:
    rng = random.Random(seed + 41)
    basis = orthonormal_basis(topic_count + 1, DIMENSIONS, rng)
    shared = 0.70
    unique = math.sqrt(1.0 - shared * shared)
    return [normalize([shared * basis[0][i] + unique * basis[topic + 1][i]
                       for i in range(DIMENSIONS)]) for topic in range(topic_count)]


def validate_dataset(dataset: dict[str, Any]) -> None:
    if dataset.get("version") != 1 or not isinstance(dataset.get("queries"), list) or not dataset["queries"]:
        raise ValueError("dataset must contain version=1 and a non-empty queries array")
    queries = dataset["queries"]
    ids = [query.get("id") for query in queries]
    texts = [query.get("query") for query in queries]
    if any(not isinstance(value, str) or not value for value in ids + texts) or len(set(ids)) != len(ids) or len(set(texts)) != len(texts):
        raise ValueError("query IDs and texts must be non-empty and unique")
    if [query.get("topic_index") for query in queries] != list(range(len(queries))):
        raise ValueError("topic_index must be the contiguous sequence 0..query_count-1")
    all_paths: list[str] = []
    for query in queries:
        if query.get("k") != 5 or not isinstance(query.get("relevant_paths"), list) or not query["relevant_paths"]:
            raise ValueError(f"query {query.get('id')} requires k=5 and relevant_paths")
        for path in query["relevant_paths"]:
            if not isinstance(path, str) or "\\" in path:
                raise ValueError(f"invalid labelled path: {path!r}")
            parsed = PurePosixPath(path)
            if parsed.is_absolute() or ".." in parsed.parts or not path.startswith("judged/") or parsed.suffix != ".md":
                raise ValueError(f"labelled path is not a safe judged Markdown path: {path!r}")
            all_paths.append(path)
    if len(all_paths) != len(set(all_paths)):
        raise ValueError("relevant paths must have exactly one judged topic")
    corpus = dataset.get("corpus")
    if not isinstance(corpus, dict) or int(corpus.get("dimensions", -1)) != DIMENSIONS or int(corpus.get("document_vectors", 0)) < 5000:
        raise ValueError("corpus requires dimensions=128 and at least 5000 vector documents")
    expected_relevant = len(queries) * int(corpus.get("relevant_documents_per_topic", 0))
    if len(all_paths) != expected_relevant or int(corpus.get("distractors", -1)) != int(corpus["document_vectors"]) - expected_relevant:
        raise ValueError("dataset relevance labels and corpus counts do not balance")
    if dataset.get("acceptance_criteria", {}).get("max_median_ann_candidate_ratio", 1) > 1:
        raise ValueError("candidate ratio acceptance threshold cannot exceed 1")


def build_corpus(root: Path, dataset: dict[str, Any]) -> tuple[list[dict[str, Any]], list[list[float]], dict[str, list[float]]]:
    corpus_config = dataset["corpus"]
    topic_count = len(dataset["queries"])
    expected = int(corpus_config["document_vectors"])
    relevant_paths = {path for query in dataset["queries"] for path in query["relevant_paths"]}
    if len(relevant_paths) != topic_count * int(corpus_config["relevant_documents_per_topic"]):
        raise ValueError("dataset must have unique, complete relevant path labels")
    if len(relevant_paths) >= expected:
        raise ValueError("relevant document count must be lower than corpus vector count")

    seed = int(corpus_config.get("seed", DEFAULT_SEED))
    rng = random.Random(seed)
    prototypes = topic_prototypes(seed, topic_count)
    query_vectors = {str(query["query"]): prototypes[int(query["topic_index"])] for query in dataset["queries"]}
    paths: list[str] = []
    topic_by_path: dict[str, int] = {}
    for query in dataset["queries"]:
        for path in query["relevant_paths"]:
            if path in topic_by_path:
                raise ValueError(f"duplicate judged path: {path}")
            topic_by_path[path] = int(query["topic_index"])
            paths.append(path)
    while len(paths) < expected:
        paths.append(f"distractors/document-{len(paths):05d}.md")

    generated: list[dict[str, Any]] = []
    for index, relative in enumerate(paths):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        # Deliberately avoid terms used by the judged query strings.
        source = f"# Evaluation item {index:05d}\nSynthetic benchmark source record number {index:05d}.\n"
        source_bytes = source.encode("utf-8")
        path.write_bytes(source_bytes)
        topic = topic_by_path.get(relative)
        if topic is not None:
            vector = [prototypes[topic][i] + rng.gauss(0.0, 0.012) for i in range(DIMENSIONS)]
            vector = normalize(vector)
            label = f"topic-{topic:02d}"
        else:
            vector = normalize([rng.gauss(0.0, 1.0) for _ in range(DIMENSIONS)])
            label = "distractor"
        generated.append({
            "relative_path": relative,
            "source_sha256": sha256_bytes(source_bytes),
            "source_size_bytes": len(source_bytes),
            "label": label,
            "topic_index": topic,
            "vector_json": json.dumps(vector, separators=(",", ":"), allow_nan=False),
        })
    if len(generated) != expected:
        raise AssertionError(f"expected {expected} generated documents, got {len(generated)}")
    return generated, prototypes, query_vectors


def metrics(ranked: list[str], relevant: set[str], k: int) -> dict[str, float]:
    top = ranked[:k]
    hits = [1 if path in relevant else 0 for path in top]
    count = sum(hits)
    first = next((index for index, hit in enumerate(hits, 1) if hit), None)
    dcg = sum(1.0 / math.log2(rank + 1) for rank, hit in enumerate(hits, 1) if hit)
    ideal = min(k, len(relevant))
    idcg = sum(1.0 / math.log2(rank + 1) for rank in range(1, ideal + 1))
    return {
        f"precision@{k}": count / k,
        f"recall@{k}": count / len(relevant) if relevant else 0.0,
        f"mrr@{k}": 1.0 / first if first else 0.0,
        f"ndcg@{k}": dcg / idcg if idcg else 0.0,
        f"hits@{k}": float(count),
    }


def summary(values: list[float]) -> dict[str, float]:
    if not values:
        raise ValueError("cannot summarize an empty measurement list")
    ordered = sorted(values)
    p95 = ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]
    return {
        "count": len(values),
        "min": round(ordered[0], 3),
        "median": round(float(statistics.median(ordered)), 3),
        "p95_nearest_rank": round(p95, 3),
        "max": round(ordered[-1], 3),
    }


def run(cli: str, root: Path, query: str, k: int, exact: bool, env: dict[str, str], timeout: float) -> tuple[dict[str, Any], float]:
    command = [cli, "query", str(root), query, "--limit", str(k), "--context-tokens", "100000"]
    if exact:
        command.append("--exact-vectors")
    start = time.perf_counter()
    proc = subprocess.run(command, text=True, capture_output=True, env=env, timeout=timeout)
    elapsed = (time.perf_counter() - start) * 1000.0
    if proc.returncode != 0:
        raise RuntimeError(f"query failed with code {proc.returncode}: {command!r}\n{proc.stderr.strip()}")
    try:
        result = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"query returned invalid JSON: {exc}") from exc
    if not isinstance(result, dict) or not isinstance(result.get("results"), list):
        raise RuntimeError("query must return an object with a results array")
    return result, elapsed


def create_fixture_archive(root: Path, dataset_path: Path, archive_path: Path) -> None:
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive_path, "w:gz") as archive:
        for source in sorted(root.rglob("*.md")):
            if ".asgend_mem" in source.parts:
                continue
            archive.add(source, arcname=f"corpus/{source.relative_to(root).as_posix()}", recursive=False)
        archive.add(dataset_path, arcname="dataset/ann_quality_judged_dataset.json", recursive=False)


class EmbeddingHandler(BaseHTTPRequestHandler):
    model_id = MODEL_ID
    query_vectors: dict[str, list[float]] = {}
    state_lock = threading.Lock()
    request_inputs: list[str] = []
    errors: list[str] = []

    def log_message(self, _format: str, *args: object) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length))
            if payload.get("model") != self.model_id:
                raise ValueError("unexpected model ID")
            query = payload.get("input")
            if not isinstance(query, str) or query not in self.query_vectors:
                raise ValueError("unexpected query input")
            vector = self.query_vectors[query]
            with self.state_lock:
                self.request_inputs.append(query)
            body = json.dumps({"data": [{"object": "embedding", "index": 0, "embedding": vector}],
                               "model": self.model_id, "usage": {"prompt_tokens": 1, "total_tokens": 1}},
                              separators=(",", ":")).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception as exc:  # test server reports unexpected input as a recorded failure
            with self.state_lock:
                self.errors.append(str(exc))
            try:
                self.send_error(400, "invalid fixture request")
            except (BrokenPipeError, ConnectionResetError):
                pass


def build_index(cli: str, root: Path, records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    scan = subprocess.run([cli, "scan", str(root)], text=True, capture_output=True, timeout=60)
    if scan.returncode != 0:
        raise RuntimeError(f"scan failed: {scan.stderr.strip()}")
    database = root / ".asgend_mem" / "index.sqlite3"
    with sqlite3_connect(database) as db:
        rows = db.execute("SELECT file_id,relative_path,sha256 FROM files WHERE deleted_at IS NULL").fetchall()
        by_path = {str(path): (int(file_id), str(digest)) for file_id, path, digest in rows}
        if len(by_path) < len(records):
            raise RuntimeError(f"scan indexed {len(by_path)} paths, expected at least {len(records)}")
        description_rows = []
        embedding_rows = []
        for record in records:
            path = record["relative_path"]
            if path not in by_path:
                raise RuntimeError(f"scan did not index generated file {path}")
            file_id, source_hash = by_path[path]
            if source_hash != record["source_sha256"]:
                raise RuntimeError(f"fixture source SHA-256 mismatch for {path}")
            title = f"Evaluation item {len(description_rows):05d}"
            description = f"Synthetic evaluation corpus record number {len(description_rows):05d}; deterministic fixture." 
            input_hash = sha256_bytes((title + "\n" + description).encode("utf-8"))
            description_rows.append((file_id, source_hash, "fixture-generator-v1", "fixture-v1", title, description, "complete"))
            embedding_rows.append((file_id, input_hash, MODEL_ID, DIMENSIONS, record["vector_json"]))
        db.execute("BEGIN")
        db.executemany("INSERT INTO descriptions(file_id,source_sha256,model_id,prompt_version,title,description,status) VALUES(?,?,?,?,?,?,?)", description_rows)
        # The current schema selects the latest complete description and requires the exact input hash.
        description_ids = db.execute("SELECT description_id,file_id,input_sha256 FROM (SELECT d.description_id,d.file_id,'' AS input_sha256 FROM descriptions d WHERE d.model_id='fixture-generator-v1' ORDER BY d.file_id)").fetchall()
        desc_id_by_file = {int(file_id): int(description_id) for description_id, file_id, _ in description_ids}
        db.executemany("INSERT INTO embeddings(description_id,file_id,input_sha256,model_id,dimensions,vector_json,ann_signature) VALUES(?,?,?,?,?,?,NULL)",
                       [(desc_id_by_file[file_id], file_id, input_hash, model, dims, vector_json)
                        for file_id, input_hash, model, dims, vector_json in embedding_rows])
        db.commit()
        integrity = db.execute("PRAGMA integrity_check").fetchone()[0]
        if integrity != "ok":
            raise RuntimeError(f"SQLite integrity_check before ANN indexing: {integrity}")
    ann_index = subprocess.run([cli, "ann-index", str(root)], text=True, capture_output=True, timeout=180)
    if ann_index.returncode != 0:
        raise RuntimeError(f"ANN index build failed: {ann_index.stderr.strip()} {ann_index.stdout.strip()}")
    index_report = json.loads(ann_index.stdout)
    if index_report.get("algorithm") != "random-hyperplane-lsh16-radius4" or index_report.get("document_vectors_indexed") != len(records) or index_report.get("document_vectors_remaining") != 0:
        raise RuntimeError(f"ANN index report did not match generated corpus: {index_report}")
    with sqlite3_connect(database) as db:
        integrity = db.execute("PRAGMA integrity_check").fetchone()[0]
        if integrity != "ok":
            raise RuntimeError(f"SQLite integrity_check after ANN indexing: {integrity}")
        manifest_rows = db.execute(
            "SELECT f.relative_path,f.sha256,e.input_sha256,e.model_id,e.dimensions,e.vector_json,e.ann_signature "
            "FROM embeddings e JOIN files f ON f.file_id=e.file_id JOIN descriptions d ON d.description_id=e.description_id "
            "WHERE d.model_id='fixture-generator-v1' ORDER BY f.relative_path"
        ).fetchall()
        manifest = []
        label_map = {record["relative_path"]: record["label"] for record in records}
        for path, source_hash, input_hash, model_id, dimensions, vector_json, signature in manifest_rows:
            manifest.append({
                "relative_path": str(path), "source_sha256": str(source_hash), "input_sha256": str(input_hash),
                "model_id": str(model_id), "dimensions": int(dimensions),
                "vector_json_sha256": sha256_bytes(str(vector_json).encode("utf-8")),
                "ann_signature": signature, "label": label_map[str(path)],
            })
        if len(manifest) != len(records):
            raise RuntimeError(f"DB-derived manifest contains {len(manifest)} rows, expected {len(records)}")
        return manifest


class sqlite3_connect:
    """Tiny context manager that commits no writes implicitly and always closes the connection."""
    def __init__(self, path: Path):
        import sqlite3
        self.sqlite3 = sqlite3
        self.path = path
        self.connection = None

    def __enter__(self):
        self.connection = self.sqlite3.connect(self.path)
        return self.connection

    def __exit__(self, exc_type, exc, tb):
        if self.connection is not None:
            self.connection.close()
        return False


def write_json(path: Path | None, value: Any) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="path to asgend-memory executable")
    parser.add_argument("--dataset", required=True, help="judged dataset JSON")
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-md", type=Path)
    parser.add_argument("--output-manifest", type=Path)
    parser.add_argument("--fixture-archive", type=Path)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout-seconds", type=float, default=45.0)
    parser.add_argument("--smoke", action="store_true", help="two queries, one measured pair, no warm-ups")
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 50 or not 0 <= args.warmup <= 3 or args.timeout_seconds <= 0:
        parser.error("repetitions must be 1..50, warmup 0..3, timeout positive")
    if args.smoke:
        args.repetitions = 1
        args.warmup = 0
    binary_path = Path(args.binary).resolve()
    dataset_path = Path(args.dataset).resolve()
    try:
        dataset = json.loads(dataset_path.read_text(encoding="utf-8"))
        if not isinstance(dataset, dict):
            raise ValueError("invalid judged dataset root")
        validate_dataset(dataset)
        all_queries = dataset["queries"]
        expected_vectors = int(dataset["corpus"]["document_vectors"])
        if expected_vectors < 5000:
            raise ValueError("corpus must contain at least 5000 vectors to trigger the production ANN path")
        queries = all_queries[:2] if args.smoke else all_queries
        if args.smoke:
            # Keep all labelled paths/topics in the corpus; only shorten measured query workload.
            pass
        output_path = args.output_json.resolve()
        output_md = args.output_md.resolve() if args.output_md else None
        manifest_path = args.output_manifest.resolve() if args.output_manifest else None
        fixture_archive = args.fixture_archive.resolve() if args.fixture_archive else None
        env = os.environ.copy()
        # Never inherit provider credentials/endpoints or planner/reranker connections.
        for name in (
            "ASGEND_MEMORY_EMBEDDING_BASE_URL", "ASGEND_MEMORY_EMBEDDING_MODEL", "ASGEND_MEMORY_EMBEDDING_API_KEY",
            "ASGEND_MEMORY_DECISION_BASE_URL", "ASGEND_MEMORY_DECISION_MODEL", "ASGEND_MEMORY_DECISION_API_KEY",
            "ASGEND_MEMORY_RERANK_BASE_URL", "ASGEND_MEMORY_RERANK_MODEL", "ASGEND_MEMORY_RERANK_API_KEY",
        ):
            env.pop(name, None)
        env["NO_PROXY"] = "127.0.0.1,localhost"
        env["no_proxy"] = "127.0.0.1,localhost"
        with tempfile.TemporaryDirectory(prefix="asgend-ann-quality-") as temp:
            root = Path(temp) / "project"
            root.mkdir()
            (root / "seed.md").write_text("# ANN quality fixture seed\nSchema bootstrap only.\n", encoding="utf-8")
            records, _prototypes, query_vectors = build_corpus(root, dataset)
            db_manifest = build_index(str(binary_path), root, records)
            if len(db_manifest) != expected_vectors:
                raise RuntimeError(f"manifest has {len(db_manifest)} vectors, expected {expected_vectors}")
            if fixture_archive:
                create_fixture_archive(root, dataset_path, fixture_archive)
            server_class = type("AnnQualityEmbeddingHandler", (EmbeddingHandler,), {})
            server_class.query_vectors = query_vectors
            server_class.request_inputs = []
            server_class.errors = []
            server = ThreadingHTTPServer(("127.0.0.1", 0), server_class)
            thread = threading.Thread(target=server.serve_forever, name="ann-quality-loopback", daemon=True)
            thread.start()
            env["ASGEND_MEMORY_EMBEDDING_BASE_URL"] = f"http://127.0.0.1:{server.server_port}/v1"
            env["ASGEND_MEMORY_EMBEDDING_MODEL"] = MODEL_ID
            env.pop("ASGEND_MEMORY_EMBEDDING_API_KEY", None)
            per_query: list[dict[str, Any]] = []
            exact_latencies: list[float] = []
            ann_latencies: list[float] = []
            ann_candidate_ratios: list[float] = []
            try:
                # Warm-up is paired and order-balanced; measured pairs also alternate order.
                for query_record in queries:
                    for warm_index in range(args.warmup):
                        order = (("exact", True), ("ann", False)) if warm_index % 2 == 0 else (("ann", False), ("exact", True))
                        for _mode, exact in order:
                            run(str(binary_path), root, query_record["query"], int(query_record["k"]), exact, env, args.timeout_seconds)
                for query_record in queries:
                    k = int(query_record["k"])
                    relevant = set(query_record["relevant_paths"])
                    samples: dict[str, list[float]] = {"exact": [], "ann": []}
                    ranked: dict[str, list[str]] = {"exact": [], "ann": []}
                    metadata: dict[str, dict[str, Any]] = {}
                    paired_speedups: list[float] = []
                    paired_candidates: list[int] = []
                    for repetition in range(args.repetitions):
                        order = (("exact", True), ("ann", False)) if repetition % 2 == 0 else (("ann", False), ("exact", True))
                        pair: dict[str, float] = {}
                        for mode, exact in order:
                            result, elapsed_ms = run(str(binary_path), root, query_record["query"], k, exact, env, args.timeout_seconds)
                            paths = [str(item.get("path", "")) for item in result["results"] if isinstance(item, dict)]
                            if not paths or any(not path for path in paths):
                                raise RuntimeError(f"{mode} returned no paths for {query_record['id']}")
                            if len(paths) != len(set(paths)):
                                raise RuntimeError(f"{mode} returned duplicate paths for {query_record['id']}")
                            if mode == "ann":
                                if result.get("vector_search") != "ann_lsh16" or result.get("document_ann_used") is not True:
                                    raise RuntimeError(f"ANN path did not activate for {query_record['id']}: {result.get('vector_search')}")
                                candidates = int(result.get("document_ann_candidates", 0))
                                vector_count = int(result.get("document_vector_count", 0))
                                if candidates < 32 or candidates >= vector_count:
                                    raise RuntimeError(f"invalid candidate count {candidates}/{vector_count} for {query_record['id']}")
                                paired_candidates.append(candidates)
                                ann_candidate_ratios.append(candidates / max(1, vector_count))
                            else:
                                if result.get("vector_search") != "exact_cosine" or result.get("document_ann_used") is not False:
                                    raise RuntimeError(f"exact-vector path did not activate for {query_record['id']}")
                            samples[mode].append(elapsed_ms)
                            pair[mode] = elapsed_ms
                            ranked[mode] = paths
                            metadata[mode] = {
                                "vector_search": result.get("vector_search"),
                                "document_ann_used": result.get("document_ann_used"),
                                "document_vector_count": int(result.get("document_vector_count", 0)),
                                "document_ann_candidates": int(result.get("document_ann_candidates", 0)),
                                "chunk_vector_count": int(result.get("chunk_vector_count", 0)),
                                "chunk_ann_candidates": int(result.get("chunk_ann_candidates", 0)),
                            }
                        paired_speedups.append(pair["exact"] / pair["ann"] if pair["ann"] > 0 else 0.0)
                    exact_metrics = metrics(ranked["exact"], relevant, k)
                    ann_metrics = metrics(ranked["ann"], relevant, k)
                    exact_relevant = [path for path in ranked["exact"] if path in relevant]
                    ann_relevant = [path for path in ranked["ann"] if path in relevant]
                    overlap = len(set(exact_relevant[:k]) & set(ann_relevant[:k])) / max(1, len(set(exact_relevant[:k])))
                    per_query.append({
                        "id": query_record["id"], "query": query_record["query"], "k": k,
                        "relevant_paths": sorted(relevant),
                        "exact": {"ranked_paths": ranked["exact"], "metrics": exact_metrics,
                                  "latency_ms": summary(samples["exact"]), "raw_latency_samples_ms": samples["exact"], **metadata["exact"]},
                        "ann": {"ranked_paths": ranked["ann"], "metrics": ann_metrics,
                                "latency_ms": summary(samples["ann"]), "raw_latency_samples_ms": samples["ann"],
                                "median_paired_exact_over_ann": round(float(statistics.median(paired_speedups)), 4),
                                "paired_candidate_counts": paired_candidates, **metadata["ann"]},
                        "ann_relevant_overlap_with_exact_at_k": round(overlap, 6),
                    })
                expected_requests = len(queries) * (args.warmup + args.repetitions) * 2
                with server_class.state_lock:
                    request_count = len(server_class.request_inputs)
                    request_errors = list(server_class.errors)
                if request_errors:
                    raise RuntimeError(f"mock server recorded invalid inputs: {request_errors[:3]}")
                if request_count != expected_requests:
                    raise RuntimeError(f"expected {expected_requests} embedding requests, observed {request_count}")
                with sqlite3_connect(root / ".asgend_mem" / "index.sqlite3") as db:
                    integrity = str(db.execute("PRAGMA integrity_check").fetchone()[0])
                    schema_version = int(db.execute("PRAGMA user_version").fetchone()[0])
                    indexed_vectors = int(db.execute("SELECT COUNT(*) FROM embeddings WHERE model_id=?", (MODEL_ID,)).fetchone()[0])
                    missing_signatures = int(db.execute("SELECT COUNT(*) FROM embeddings WHERE model_id=? AND ann_signature IS NULL", (MODEL_ID,)).fetchone()[0])
                if integrity != "ok" or indexed_vectors != expected_vectors or missing_signatures:
                    raise RuntimeError(f"post-run DB verification failed: integrity={integrity}, vectors={indexed_vectors}, missing_signatures={missing_signatures}")
                criteria = dataset["acceptance_criteria"]
                def mean_metric(mode: str, metric_name: str) -> float:
                    return statistics.mean(float(row[mode]["metrics"][metric_name]) for row in per_query)
                mean_exact_recall = mean_metric("exact", "recall@5")
                mean_ann_recall = mean_metric("ann", "recall@5")
                mean_ann_mrr = mean_metric("ann", "mrr@5")
                mean_ann_ndcg = mean_metric("ann", "ndcg@5")
                median_candidate_ratio = statistics.median(ann_candidate_ratios)
                acceptance = {
                    "mean_exact_recall_at_5": {"actual": round(mean_exact_recall, 6), "minimum": criteria["min_mean_exact_recall_at_k"], "pass": mean_exact_recall >= criteria["min_mean_exact_recall_at_k"]},
                    "mean_ann_recall_at_5": {"actual": round(mean_ann_recall, 6), "minimum": criteria["min_mean_ann_recall_at_k"], "pass": mean_ann_recall >= criteria["min_mean_ann_recall_at_k"]},
                    "mean_ann_mrr_at_5": {"actual": round(mean_ann_mrr, 6), "minimum": criteria["min_mean_ann_mrr_at_k"], "pass": mean_ann_mrr >= criteria["min_mean_ann_mrr_at_k"]},
                    "mean_ann_ndcg_at_5": {"actual": round(mean_ann_ndcg, 6), "minimum": criteria["min_mean_ann_ndcg_at_k"], "pass": mean_ann_ndcg >= criteria["min_mean_ann_ndcg_at_k"]},
                    "median_ann_candidate_ratio": {"actual": round(median_candidate_ratio, 6), "maximum": criteria["max_median_ann_candidate_ratio"], "pass": median_candidate_ratio <= criteria["max_median_ann_candidate_ratio"]},
                    "all_queries_minimum_ann_candidates": {"actual_minimum": min(int(row["ann"]["document_ann_candidates"]) for row in per_query), "minimum": criteria["min_ann_candidates_per_query"], "pass": min(int(row["ann"]["document_ann_candidates"]) for row in per_query) >= criteria["min_ann_candidates_per_query"]},
                    "all_queries_maximum_ann_candidate_ratio": {"actual_maximum": round(max(float(row["ann"]["document_ann_candidates"]) / float(row["ann"]["document_vector_count"]) for row in per_query), 6), "maximum": criteria["max_ann_candidates_per_query_ratio"], "pass": max(float(row["ann"]["document_ann_candidates"]) / float(row["ann"]["document_vector_count"]) for row in per_query) <= criteria["max_ann_candidates_per_query_ratio"]},
                    "sqlite_integrity": {"actual": integrity, "pass": integrity == "ok"},
                    "request_accounting": {"actual": request_count, "expected": expected_requests, "pass": request_count == expected_requests},
                }
                overall_pass = all(item["pass"] for item in acceptance.values())
                output = {
                    "status": "complete" if overall_pass else "criteria_failed",
                    "benchmark": "asgend-memory-labelled-ann-quality-v1",
                    "retrieval_algorithm": "random-hyperplane-lsh16-radius4",
                    "synthetic_only": True,
                    "live_provider_used": False,
                    "embedding_endpoint": "local 127.0.0.1 loopback mock; no API key or external network",
                    "latency_scope": "end-to-end asgend-memory CLI wall latency including process startup, loopback embedding HTTP, probe setup and SQLite search; not pure kernel latency",
                    "dataset_id": dataset["dataset_id"], "dataset_sha256": sha256_bytes(dataset_path.read_bytes()),
                    "harness_sha256": sha256_bytes(Path(__file__).read_bytes()),
                    "binary_sha256": sha256_bytes(binary_path.read_bytes()),
                    "model_id": MODEL_ID, "dimensions": DIMENSIONS, "document_vectors": expected_vectors,
                    "distractor_vectors": expected_vectors - len({p for q in all_queries for p in q["relevant_paths"]}),
                    "judged_query_count": len(per_query), "warmup_pairs_per_query": args.warmup,
                    "measured_pairs_per_query": args.repetitions,
                    "measured_mode_queries": len(per_query) * args.repetitions * 2,
                    "embedding_requests_expected": expected_requests, "embedding_requests_observed": request_count,
                    "request_errors": request_errors, "sqlite_integrity": integrity, "schema_version": schema_version,
                    "indexed_vectors": indexed_vectors, "ann_signatures_missing": missing_signatures,
                    "corpus_manifest_sha256": sha256_bytes(canonical_json(db_manifest)),
                    "corpus_manifest_rows": len(db_manifest),
                    "aggregate": {
                        "exact": {metric: round(statistics.mean(row["exact"]["metrics"][metric] for row in per_query), 6) for metric in per_query[0]["exact"]["metrics"]},
                        "ann": {metric: round(statistics.mean(row["ann"]["metrics"][metric] for row in per_query), 6) for metric in per_query[0]["ann"]["metrics"]},
                        "median_exact_latency_ms": round(float(statistics.median(v for row in per_query for v in row["exact"]["raw_latency_samples_ms"])), 3),
                        "median_ann_latency_ms": round(float(statistics.median(v for row in per_query for v in row["ann"]["raw_latency_samples_ms"])), 3),
                        "p95_exact_latency_ms": summary([v for row in per_query for v in row["exact"]["raw_latency_samples_ms"]])["p95_nearest_rank"],
                        "p95_ann_latency_ms": summary([v for row in per_query for v in row["ann"]["raw_latency_samples_ms"]])["p95_nearest_rank"],
                        "median_paired_exact_over_ann": round(float(statistics.median(row["ann"]["median_paired_exact_over_ann"] for row in per_query)), 4),
                        "mean_ann_relevant_overlap_with_exact_at_k": round(statistics.mean(row["ann_relevant_overlap_with_exact_at_k"] for row in per_query), 6),
                        "median_candidate_ratio": round(float(statistics.median(ann_candidate_ratios)), 6),
                    },
                    "acceptance": acceptance,
                    "acceptance_criteria_source": "tests/fixtures/ann_quality_judged_dataset.json; frozen before results were measured",
                    "queries": per_query,
                    "notes": "Rank metrics use binary judged paths. Exact cosine is a retrieval comparison baseline, not the label oracle; relevance labels are the separate dataset paths. Synthetic geometry is intentionally clusterable and does not establish real-world semantic quality.",
                }
                output_path.parent.mkdir(parents=True, exist_ok=True)
                write_json(output_path, output)
                if output_md:
                    lines = [
                        "# Stage 34 — ANN Retrieval Quality & Performance Benchmark", "",
                        f"- Status: **{output['status']}**", f"- Judged queries: {len(per_query)}; corpus vectors: {expected_vectors:,}",
                        f"- Dataset SHA-256: `{output['dataset_sha256']}`", f"- Binary SHA-256: `{output['binary_sha256']}`",
                        f"- Harness SHA-256: `{output['harness_sha256']}`", "- Provider: local loopback mock only; no live provider used.", "",
                        "## Aggregate quality / latency", "",
                        "| Mode | Precision@5 | Recall@5 | MRR@5 | nDCG@5 | Median latency (ms) | p95 latency (ms) |", "|---|---:|---:|---:|---:|---:|---:|",
                    ]
                    for mode, label in (("exact", "Exact cosine"), ("ann", "ANN LSH16/radius4")):
                        row = output["aggregate"][mode]
                        med = output["aggregate"][f"median_{mode}_latency_ms"]
                        p95 = output["aggregate"][f"p95_{mode}_latency_ms"]
                        lines.append(f"| {label} | {row['precision@5']:.4f} | {row['recall@5']:.4f} | {row['mrr@5']:.4f} | {row['ndcg@5']:.4f} | {med:.3f} | {p95:.3f} |")
                    lines.extend(["", "## Query-level results", "", "| Query | Exact Recall@5 | ANN Recall@5 | ANN MRR@5 | ANN nDCG@5 | Candidates / vectors | Median exact / ANN latency ratio |", "|---|---:|---:|---:|---:|---:|---:|"])
                    for row in per_query:
                        lines.append(f"| {row['id']} | {row['exact']['metrics']['recall@5']:.4f} | {row['ann']['metrics']['recall@5']:.4f} | {row['ann']['metrics']['mrr@5']:.4f} | {row['ann']['metrics']['ndcg@5']:.4f} | {row['ann']['document_ann_candidates']:,} / {row['ann']['document_vector_count']:,} | {row['ann']['median_paired_exact_over_ann']:.3f}× |")
                    lines.extend(["", "## Acceptance criteria", ""])
                    for name, result in acceptance.items():
                        actual = result.get("actual", result.get("actual_minimum", result.get("actual_maximum")))
                        threshold = result.get("minimum", result.get("maximum", result.get("expected")))
                        lines.append(f"- [{'x' if result['pass'] else ' '}] `{name}`: actual `{actual}`, threshold/reference `{threshold}`.")
                    lines.extend(["", "## Methodology / caveats", "", output["latency_scope"], output["notes"], ""])
                    output_md.parent.mkdir(parents=True, exist_ok=True)
                    output_md.write_text("\n".join(lines), encoding="utf-8")
                manifest_output = {
                    "version": 1, "dataset_id": dataset["dataset_id"], "seed": int(dataset["corpus"].get("seed", DEFAULT_SEED)),
                    "model_id": MODEL_ID, "dimensions": DIMENSIONS, "vector_count": len(db_manifest), "rows": db_manifest,
                }
                write_json(manifest_path, manifest_output)
                if fixture_archive:
                    # Fixture archive is written before query execution; DB state stays private and is not packaged.
                    pass
                if not overall_pass:
                    print(json.dumps(output, ensure_ascii=False, indent=2), file=sys.stderr)
                    return 3
                print(json.dumps({"status": output["status"], "queries": len(per_query), "vectors": expected_vectors,
                                  "aggregate": output["aggregate"], "acceptance": acceptance,
                                  "json": str(output_path), "markdown": str(output_md) if output_md else None}, ensure_ascii=False))
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=3)
                if thread.is_alive():
                    raise RuntimeError("mock embedding server thread did not stop")
        return 0
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError, RuntimeError, subprocess.TimeoutExpired, tarfile.TarError) as exc:
        print(f"benchmark_memory_ann_quality: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
