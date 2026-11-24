"""Command line entry point: ``python -m asgend_node enroll|run|status|rotate-token``."""

from __future__ import annotations

import argparse
import json
import os
import signal
import sys
import time

from . import PROTOCOL_VERSION, __version__
from .agent import EXIT_REVOKED, NodeAgent, NodeRevoked
from .config import NodeConfig, load_config
from .core_client import CoreClient, CoreEndpoint, CoreError, CoreUnavailable
from .cp_client import CPClient, CPError, CPUnavailable, validate_server_url
from .logutil import get_logger, register_secret, setup_logging
from .state import DB_FILE, Credentials, Store, utc_now

log = get_logger()

EXIT_ERROR = 1
EXIT_USAGE = 2


def _core_client(cfg: NodeConfig) -> CoreClient:
    token = cfg.resolved_core_token()
    register_secret(token)
    return CoreClient(CoreEndpoint(host=cfg.core_host, port=int(cfg.core_port), unix_socket=cfg.core_unix_socket,
                                   auth_token=token, timeout=float(cfg.core_timeout),
                                   max_response_bytes=int(cfg.core_max_response_bytes)))


def _add_common(p: argparse.ArgumentParser) -> None:
    p.add_argument("--config", help="TOML config file (see packaging/node.example.toml)")
    p.add_argument("--state-dir", help="directory for credentials.json and node.sqlite3")
    p.add_argument("--log-level", help="DEBUG, INFO, WARNING, ERROR")
    p.add_argument("--core-host")
    p.add_argument("--core-port", type=int)
    p.add_argument("--core-unix-socket")
    p.add_argument("--core-token-file", help="file with the core server.auth_token")


def _load(args) -> NodeConfig:
    overrides = {
        "state_dir": args.state_dir, "log_level": args.log_level, "core_host": args.core_host,
        "core_port": args.core_port, "core_unix_socket": args.core_unix_socket,
        "core_auth_token_file": args.core_token_file,
    }
    for name in ("ca_file",):
        if getattr(args, name, None):
            overrides[name] = getattr(args, name)
    if getattr(args, "allow_insecure_http", False):
        overrides["allow_insecure_http"] = True
    return load_config(args.config, overrides)


def _read_code(args) -> str | None:
    if args.code == "-":
        return sys.stdin.readline().strip() or None
    if args.code:
        return args.code.strip()
    if args.code_file:
        with open(args.code_file, "r", encoding="utf-8") as fh:
            return fh.read().strip() or None
    return os.environ.get("ASGEND_NODE_ENROLL_CODE") or None


def cmd_enroll(args) -> int:
    cfg = _load(args)
    setup_logging(cfg.log_level)
    code = _read_code(args)
    if not code:
        print("error: enrollment code required (--code, --code -, --code-file or ASGEND_NODE_ENROLL_CODE)",
              file=sys.stderr)
        return EXIT_USAGE
    register_secret(code)
    try:
        validate_server_url(args.server, cfg.allow_insecure_http)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_USAGE
    creds = Credentials(cfg.state_dir)
    if creds.exists() and not args.force:
        try:
            existing = creds.load()
        except (OSError, ValueError):
            existing = {}
        if not existing.get("revoked"):
            print(f"error: node already enrolled as {existing.get('node_id')}; use --force to re-enroll",
                  file=sys.stderr)
            return EXIT_USAGE
    labels = [x.strip() for x in (args.labels or "").split(",") if x.strip()]
    core_version = None
    try:
        core_version = str(_core_client(cfg).ping().get("version") or "") or None
    except (CoreUnavailable, CoreError, OSError) as exc:
        log.warning("core not reachable during enrollment (%s); continuing", exc)
    cp = CPClient(args.server, ca_file=cfg.ca_file, allow_insecure_http=cfg.allow_insecure_http,
                  timeout=cfg.http_timeout)
    try:
        reply = cp.enroll(code, args.name, labels, PROTOCOL_VERSION, core_version)
    except CPError as exc:
        print(f"error: enrollment refused: {exc.code} {exc.message}", file=sys.stderr)
        return EXIT_ERROR
    except CPUnavailable as exc:
        print(f"error: control plane unreachable: {exc}", file=sys.stderr)
        return EXIT_ERROR
    token = reply.get("node_token")
    if not isinstance(token, str) or not token.startswith("ncp_") or not reply.get("node_id"):
        print("error: control plane returned an invalid enrollment reply", file=sys.stderr)
        return EXIT_ERROR
    register_secret(token)
    db_path = os.path.join(cfg.state_dir, DB_FILE)
    if os.path.exists(db_path):  # state of a previous identity must not leak into the new one
        aside = f"{db_path}.{time.strftime('%Y%m%d%H%M%S')}.bak"
        for suffix in ("", "-wal", "-shm"):
            if os.path.exists(db_path + suffix):
                os.replace(db_path + suffix, aside + suffix)
        log.info("previous node database moved to %s", aside)
    creds.save({
        "server_url": args.server, "node_id": reply["node_id"], "workspace_id": reply.get("workspace_id"),
        "node_token": token, "name": args.name, "labels": labels,
        "heartbeat_interval_seconds": reply.get("heartbeat_interval_seconds", 15),
        "lease_seconds": reply.get("lease_seconds", 45), "ca_file": cfg.ca_file,
        "allow_insecure_http": bool(cfg.allow_insecure_http), "enrolled_at": utc_now(), "revoked": False,
    })
    store = Store(cfg.state_dir)
    store.set_meta("node_id", str(reply["node_id"]))
    store.close()
    print(json.dumps({"node_id": reply["node_id"], "workspace_id": reply.get("workspace_id"),
                      "credentials": creds.path}))
    log.info("enrolled as node %s in workspace %s", reply["node_id"], reply.get("workspace_id"))
    return 0


def build_agent(cfg: NodeConfig) -> tuple[NodeAgent | None, int]:
    creds = Credentials(cfg.state_dir)
    if not creds.exists():
        print(f"error: node is not enrolled (no {creds.path}); run 'enroll' first", file=sys.stderr)
        return None, EXIT_USAGE
    data = creds.load()
    if data.get("revoked"):
        print("error: this node was revoked by the control plane; re-enroll with 'enroll --force'",
              file=sys.stderr)
        return None, EXIT_REVOKED
    token = data.get("node_token")
    register_secret(token)
    cp = CPClient(data["server_url"], token=token, ca_file=cfg.ca_file or data.get("ca_file"),
                  allow_insecure_http=bool(cfg.allow_insecure_http or data.get("allow_insecure_http")),
                  timeout=cfg.http_timeout)
    store = Store(cfg.state_dir)
    stored_node = store.get_meta("node_id")
    if stored_node and stored_node != data.get("node_id"):
        print("error: node.sqlite3 belongs to a different node identity", file=sys.stderr)
        return None, EXIT_USAGE
    if not stored_node:
        store.set_meta("node_id", str(data.get("node_id")))
    return NodeAgent(cfg, data, store, _core_client(cfg), cp, credentials_store=creds), 0


def cmd_run(args) -> int:
    cfg = _load(args)
    setup_logging(cfg.log_level)
    agent, rc = build_agent(cfg)
    if agent is None:
        return rc

    def _graceful(signum, _frame):
        log.info("received signal %s; stopping", signum)
        agent.stop()

    signal.signal(signal.SIGTERM, _graceful)
    signal.signal(signal.SIGINT, _graceful)
    if args.once:
        try:
            agent.heartbeat_once()
            agent.poll_commands_once(wait_ms=0)
            agent.sync_events_once()
            agent.upload_artifacts_once()
            agent.sync_events_once()
        except NodeRevoked:
            return EXIT_REVOKED
        return 0
    return agent.run()


def cmd_status(args) -> int:
    cfg = _load(args)
    setup_logging(cfg.log_level or "WARNING")
    creds = Credentials(cfg.state_dir)
    out: dict = {"version": __version__, "state_dir": cfg.state_dir, "enrolled": creds.exists()}
    if creds.exists():
        data = creds.load()
        for key in ("node_id", "workspace_id", "server_url", "name", "labels", "enrolled_at", "revoked",
                    "revoked_at"):
            out[key] = data.get(key)
        store = Store(cfg.state_dir)
        out["counts"] = store.counts()
        store.close()
    if args.check_core:
        try:
            ping = _core_client(cfg).ping()
            out["core"] = {"reachable": True, "version": ping.get("version")}
        except (CoreUnavailable, CoreError, OSError) as exc:
            out["core"] = {"reachable": False, "error": str(exc)}
    print(json.dumps(out, indent=2, sort_keys=True))
    if out.get("revoked"):
        return EXIT_REVOKED
    return 0 if out["enrolled"] else EXIT_USAGE


def cmd_rotate(args) -> int:
    cfg = _load(args)
    setup_logging(cfg.log_level)
    agent, rc = build_agent(cfg)
    if agent is None:
        return rc
    try:
        agent.rotate_token()
    except NodeRevoked:
        return EXIT_REVOKED
    except (CPError, CPUnavailable) as exc:
        print(f"error: token rotation failed: {exc}", file=sys.stderr)
        return EXIT_ERROR
    print("token rotated")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="asgend-node", description="ASGEND Execution Node agent")
    parser.add_argument("--version", action="version", version=f"asgend-node {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("enroll", help="register this node with a Control Plane")
    _add_common(p)
    p.add_argument("--server", required=True, help="Control Plane base URL (https://...)")
    p.add_argument("--code", help="enrollment code ('-' reads it from stdin)")
    p.add_argument("--code-file", help="file containing the enrollment code")
    p.add_argument("--name", required=True, help="node display name")
    p.add_argument("--labels", help="comma separated capability labels")
    p.add_argument("--ca-file", help="custom CA bundle for TLS verification")
    p.add_argument("--allow-insecure-http", action="store_true",
                   help="allow plain http:// to a non-loopback host (development only)")
    p.add_argument("--force", action="store_true", help="replace an existing enrollment")
    p.set_defaults(func=cmd_enroll)

    p = sub.add_parser("run", help="run the node agent")
    _add_common(p)
    p.add_argument("--ca-file")
    p.add_argument("--once", action="store_true", help="single pass of every loop, then exit")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("status", help="show local node state (never prints tokens)")
    _add_common(p)
    p.add_argument("--check-core", action="store_true", help="also ping the local core")
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("rotate-token", help="rotate the node token")
    _add_common(p)
    p.add_argument("--ca-file")
    p.set_defaults(func=cmd_rotate)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except (ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_USAGE
