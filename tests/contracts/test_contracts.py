#!/usr/bin/env python3
"""ASGEND Remote API v1 contract tests (A0).

Run:  python tests/contracts/test_contracts.py      (exit code 0 on success)
 or:  python -m pytest tests/contracts               (pytest optional)

Requires: jsonschema >= 4.18 (referencing), PyYAML.
Checks:
  * every schema in contracts/v1/schemas is a valid draft 2020-12 schema and all $refs resolve;
  * every example listed in contracts/v1/examples/index.json validates; every file is listed;
  * every negative example in examples/invalid fails;
  * examples with injected unknown fields still validate (unless the schema is closed, then they fail);
  * openapi.yaml parses, is 3.1, all $refs resolve, every CONTRACT_SKETCH §6/§7 endpoint is present;
  * actual Control Plane routes/security match OpenAPI, and client endpoint references are documented;
  * contracts/v1/state_machine.json is consistent with the schemas and docs/API_CONTRACT.md;
  * reference scenarios: command redelivery + node dedupe, event ingest seq/gaps/duplicates,
    client reconnect/resume with the event cursor.
"""
from __future__ import annotations

import ast
import copy
import json
import re
import sys
import unittest
from pathlib import Path
from urllib.parse import urldefrag

try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None

from jsonschema import Draft202012Validator
from jsonschema.exceptions import ValidationError
from referencing import Registry, Resource
from referencing.jsonschema import DRAFT202012

ROOT = Path(__file__).resolve().parents[2]
V1 = ROOT / "contracts" / "v1"
SCHEMAS = V1 / "schemas"
EXAMPLES = V1 / "examples"
OPENAPI = V1 / "openapi.yaml"
SKETCH = V1 / "CONTRACT_SKETCH.md"
STATE_MACHINE = V1 / "state_machine.json"
API_DOC = ROOT / "docs" / "API_CONTRACT.md"
BASE = "https://asgend.invalid/contracts/v1/schemas/"
UNKNOWN_KEY = "x_future_field_for_tolerance_test"


def load_json(path: Path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def load_schemas():
    schemas = {}
    for p in sorted(SCHEMAS.glob("*.schema.json")):
        schemas[p.name] = load_json(p)
    return schemas


SCHEMA_DOCS = load_schemas()
REGISTRY = Registry().with_resources(
    (doc["$id"], Resource.from_contents(doc, default_specification=DRAFT202012)) for doc in SCHEMA_DOCS.values()
)


def validator_for(schema_ref: str) -> Draft202012Validator:
    return Draft202012Validator({"$ref": BASE + schema_ref}, registry=REGISTRY)


def errors_for(schema_ref: str, instance) -> list[ValidationError]:
    return list(validator_for(schema_ref).iter_errors(instance))


def iter_refs(node, path=""):
    if isinstance(node, dict):
        for k, v in node.items():
            if k == "$ref" and isinstance(v, str):
                yield path, v
            else:
                yield from iter_refs(v, f"{path}/{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from iter_refs(v, f"{path}/{i}")


def json_pointer(doc, pointer: str):
    if pointer in ("", "/"):
        return doc
    cur = doc
    for raw in pointer.lstrip("/").split("/"):
        part = raw.replace("~1", "/").replace("~0", "~")
        if isinstance(cur, list):
            cur = cur[int(part)]
        else:
            cur = cur[part]
    return cur


def inject_unknown(obj, map_keys, parent_key=None):
    """Adds an unknown field to every object (recursively), except map-valued objects."""
    if isinstance(obj, dict):
        out = {k: inject_unknown(v, map_keys, k) for k, v in obj.items()}
        if parent_key not in map_keys:
            out[UNKNOWN_KEY] = {"nested": [1, "two", None], "flag": True}
        return out
    if isinstance(obj, list):
        return [inject_unknown(v, map_keys, parent_key) for v in obj]
    return obj


INDEX = load_json(EXAMPLES / "index.json")


class SchemaTests(unittest.TestCase):
    def test_schemas_present(self):
        expected = {
            "common", "error", "user_input_request", "remote_task", "command_envelope", "task_event", "node",
            "artifact", "workspace", "audit_event", "pairing", "list", "requests", "node_channel",
        }
        self.assertTrue(expected <= {n.split(".")[0] for n in SCHEMA_DOCS}, sorted(SCHEMA_DOCS))

    def test_meta_schema_valid(self):
        for name, doc in SCHEMA_DOCS.items():
            with self.subTest(schema=name):
                Draft202012Validator.check_schema(doc)
                self.assertEqual(doc.get("$schema"), "https://json-schema.org/draft/2020-12/schema")
                self.assertEqual(doc.get("$id"), BASE + name)

    def test_all_schema_refs_resolve(self):
        for name, doc in SCHEMA_DOCS.items():
            resolver = REGISTRY.resolver(base_uri=doc["$id"])
            for path, ref in iter_refs(doc):
                with self.subTest(schema=name, at=path, ref=ref):
                    resolver.lookup(ref)  # raises on failure

    def test_closed_objects_are_documented(self):
        """additionalProperties:false only on grant-shaping inputs (security exception)."""
        allowed = {
            ("pairing.schema.json", "/$defs/pairing_code_create_request"),
            ("pairing.schema.json", "/$defs/node_enrollment_create_request"),
            ("workspace.schema.json", "/$defs/members_put_request"),
            ("workspace.schema.json", "/$defs/member_binding_input"),
        }
        found = set()

        def walk(node, name, path):
            if isinstance(node, dict):
                if node.get("additionalProperties") is False:
                    found.add((name, path))
                for k, v in node.items():
                    walk(v, name, f"{path}/{k}")
            elif isinstance(node, list):
                for i, v in enumerate(node):
                    walk(v, name, f"{path}/{i}")

        for name, doc in SCHEMA_DOCS.items():
            walk(doc, name, "")
        self.assertEqual(found, allowed)


class ExampleTests(unittest.TestCase):
    def test_index_covers_all_files(self):
        listed = {e["file"] for e in INDEX["valid"]} | {e["file"] for e in INDEX["invalid"]}
        on_disk = {p.relative_to(EXAMPLES).as_posix() for p in EXAMPLES.rglob("*.json") if p.name != "index.json"}
        self.assertEqual(on_disk - listed, set(), "example files missing from index.json")
        self.assertEqual(listed - on_disk, set(), "index.json lists missing files")

    def test_valid_examples(self):
        for entry in INDEX["valid"]:
            with self.subTest(example=entry["file"]):
                errs = errors_for(entry["schema"], load_json(EXAMPLES / entry["file"]))
                self.assertEqual([], [f"{list(e.absolute_path)}: {e.message}" for e in errs])

    def test_invalid_examples_fail(self):
        for entry in INDEX["invalid"]:
            with self.subTest(example=entry["file"], reason=entry.get("reason")):
                self.assertTrue(entry["file"].startswith("invalid/"))
                self.assertNotEqual([], errors_for(entry["schema"], load_json(EXAMPLES / entry["file"])),
                                    "negative example unexpectedly validates")

    def test_unknown_fields_tolerated(self):
        map_keys = set(INDEX.get("map_keys", []))
        for entry in INDEX["valid"]:
            instance = load_json(EXAMPLES / entry["file"])
            with self.subTest(example=entry["file"]):
                if entry.get("closed"):
                    top = dict(instance)
                    top[UNKNOWN_KEY] = 1
                    self.assertNotEqual([], errors_for(entry["schema"], top),
                                        "closed (grant-shaping) schema must reject unknown fields")
                else:
                    errs = errors_for(entry["schema"], inject_unknown(instance, map_keys))
                    self.assertEqual([], [f"{list(e.absolute_path)}: {e.message}" for e in errs])

    def test_examples_cover_required_operations(self):
        files = {e["file"] for e in INDEX["valid"]}
        for needed in [
            "submit_request.json", "submit_response_task.json", "task_status_running.json", "events_page.json",
            "answer_request.json", "cancel_request.json", "retry_request.json", "artifact_metadata.json",
            "download_ticket_response.json", "node_enroll_request.json", "node_heartbeat_request.json",
            "node_lease_response.json", "node_ack_request.json", "node_events_request.json",
            "node_snapshot_request.json", "node_artifact_init_request.json", "node_artifact_complete_response.json",
            "error_conflict_idempotency.json",
        ]:
            self.assertIn(needed, files)

    def test_no_real_secrets_in_examples(self):
        for p in EXAMPLES.rglob("*.json"):
            text = p.read_text(encoding="utf-8")
            for m in re.finditer(r"\b(ucp|ncp|dlt)_[A-Za-z0-9_-]{32,}", text):
                self.assertIn("EXAMPLE", m.group(0), f"{p.name}: token-like value must be an obvious placeholder")


def _sketch_endpoints():
    """(METHOD, normalized path) pairs from the §6 and §7 tables of CONTRACT_SKETCH.md."""
    out = set()
    row = re.compile(r"^\|\s*([A-Z/]+)\s*\|\s*`(/v1/[^`]*)`")
    for line in SKETCH.read_text(encoding="utf-8").splitlines():
        m = row.match(line)
        if not m:
            continue
        path = re.sub(r"\{[^}]+\}", "{}", m.group(2))
        for method in m.group(1).split("/"):
            out.add((method, path))
        if "`GET` same path" in line:
            out.add(("GET", path))
    return out


@unittest.skipIf(yaml is None, "PyYAML not installed")
class OpenApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(OPENAPI, encoding="utf-8") as f:
            cls.doc = yaml.safe_load(f)

    def test_version_and_info(self):
        self.assertTrue(str(self.doc["openapi"]).startswith("3.1"))
        self.assertIn("title", self.doc["info"])
        self.assertEqual(self.doc.get("jsonSchemaDialect"), "https://json-schema.org/draft/2020-12/schema")

    def test_security_schemes(self):
        schemes = self.doc["components"]["securitySchemes"]
        for name in ("userBearer", "nodeBearer", "metricsBearer"):
            self.assertEqual(schemes[name]["type"], "http")
            self.assertEqual(schemes[name]["scheme"], "bearer")
        self.assertIn("downloadTicket", schemes)

    def test_refs_resolve(self):
        cache = {}
        for path, ref in iter_refs(self.doc):
            with self.subTest(at=path, ref=ref):
                target, frag = urldefrag(ref)
                if target:
                    file = (OPENAPI.parent / target).resolve()
                    self.assertTrue(file.is_file(), f"missing {file}")
                    if file not in cache:
                        cache[file] = load_json(file)
                    json_pointer(cache[file], frag)
                else:
                    json_pointer(self.doc, frag)

    def test_sketch_endpoints_present(self):
        have = set()
        for p, item in self.doc["paths"].items():
            norm = re.sub(r"\{[^}]+\}", "{}", p)
            for method in item:
                if method in ("get", "put", "post", "delete", "patch", "head"):
                    have.add((method.upper(), norm))
        missing = _sketch_endpoints() - have
        self.assertGreaterEqual(len(_sketch_endpoints()), 35)
        self.assertEqual(missing, set())

    def test_operations_well_formed(self):
        op_ids = set()
        for p, item in self.doc["paths"].items():
            for method, op in item.items():
                if method not in ("get", "put", "post", "delete", "patch", "head"):
                    continue
                with self.subTest(op=f"{method.upper()} {p}"):
                    self.assertIn("operationId", op)
                    self.assertNotIn(op["operationId"], op_ids)
                    op_ids.add(op["operationId"])
                    self.assertIn("security", op, "explicit security on every operation")
                    codes = set(map(str, op["responses"]))
                    self.assertTrue(any(c.startswith("2") for c in codes))
                    self.assertTrue(any(c[0] in "45" for c in codes), "error responses documented")
                    for code, resp in op["responses"].items():
                        if "$ref" in resp:
                            continue
                        self.assertIn("X-Request-ID", resp.get("headers", {}), f"{code} lacks X-Request-ID")
                        self.assertIn("X-ASGEND-API-Version", resp.get("headers", {}))

    def test_download_and_upload_headers(self):
        dl = self.doc["paths"]["/v1/downloads/{ticket}"]["get"]
        params = {prm.get("name") for prm in dl.get("parameters", []) if isinstance(prm, dict)}
        params |= {prm["$ref"].rsplit("/", 1)[-1] for prm in dl.get("parameters", []) if "$ref" in prm}
        self.assertTrue({"Range"} <= params or "RangeHeader" in params, params)
        self.assertIn("206", dl["responses"])
        self.assertIn("Content-Range", dl["responses"]["206"]["headers"])
        up = self.doc["paths"]["/v1/node/artifacts/{art}/content"]["put"]
        names = {prm.get("name") or prm.get("$ref", "").rsplit("/", 1)[-1] for prm in up["parameters"]}
        self.assertTrue("Content-Range" in names or "ContentRangeHeader" in names, names)

    def test_rate_limited_has_retry_after(self):
        resp = self.doc["components"]["responses"]["RateLimited"]
        self.assertIn("Retry-After", resp["headers"])

    def test_optional_openapi_validator(self):
        try:
            from openapi_spec_validator import validate  # type: ignore
        except ImportError:
            self.skipTest("openapi-spec-validator not installed (optional)")
        validate(self.doc, base_uri=OPENAPI.as_uri())


class RuntimeOpenApiParityTests(unittest.TestCase):
    """Keep the executable Control Plane route table aligned with OpenAPI and clients.

    OpenAPI is the public contract; server/asgend_cp/app.py is the runtime dispatcher.
    Client checks are intentionally limited to request-bearing source locations, not
    comments or documentation prose.
    """

    @classmethod
    def setUpClass(cls):
        if yaml is None:
            raise unittest.SkipTest("PyYAML not installed")
        cls.openapi = yaml.safe_load(OPENAPI.read_text(encoding="utf-8"))
        cls.operations = {
            (method.upper(), path): op
            for path, path_item in cls.openapi["paths"].items()
            for method, op in path_item.items()
            if method.lower() in {"get", "put", "post", "delete", "patch", "head"}
        }

    @staticmethod
    def _server_routes():
        source = ROOT / "server" / "asgend_cp" / "app.py"
        tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
        register = next((n for n in ast.walk(tree)
                         if isinstance(n, ast.FunctionDef) and n.name == "_register_routes"), None)
        if register is None:
            raise AssertionError("App._register_routes() was not found; update the route inventory test")

        symbolic: dict[str, str] = {}
        for node in ast.walk(register):
            if not isinstance(node, ast.Assign) or len(node.targets) != 1:
                continue
            target, value = node.targets[0], node.value
            if not isinstance(target, ast.Tuple) or not isinstance(value, ast.Tuple):
                continue
            for name_node, value_node in zip(target.elts, value.elts):
                if (isinstance(name_node, ast.Name) and isinstance(value_node, ast.Call)
                        and isinstance(value_node.func, ast.Name)
                        and value_node.func.id == "_id_pat" and value_node.args
                        and isinstance(value_node.args[0], ast.Constant)
                        and isinstance(value_node.args[0].value, str)):
                    symbolic[name_node.id] = "{" + value_node.args[0].value + "}"

        def string_value(node):
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                return node.value
            if isinstance(node, ast.JoinedStr):
                pieces = []
                for part in node.values:
                    if isinstance(part, ast.Constant) and isinstance(part.value, str):
                        pieces.append(part.value)
                    elif isinstance(part, ast.FormattedValue) and isinstance(part.value, ast.Name):
                        if part.value.id not in symbolic:
                            raise AssertionError(f"unrecognized route-template variable: {part.value.id}")
                        pieces.append(symbolic[part.value.id])
                    else:
                        raise AssertionError(f"unrecognized route template expression: {ast.dump(part)}")
                return "".join(pieces)
            raise AssertionError(f"route method/path must be literal or f-string: {ast.dump(node)}")

        result = []
        for node in ast.walk(register):
            if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "add" and isinstance(node.func.value, ast.Name)
                    and node.func.value.id == "r"):
                continue
            if len(node.args) < 4:
                raise AssertionError("Router.add route registration missing required arguments")
            method = string_value(node.args[0]).upper()
            pattern = string_value(node.args[1])
            auth = string_value(node.args[3])
            name = string_value(node.args[4]) if len(node.args) >= 5 else pattern
            # Convert the one intentionally regex-shaped route (download ticket) into
            # its documented path template, while f-string id paths are already symbolic.
            path = re.sub(r"\(\?P<([A-Za-z_]\w*)>[^)]*\)", r"{\1}", pattern)
            result.append((method, path, auth, name))
        return result

    @staticmethod
    def _path_matches_template(observed: str, documented: str) -> bool:
        observed = re.sub(r"\$\{segment\([^}]+\)\}", "{}", observed)
        observed = re.sub(r"\$\{_seg\([^}]+\)\}", "{}", observed)
        observed = re.sub(r"\$\{urllib\.parse\.quote\([^}]+\)\}", "{}", observed)
        observed = re.sub(r"\$[A-Za-z_]\w*", "{}", observed)
        observed = re.sub(r"%\d+", "{}", observed)
        observed = observed.split("?", 1)[0]
        left, right = observed.strip("/").split("/"), documented.strip("/").split("/")
        return len(left) == len(right) and all(
            a == b or a == "{}" or (b.startswith("{") and b.endswith("}"))
            for a, b in zip(left, right)
        )

    def _client_references(self):
        refs: list[tuple[str, str | None, str]] = []
        # Native Android: request(method, path) calls in the Control Plane transport.
        android = ROOT / "clients/asgend_android/app/src/main/kotlin/org/asgend/nativeclient/data/ControlPlaneClient.kt"
        text = android.read_text(encoding="utf-8")
        refs.extend(("native Android", m.group(1), m.group(2)) for m in re.finditer(
            r'request\(\s*"(GET|POST|PUT|DELETE|PATCH)"\s*,\s*"([^"]*/v1/[^"]*)"', text
        ))

        # Flutter: direct calls to the shared HTTP _send helper.
        flutter = ROOT / "clients/asgend_flutter/lib/src/api/control_plane_client.dart"
        text = flutter.read_text(encoding="utf-8")
        refs.extend(("Flutter", m.group(1), m.group(2)) for m in re.finditer(
            r"_send\(\s*['\"](GET|POST|PUT|DELETE|PATCH)['\"]\s*,\s*['\"]([^'\"]*/v1/[^'\"]*)['\"]",
            text,
        ))

        # Node agent: parse real self.request(method, path) calls (including f-strings).
        node = ROOT / "node_agent/asgend_node/cp_client.py"
        tree = ast.parse(node.read_text(encoding="utf-8"), filename=str(node))

        def node_literal(n):
            if isinstance(n, ast.Constant) and isinstance(n.value, str):
                return n.value
            if isinstance(n, ast.JoinedStr):
                parts = []
                for part in n.values:
                    if isinstance(part, ast.Constant) and isinstance(part.value, str):
                        parts.append(part.value)
                    elif isinstance(part, ast.FormattedValue):
                        parts.append("{}")
                    else:
                        return None
                return "".join(parts)
            return None

        for n in ast.walk(tree):
            if not (isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
                    and n.func.attr == "request" and isinstance(n.func.value, ast.Name)
                    and n.func.value.id == "self" and len(n.args) >= 2):
                continue
            method, path = node_literal(n.args[0]), node_literal(n.args[1])
            if method and path and "/v1/" in path:
                refs.append(("Node agent", method.upper(), path))

        # Qt: infer method/path pairs from the actual ControlPlaneAdapter request
        # blocks. Request::verb defaults to GET; mutations override it before send().
        # Expand the shared command() path from its real call sites, and account for
        # downloadArtifact's direct QNetworkAccessManager::get() path separately.
        qt_root = ROOT / "clients/asgend_qt/src/core"
        qt_source = qt_root / "controlplaneadapter.cpp"
        qt_header = qt_root / "controlplaneadapter.h"
        qt_text = qt_source.read_text(encoding="utf-8")
        qt_header_text = qt_header.read_text(encoding="utf-8")
        default_verb = re.search(r'\bQByteArray\s+verb\s*=\s*"([A-Z]+)"', qt_header_text)
        if not default_verb:
            raise AssertionError("Qt ControlPlaneAdapter::Request default verb was not found")

        command_verbs = sorted(set(re.findall(
            r'\bcommand\s*\(\s*taskId\s*,\s*QStringLiteral\("([a-z]+)"\)', qt_text
        )))
        request_path_literals: set[str] = set()
        request_declarations = list(re.finditer(r'\bRequest\s+([A-Za-z_]\w*)\s*;', qt_text))
        if not request_declarations:
            raise AssertionError("Qt ControlPlaneAdapter has no Request declarations")
        for declaration in request_declarations:
            variable = declaration.group(1)
            tail = qt_text[declaration.end():]
            send_call = re.search(rf'\bsend\s*\(\s*{re.escape(variable)}\s*,', tail)
            if not send_call:
                raise AssertionError(f"Qt Request {variable!r} is not sent through ControlPlaneAdapter::send")
            block = tail[:send_call.start()]
            path_matches = re.findall(
                rf'\b{re.escape(variable)}\.path\s*=\s*QStringLiteral\("([^"\n]*/v1/[^"\n]*)"\)',
                block,
            )
            if len(path_matches) != 1:
                raise AssertionError(
                    f"Qt Request {variable!r} must assign exactly one literal /v1/ path; found {path_matches!r}"
                )
            path = path_matches[0]
            request_path_literals.add(path)
            explicit_verbs = re.findall(
                rf'\b{re.escape(variable)}\.verb\s*=\s*"([A-Z]+)"', block
            )
            if len(explicit_verbs) > 1:
                raise AssertionError(f"Qt Request {variable!r} assigns multiple HTTP verbs: {explicit_verbs!r}")
            method = explicit_verbs[0] if explicit_verbs else default_verb.group(1)

            if "%2" in path:
                if path.count("%2") != 1 or not command_verbs:
                    raise AssertionError(f"Qt dynamic command route cannot be resolved: {path!r}")
                refs.extend(("Qt", method, path.replace("%2", action)) for action in command_verbs)
            else:
                refs.append(("Qt", method, path))

        # The artifact GET uses a short-lived download ticket and bypasses send().
        # Derive the path and verb from the helper implementations explicitly.
        resolve_start = qt_text.find("QUrl ControlPlaneAdapter::resolveDownloadUrl")
        download_start = qt_text.find("void ControlPlaneAdapter::downloadArtifact")
        stream_start = qt_text.find("void ControlPlaneAdapter::startDownload")
        if min(resolve_start, download_start, stream_start) < 0 or not resolve_start < download_start < stream_start:
            raise AssertionError("Qt download URL/stream helpers were not found in expected order")
        resolve_block = qt_text[resolve_start:download_start]
        download_paths = re.findall(
            r'return\s+m_profile\.url\(QStringLiteral\("([^"\n]*/v1/[^"\n]*)"\)', resolve_block
        )
        if len(download_paths) != 1:
            raise AssertionError(f"Qt download fallback path must be unique; found {download_paths!r}")
        stream_block = qt_text[stream_start:]
        direct_methods = re.findall(r'\bm_nam->(get|post|put|deleteResource)\(request\)', stream_block)
        if direct_methods != ["get"]:
            raise AssertionError(f"Qt artifact download must use one direct GET request; found {direct_methods!r}")
        download_path = download_paths[0]
        refs.append(("Qt", direct_methods[0].upper(), download_path))

        # Fail closed if a new versioned endpoint string appears outside the request
        # blocks or the explicitly modelled ticket-download helper.
        all_qt_sources = sorted(qt_root.glob("*.cpp"))
        source_path_literals = {
            match.group(1)
            for source in all_qt_sources
            for match in re.finditer(r'"([^"\n]*/v1/[^"\n]*)"', source.read_text(encoding="utf-8"))
        }
        accounted_literals = request_path_literals | {download_path}
        if source_path_literals != accounted_literals:
            raise AssertionError(
                "Qt /v1/ string inventory differs from inferred request paths; "
                f"unaccounted={sorted(source_path_literals - accounted_literals)!r}, "
                f"missing={sorted(accounted_literals - source_path_literals)!r}"
            )
        return refs

    def test_runtime_route_inventory_exactly_matches_openapi(self):
        routes = self._server_routes()
        actual_rows = [(method, path) for method, path, _auth, _name in routes]
        self.assertEqual(len(actual_rows), len(set(actual_rows)), "duplicate runtime routes")
        actual, documented = set(actual_rows), set(self.operations)
        self.assertEqual(actual - documented, set(), "runtime routes missing from OpenAPI")
        self.assertEqual(documented - actual, set(), "OpenAPI operations have no runtime handler")

    def test_runtime_auth_class_matches_openapi_security(self):
        expected = {
            "none": [],
            "user": [{"userBearer": []}],
            "node": [{"nodeBearer": []}],
            "metrics": [{"metricsBearer": []}],
            # Download tickets are capability credentials in the path; they are not
            # the Authorization bearer schemes used by user/node requests.
            "ticket": [],
        }
        for method, path, auth, _name in self._server_routes():
            with self.subTest(method=method, path=path, auth=auth):
                self.assertIn(auth, expected, f"unknown runtime auth class {auth!r}")
                self.assertEqual(self.operations[(method, path)].get("security"), expected[auth])

    def test_client_control_plane_paths_exist_in_openapi(self):
        refs = self._client_references()
        by_client = {}
        issues = []
        for client, method, observed in refs:
            by_client[client] = by_client.get(client, 0) + 1
            matches = [(m, p) for m, p in self.operations
                       if self._path_matches_template(observed, p)
                       and (method is None or method == m)]
            if not matches:
                issues.append(f"{client}: {method or 'ANY'} {observed}")
        # Floors protect the source extractors themselves: a regex change must not
        # silently reduce a client to one discovered endpoint and still pass.
        minimum_refs = {"native Android": 10, "Flutter": 10, "Qt": 16, "Node agent": 6}
        for client, minimum in minimum_refs.items():
            with self.subTest(client=client):
                self.assertGreaterEqual(by_client.get(client, 0), minimum,
                                        f"too few {client} endpoint references discovered")
        self.assertEqual(issues, [], "client routes not described by OpenAPI:\n" + "\n".join(issues))

    def test_qt_method_inference_covers_all_request_paths(self):
        qt = [(method, path) for client, method, path in self._client_references() if client == "Qt"]
        expected = {
            ("POST", "/v1/pairing/redeem"),
            ("POST", "/v1/auth/logout"),
            ("GET", "/v1/me"),
            ("GET", "/v1/health"),
            ("GET", "/v1/workspaces/%1/tasks"),
            ("POST", "/v1/workspaces/%1/tasks"),
            ("GET", "/v1/tasks/%1"),
            ("GET", "/v1/tasks/%1/events"),
            ("POST", "/v1/tasks/%1/answer"),
            ("POST", "/v1/tasks/%1/cancel"),
            ("POST", "/v1/tasks/%1/retry"),
            ("GET", "/v1/workspaces/%1/nodes"),
            ("POST", "/v1/nodes/%1/revoke"),
            ("GET", "/v1/tasks/%1/artifacts"),
            ("POST", "/v1/artifacts/%1/download-ticket"),
            ("GET", "/v1/downloads/%1"),
        }
        self.assertEqual(len(qt), len(expected), "Qt scanner must emit one method/path pair per request branch")
        self.assertEqual(set(qt), expected)
        self.assertTrue(all(method in {"GET", "POST", "PUT", "PATCH", "DELETE"} for method, _ in qt))

    def test_client_path_template_matcher_rejects_wrong_static_segment(self):
        self.assertTrue(self._path_matches_template(
            "/v1/tasks/${_seg(taskId)}/$verb", "/v1/tasks/{task}/cancel"))
        self.assertFalse(self._path_matches_template(
            "/v1/tasks/${_seg(taskId)}/bogus", "/v1/tasks/{task}/cancel"))


class StateMachineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sm = load_json(STATE_MACHINE)
        common = SCHEMA_DOCS["common.schema.json"]["$defs"]
        cls.task_states = set(common["task_state"]["enum"])
        cls.command_states = set(common["command_state"]["enum"])
        cls.terminal = set(common["terminal_task_state"]["enum"])

    def test_states_match_schema(self):
        self.assertEqual(set(self.sm["task"]["states"]), self.task_states)
        self.assertEqual(set(self.sm["command"]["states"]), self.command_states)
        self.assertEqual(set(self.sm["task"]["terminal"]), self.terminal)

    def test_transitions_reference_known_states(self):
        for kind, states in (("task", self.task_states), ("command", self.command_states)):
            for tr in self.sm[kind]["transitions"]:
                with self.subTest(kind=kind, tr=tr["id"]):
                    self.assertLessEqual(set(tr["from"]), states | {"new"})
                    self.assertLessEqual(set(tr["to"]), states)
                    self.assertIn(tr["by"], {"server", "node", "user"})
                    self.assertTrue(tr.get("trigger"))

    def test_terminal_states_only_left_by_retry(self):
        for tr in self.sm["task"]["transitions"]:
            for src in tr["from"]:
                if src in self.terminal:
                    with self.subTest(tr=tr["id"]):
                        self.assertIn(src, {"failed", "delivery_failed"})

    def test_command_terminal_states_are_final(self):
        for tr in self.sm["command"]["transitions"]:
            with self.subTest(tr=tr["id"]):
                self.assertFalse(set(tr["from"]) & set(self.sm["command"]["terminal"]))

    def test_server_never_sets_execution_states(self):
        exec_states = set(SCHEMA_DOCS["common.schema.json"]["$defs"]["core_task_state"]["enum"])
        for tr in self.sm["task"]["transitions"]:
            if tr["by"] != "node":
                with self.subTest(tr=tr["id"]):
                    self.assertFalse(set(tr["to"]) & exec_states, "only nodes report core execution states")

    def test_doc_table_matches(self):
        text = API_DOC.read_text(encoding="utf-8")
        for kind in ("task", "command"):
            for tr in self.sm[kind]["transitions"]:
                cells = " | ".join(", ".join(f"`{s}`" for s in tr[k]) for k in ("from", "to"))
                needle = f"| {tr['id']} | {cells} | {tr['by']} |"
                with self.subTest(kind=kind, tr=tr["id"]):
                    self.assertIn(needle, text)


# --------------------------------------------------------------------------------------------
# Reference scenarios. A tiny in-memory model of the normative rules in docs/API_CONTRACT.md;
# implementations are expected to pass equivalent integration tests.
# --------------------------------------------------------------------------------------------

class _Outbox:
    LEASE = 60

    def __init__(self, commands):
        self.cmds = {c["command_id"]: dict(c, state="pending", lease_id=None, lease_expires_at=None, delivery_attempts=0)
                     for c in commands}
        self.order = [c["command_id"] for c in commands]
        self.n = 0

    def lease(self, now, max_n=10):
        out, blocked_tasks = [], set()
        for cid in self.order:
            c = self.cmds[cid]
            if c["state"] == "leased" and c["lease_expires_at"] <= now:
                c["state"] = "pending"  # lease expired -> redeliverable
            if c["state"] in ("acked", "rejected", "expired"):
                continue
            if c["task_id"] in blocked_tasks:
                continue  # per-task FIFO: later commands wait for earlier ones
            blocked_tasks.add(c["task_id"])
            if c["state"] == "pending" and c["expires_at"] <= now:
                c["state"] = "expired"
                blocked_tasks.discard(c["task_id"])
                if c["type"] == "submit":  # task -> delivery_failed: its queued commands expire too
                    for other in self.cmds.values():
                        if other["task_id"] == c["task_id"] and other["state"] == "pending":
                            other["state"] = "expired"
                continue
            if c["state"] == "pending" and len(out) < max_n:
                self.n += 1
                c.update(state="leased", lease_id=f"lease-{self.n:08d}", lease_expires_at=now + self.LEASE)
                c["delivery_attempts"] += 1
                out.append(copy.deepcopy(c))
        return out

    def ack(self, cid, lease_id, outcome):
        c = self.cmds[cid]
        if c["state"] in ("acked", "rejected"):
            if (c["state"] == "acked") == (outcome == "accepted"):
                return 200
            return 409
        if c["state"] == "expired":
            return 410
        if c["lease_id"] != lease_id:
            return 409
        c["state"] = "acked" if outcome == "accepted" else "rejected"
        return 200


class _Node:
    def __init__(self):
        self.seen = {}        # command_id -> core_task_id (persisted locally)
        self.core_submits = []

    def handle(self, cmd):
        if cmd["command_id"] in self.seen:
            return self.seen[cmd["command_id"]]
        core_key = "cp:" + cmd["command_id"]
        existing = [s for s in self.core_submits if s["idempotency_key"] == core_key]
        if not existing:
            self.core_submits.append({"idempotency_key": core_key, "task": cmd["payload"]["task"]})
        core_id = f"core{len(self.core_submits)}"
        self.seen[cmd["command_id"]] = core_id
        return core_id


class _Ingest:
    def __init__(self):
        self.last_seq = {}
        self.stored = []
        self.next_event_id = 1000

    def post(self, events):
        accepted = duplicates = 0
        gaps, gapped = [], set()
        for e in events:
            t = e["task_id"]
            if t in gapped:
                continue
            last = self.last_seq.get(t, 0)
            if e["seq"] <= last:
                duplicates += 1
            elif e["seq"] == last + 1:
                self.last_seq[t] = e["seq"]
                self.next_event_id += 1
                self.stored.append(dict(e, event_id=self.next_event_id))
                accepted += 1
            else:
                gaps.append({"task_id": t, "expected_seq": last + 1})
                gapped.add(t)
        return {"api_version": 1, "accepted": accepted, "duplicates": duplicates, "gaps": gaps}

    def page(self, after, limit):
        evs = [e for e in self.stored if e["event_id"] > after][:limit]
        head = self.stored[-1]["event_id"] if self.stored else 0
        last = evs[-1]["event_id"] if evs else min(after, head)
        return {"api_version": 1, "events": evs, "last_event_id": last}


class ScenarioTests(unittest.TestCase):
    def setUp(self):
        self.lease_example = load_json(EXAMPLES / "node_lease_response.json")["commands"][0]

    def test_redelivery_after_lost_ack_creates_one_core_task(self):
        cmd = dict(self.lease_example, expires_at=10_000)
        box, node = _Outbox([cmd]), _Node()
        first = box.lease(now=0)
        self.assertEqual(len(first), 1)
        node.handle(first[0])                      # node submits to core, ack is lost
        self.assertEqual(box.lease(now=30), [])    # still leased, not redelivered early
        second = box.lease(now=61)                 # lease expired -> redelivered, same command_id
        self.assertEqual(second[0]["command_id"], first[0]["command_id"])
        self.assertNotEqual(second[0]["lease_id"], first[0]["lease_id"])
        self.assertEqual(second[0]["delivery_attempts"], 2)
        node.handle(second[0])
        self.assertEqual(len(node.core_submits), 1, "redelivered submit must not create a second core task")
        self.assertEqual(node.core_submits[0]["idempotency_key"], "cp:" + cmd["command_id"])
        self.assertEqual(box.ack(cmd["command_id"], first[0]["lease_id"], "accepted"), 409, "superseded lease")
        self.assertEqual(box.ack(cmd["command_id"], second[0]["lease_id"], "accepted"), 200)
        self.assertEqual(box.ack(cmd["command_id"], second[0]["lease_id"], "accepted"), 200, "idempotent re-ack")
        self.assertEqual(box.ack(cmd["command_id"], second[0]["lease_id"], "rejected"), 409)
        self.assertEqual(box.lease(now=200), [])

    def test_node_restart_dedupes_by_core_idempotency_key(self):
        cmd = dict(self.lease_example, expires_at=10_000)
        box = _Outbox([cmd])
        node = _Node()
        node.handle(box.lease(now=0)[0])
        node.seen.clear()                           # node lost its local dedupe table
        node.handle(box.lease(now=61)[0])
        self.assertEqual(len(node.core_submits), 1, "core idempotency_key cp:<command_id> is the second line of defence")

    def test_per_task_fifo_and_ttl_expiry(self):
        submit = dict(self.lease_example, state="pending", expires_at=100)
        cancel = dict(load_json(EXAMPLES / "cancel_response_command.json"), expires_at=10_000)
        box = _Outbox([submit, cancel])
        self.assertEqual([c["type"] for c in box.lease(now=0)], ["submit"], "cancel must not overtake submit")
        self.assertEqual(box.lease(now=61)[0]["type"], "submit")
        # lease of attempt 2 ends at 121 without ack; TTL (100) has passed -> submit expires and the
        # task becomes delivery_failed, so the queued cancel for that task expires too.
        self.assertEqual(box.lease(now=200), [])
        self.assertEqual(box.cmds[submit["command_id"]]["state"], "expired")
        self.assertEqual(box.cmds[cancel["command_id"]]["state"], "expired")
        self.assertEqual(box.ack(submit["command_id"], "lease-00000002", "accepted"), 410)

    def test_event_ingest_duplicates_and_gaps(self):
        batch = load_json(EXAMPLES / "node_events_request.json")["events"]
        for e in batch:
            self.assertEqual(errors_for("task_event.schema.json#/$defs/node_event", e), [])
        ing = _Ingest()
        r1 = ing.post(batch[:2])
        self.assertEqual((r1["accepted"], r1["duplicates"], r1["gaps"]), (2, 0, []))
        r2 = ing.post(batch)                         # resend after timeout: 2 dupes, 2 new
        self.assertEqual((r2["accepted"], r2["duplicates"]), (2, 2))
        far = dict(batch[0], seq=9)
        r3 = ing.post([far])
        self.assertEqual(r3["gaps"], [{"task_id": far["task_id"], "expected_seq": 5}])
        self.assertEqual(r3["accepted"], 0)
        self.assertEqual(errors_for("node_channel.schema.json#/$defs/events_response", r3), [])

    def test_client_reconnect_resumes_without_loss_or_duplicates(self):
        ing = _Ingest()
        base = load_json(EXAMPLES / "node_events_request.json")["events"][1]
        ing.post([dict(base, seq=i) for i in range(1, 8)])
        seen, cursor = [], 0
        page = ing.page(cursor, limit=3)
        seen += [e["event_id"] for e in page["events"]]
        cursor = page["last_event_id"]
        # connection drops; more events arrive meanwhile
        ing.post([dict(base, seq=i) for i in range(8, 11)])
        while True:
            page = ing.page(cursor, limit=3)
            if not page["events"]:
                self.assertEqual(page["last_event_id"], cursor)
                break
            seen += [e["event_id"] for e in page["events"]]
            cursor = page["last_event_id"]
        self.assertEqual(seen, sorted(set(seen)))
        self.assertEqual(len(seen), 10)

    def test_cursor_reset_is_detectable(self):
        ing = _Ingest()
        base = load_json(EXAMPLES / "node_events_request.json")["events"][1]
        ing.post([dict(base, seq=1), dict(base, seq=2)])
        page = ing.page(after=5000, limit=10)       # client cursor from before a server restore
        self.assertLess(page["last_event_id"], 5000, "empty page returns min(after, head) -> client resyncs")

    def test_example_event_page_is_ordered(self):
        page = load_json(EXAMPLES / "events_page.json")
        ids = [e["event_id"] for e in page["events"]]
        self.assertEqual(ids, sorted(ids))
        self.assertEqual(page["last_event_id"], ids[-1])
        node_seqs = [e["seq"] for e in page["events"] if e["seq"] is not None]
        self.assertEqual(node_seqs, list(range(1, len(node_seqs) + 1)))


def main() -> int:
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
