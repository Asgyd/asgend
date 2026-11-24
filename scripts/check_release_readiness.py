#!/usr/bin/env python3
"""Audit ASGEND release-version metadata and report outstanding 1.0.0 gates.

This is a release guard, not a substitute for executing the test suites or
observing hosted CI results. Use --strict before creating a final release tag.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = ROOT / "release" / "versions.json"
GATES_FILE = ROOT / "release" / "1.0.0-gates.json"
REPORT_FILE = ROOT / "docs" / "RELEASE_STATUS.md"


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"cannot read {path.relative_to(ROOT)}: {exc}") from exc


def extract(pattern: str, content: str, label: str) -> str:
    match = re.search(pattern, content, re.MULTILINE)
    if not match:
        raise RuntimeError(f"version source not found: {label}")
    return match.group(1).strip()


def actual_versions(root: Path = ROOT) -> dict[str, str]:
    core_cmake = read_text(root / "CMakeLists.txt")
    cp_init = read_text(root / "server" / "asgend_cp" / "__init__.py")
    compose = read_text(root / "server" / "deploy" / "compose.yaml")
    openapi = read_text(root / "contracts" / "v1" / "openapi.yaml")
    qt_cmake = read_text(root / "clients" / "asgend_qt" / "CMakeLists.txt")
    appstream = read_text(root / "packaging" / "linux" / "org.asgend.QtClient.metainfo.xml")
    flutter = read_text(root / "clients" / "asgend_flutter" / "pubspec.yaml")

    cp_tags = sorted(set(re.findall(r"image:\s*asgend-control-plane:([^\s#]+)", compose)))
    if len(cp_tags) != 1:
        raise RuntimeError(f"expected one Control Plane image tag in Compose, found {cp_tags!r}")
    qt_releases = re.findall(r'<release\s+version="([^"]+)"', appstream)
    if not qt_releases:
        raise RuntimeError("Qt AppStream release version not found")

    return {
        "core": extract(r"project\(agent_orchestrator_c\s+VERSION\s+([^\s)]+)", core_cmake, "CMake core project version"),
        "control_plane": extract(r'^__version__\s*=\s*["\']([^"\']+)', cp_init, "Control Plane __version__"),
        "control_plane_compose": cp_tags[0],
        "api": extract(r"^\s*version:\s*([^\s#]+)", openapi, "OpenAPI info.version"),
        "qt_client": extract(r"project\(asgend_qt\s+VERSION\s+([^\s)]+)", qt_cmake, "Qt CMake project version"),
        "qt_appstream": qt_releases[0],
        "flutter_client": extract(r"^version:\s*([^\s#]+)", flutter, "Flutter pubspec version"),
    }


def audit_versions(root: Path = ROOT) -> list[str]:
    manifest = json.loads(read_text(root / "release" / "versions.json"))
    expected: dict[str, str] = manifest["components"]
    actual = actual_versions(root)
    observed_map = {
        "core": "core",
        "control_plane": "control_plane",
        "control_plane_compose": "control_plane",
        "api": "api",
        "qt_client": "qt_client",
        "qt_appstream": "qt_client",
        "flutter_client": "flutter_client",
    }
    errors = []
    for source, component in observed_map.items():
        got = actual[source]
        want = expected[component]
        if got != want:
            errors.append(f"{source}: source says {got!r}, manifest says {want!r} ({component})")
    candidate = manifest.get("candidate_version")
    if expected.get("control_plane") != candidate:
        errors.append("Control Plane component version must equal candidate_version")
    if expected.get("api") != candidate:
        errors.append("API component version must equal candidate_version")
    gate_manifest = json.loads(read_text(root / "release" / "1.0.0-gates.json"))
    if gate_manifest.get("candidate_version") != candidate:
        errors.append("release gate candidate_version differs from release/versions.json")
    if gate_manifest.get("release_version") != manifest.get("product_version"):
        errors.append("release gate release_version differs from release/versions.json product_version")
    if manifest.get("release_channel") != "release-candidate" and manifest.get("release_approved") is False:
        errors.append("release_approved=false but release_channel is not 'release-candidate'")
    if manifest.get("release_approved") and manifest.get("release_channel") != "stable":
        errors.append("release_approved=true requires release_channel='stable'")
    return errors


def validate_gates(data: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(data.get("gates"), list):
        raise RuntimeError("release gate manifest must contain a gates array")
    seen: set[str] = set()
    for gate in data["gates"]:
        if not isinstance(gate, dict):
            raise RuntimeError("each release gate must be an object")
        gate_id = gate.get("id")
        if not isinstance(gate_id, str) or not gate_id.strip() or gate_id in seen:
            raise RuntimeError(f"invalid or duplicate release gate id: {gate_id!r}")
        seen.add(gate_id)
        if gate.get("status") not in {"passed", "blocked", "not_run", "pending"}:
            raise RuntimeError(f"gate {gate_id} has unsupported status {gate.get('status')!r}")
        if not isinstance(gate.get("required"), bool):
            raise RuntimeError(f"gate {gate_id} must specify required=true/false")
        if not isinstance(gate.get("evidence"), str) or not gate["evidence"].strip():
            raise RuntimeError(f"gate {gate_id} must include evidence or a blocker reason")
    return data


def load_gates(root: Path = ROOT) -> dict[str, Any]:
    data = json.loads(read_text(root / "release" / "1.0.0-gates.json"))
    return validate_gates(data)


def release_blockers(gates_data: dict[str, Any], version_errors: list[str],
                      version_manifest: dict[str, Any] | None = None) -> list[str]:
    blockers = [f"version metadata: {error}" for error in version_errors]
    if version_manifest is not None and not version_manifest.get("release_approved", False):
        blockers.append("version manifest release_approved=false")
    if not gates_data.get("release_ready", False):
        blockers.append("release manifest has release_ready=false")
    for gate in gates_data["gates"]:
        if gate["required"] and gate["status"] != "passed":
            blockers.append(f"{gate['id']}: {gate['title']} [{gate['status']}] — {gate['evidence']}")
    return blockers


def render_report(version_manifest: dict[str, Any], gates_data: dict[str, Any],
                  version_errors: list[str]) -> str:
    blockers = release_blockers(gates_data, version_errors, version_manifest)
    lines = [
        "# ASGEND 1.0.0 release readiness",
        "",
        f"- Product release: `{version_manifest['product_version']}`",
        f"- Current candidate: `{version_manifest['candidate_version']}`",
        f"- Channel: `{version_manifest['release_channel']}`",
        f"- Release approved: `{str(version_manifest['release_approved']).lower()}`",
        f"- Gate manifest ready: `{str(gates_data.get('release_ready', False)).lower()}`",
        f"- Decision: **{'READY' if not blockers else 'BLOCKED'}**",
        "",
        "## Component versions",
        "",
        "| Component | Version | Source |",
        "|---|---|---|",
    ]
    for component, version in version_manifest["components"].items():
        source = version_manifest.get("version_sources", {}).get(component, "")
        lines.append(f"| `{component}` | `{version}` | {source} |")
    lines.extend(["", "## Acceptance gates", "", "| Gate | Required | Status | Evidence / blocker |", "|---|---:|---|---|"])
    for gate in gates_data["gates"]:
        evidence = gate["evidence"].replace("|", "\\|")
        lines.append(f"| {gate['title']} (`{gate['id']}`) | {'yes' if gate['required'] else 'no'} | **{gate['status']}** | {evidence} |")
    lines.extend(["", "## Release blockers", ""])
    if blockers:
        lines.extend(f"- {item}" for item in blockers)
    elif version_manifest.get("release_approved"):
        lines.append("No required blockers recorded. The maintainer approved this stable release.")
    else:
        lines.append("No required blockers recorded. A maintainer must still review the evidence and explicitly approve the stable release.")
    lines.extend([
        "",
        "This report is derived from `release/versions.json` and `release/1.0.0-gates.json`.",
        "Do not change a gate to `passed` without a reproducible run or an observed hosted CI result. Source files existing is not acceptance evidence.",
        "",
    ])
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strict", action="store_true", help="exit non-zero while any required release gate is open")
    parser.add_argument("--write-report", action="store_true", help=f"write {REPORT_FILE.relative_to(ROOT)}")
    parser.add_argument("--json", action="store_true", help="emit machine-readable audit summary")
    args = parser.parse_args(argv)

    try:
        versions_manifest = json.loads(read_text(VERSION_FILE))
        gates_data = load_gates()
        version_errors = audit_versions()
        blockers = release_blockers(gates_data, version_errors, versions_manifest)
        report = render_report(versions_manifest, gates_data, version_errors)
    except (RuntimeError, KeyError, ValueError, json.JSONDecodeError) as exc:
        print(f"release readiness audit error: {exc}", file=sys.stderr)
        return 3

    if args.write_report:
        REPORT_FILE.write_text(report, encoding="utf-8")
    if args.json:
        print(json.dumps({
            "product_version": versions_manifest["product_version"],
            "candidate_version": versions_manifest["candidate_version"],
            "release_ready": not blockers,
            "version_errors": version_errors,
            "blocker_count": len(blockers),
            "blockers": blockers,
        }, ensure_ascii=False, indent=2))
    else:
        print(f"ASGEND {versions_manifest['product_version']} release readiness: {'READY' if not blockers else 'BLOCKED'}")
        print(f"Candidate: {versions_manifest['candidate_version']}; required blockers: {len(blockers)}")
        for item in blockers:
            print(f"BLOCKER: {item}")
        if args.write_report:
            print(f"Wrote {REPORT_FILE.relative_to(ROOT)}")

    if version_errors:
        return 1
    if args.strict and blockers:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
