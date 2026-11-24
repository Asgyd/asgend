"""Command line: ``python -m asgend_cp serve|migrate|admin|backup|restore|...``."""

from __future__ import annotations

import argparse
import json
import logging
import signal
import sys
import threading
import urllib.request

from . import __version__, admin
from .config import Config, ConfigError
from .db import SchemaError, StorageError, open_storage
from .logs import get_logger, log, setup_logging
from .migrations import LATEST_VERSION


def build_server(cfg: Config, storage=None, clock=None):
    """Create App + HTTP server (not started). Raises ConfigError/SchemaError."""
    from .app import App
    from .http import ControlPlaneHTTPServer, make_ssl_context
    cfg.check_transport()
    st = storage or open_storage(cfg.db_url, cfg.db_password, cfg.db_pool_size)
    if cfg.auto_migrate:
        st.migrate()
    st.check_schema()
    app = App(cfg, st, clock)
    ctx = make_ssl_context(cfg.tls_cert, cfg.tls_key) if cfg.tls_enabled else None
    httpd = ControlPlaneHTTPServer(app, cfg.bind, cfg.port, ctx)
    return app, httpd


def cmd_serve(cfg: Config, args) -> int:
    logger = get_logger()
    try:
        app, httpd = build_server(cfg)
    except (ConfigError, SchemaError, StorageError) as e:
        log(logger, logging.ERROR, "startup refused", reason=str(e))
        return 2
    if not cfg.allowed_hosts and not cfg.bind_is_loopback():
        log(logger, logging.WARNING, "allowed_hosts is empty; set ASGEND_CP_ALLOWED_HOSTS in production")
    stop = threading.Event()

    def _sig(signum, frame):
        stop.set()
    for s in (signal.SIGTERM, signal.SIGINT):
        try:
            signal.signal(s, _sig)
        except (ValueError, OSError):
            pass
    app.start_background()
    t = threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.5},
                         name="asgend-cp-http", daemon=True)
    t.start()
    host, port = httpd.server_address[:2]
    log(logger, logging.INFO, "control plane listening", host=host, port=port,
        tls=cfg.tls_enabled, version=__version__, schema_version=LATEST_VERSION,
        dialect=app.storage.dialect)
    while not stop.wait(1.0):
        pass
    log(logger, logging.INFO, "shutdown requested; draining", inflight=httpd.inflight)
    httpd.graceful_stop(cfg.shutdown_grace_seconds)
    app.close()
    log(logger, logging.INFO, "shutdown complete")
    return 0


def cmd_migrate(cfg: Config, args) -> int:
    st = open_storage(cfg.db_url, cfg.db_password, 2)
    try:
        if args.status:
            cur = st.schema_version()
            print(json.dumps({"current": cur, "latest": LATEST_VERSION,
                              "applied": st.applied_migrations()}, default=str))
            return 0 if cur <= LATEST_VERSION else 3
        applied = st.migrate(args.target)
        print(json.dumps({"applied": applied, "current": st.schema_version()}))
        return 0
    except SchemaError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 3
    finally:
        st.close()


def cmd_admin(cfg: Config, args) -> int:
    st = open_storage(cfg.db_url, cfg.db_password, 2)
    try:
        st.check_schema()
        a = args.admin_cmd
        if a == "create-workspace":
            ws = admin.create_workspace(st, args.name, args.owner)
            print(json.dumps({"workspace_id": ws}))
        elif a == "create-user":
            r = admin.create_user(st, args.name, args.workspace, args.role)
            print(json.dumps({"user_id": r["user_id"], "token_id": r["token_id"], "token": r["token"],
                              "note": "token is shown once; store it securely"}))
        elif a == "issue-token":
            r = admin.new_user_token(st, args.user)
            print(json.dumps({"user_id": r["user_id"], "token_id": r["token_id"], "token": r["token"],
                              "note": "token is shown once; store it securely"}))
        elif a == "add-member":
            admin.add_member(st, args.workspace, args.user, args.role)
            print(json.dumps({"ok": True}))
        elif a == "list":
            for row in admin.listing(st, args.what):
                print(json.dumps(row, default=str))
        elif a == "revoke-token":
            token = sys.stdin.readline().strip() if args.token_stdin else None
            n = admin.revoke_token(st, token=token, token_id=args.token_id, user_id=args.user,
                                   node_id=args.node)
            print(json.dumps({"revoked": n}))
        return 0
    except (admin.AdminError, SchemaError) as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 1
    finally:
        st.close()


def cmd_cleanup(cfg: Config, args) -> int:
    from .app import App
    st = open_storage(cfg.db_url, cfg.db_password, 2)
    st.check_schema()
    app = App(cfg, st)
    try:
        print(json.dumps(app.run_maintenance()))
    finally:
        app.close()
    return 0


def cmd_backup(cfg: Config, args) -> int:
    from . import backup
    st = open_storage(cfg.db_url, cfg.db_password, 2)
    try:
        m = backup.backup(st, cfg.data_dir, args.out)
        print(json.dumps({"ok": True, "schema_version": m["schema_version"], "artifacts": len(m["artifacts"])}))
        return 0
    except backup.BackupError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 1
    finally:
        st.close()


def cmd_restore(cfg: Config, args) -> int:
    from . import backup
    try:
        if args.verify_only:
            manifest = backup.verify_backup(args.source)
            print(json.dumps({"ok": True, "verified": True,
                              "dialect": manifest["dialect"],
                              "schema_version": manifest["schema_version"],
                              "artifacts": len(manifest["artifacts"])}))
            return 0
        m = backup.restore(cfg.db_url, cfg.data_dir, args.source, force=args.force)
        print(json.dumps({"ok": True, "schema_version": m.get("schema_version")}))
        return 0
    except backup.BackupError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 1


def cmd_verify_artifacts(cfg: Config, args) -> int:
    from . import backup
    st = open_storage(cfg.db_url, cfg.db_password, 2)
    try:
        r = backup.verify_artifacts(st, cfg.data_dir)
        print(json.dumps(r))
        return 0 if not r["problems"] else 1
    finally:
        st.close()


def cmd_healthcheck(cfg: Config, args) -> int:
    scheme = "https" if cfg.tls_enabled else "http"
    host = "127.0.0.1" if cfg.bind in ("0.0.0.0", "127.0.0.1", "") else ("[::1]" if ":" in cfg.bind else cfg.bind)
    url = f"{scheme}://{host}:{cfg.port}/v1/{'ready' if args.ready else 'health'}"
    try:
        import ssl
        ctx = None
        if scheme == "https":
            # local probe: trust the server's own certificate, hostname is the loopback address
            ctx = ssl.create_default_context(cafile=cfg.tls_cert)
            ctx.check_hostname = False
        with urllib.request.urlopen(url, timeout=4, context=ctx) as r:
            return 0 if r.status == 200 else 1
    except Exception:
        return 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="asgend_cp", description="ASGEND Remote Control Plane")
    ap.add_argument("--config", help="JSON config file (env ASGEND_CP_CONFIG)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("serve", help="run the HTTP API server")
    m = sub.add_parser("migrate", help="apply schema migrations")
    m.add_argument("--status", action="store_true")
    m.add_argument("--target", type=int)
    adm = sub.add_parser("admin", help="administrative commands")
    asub = adm.add_subparsers(dest="admin_cmd", required=True)
    p = asub.add_parser("create-workspace")
    p.add_argument("--name", required=True)
    p.add_argument("--owner")
    p = asub.add_parser("create-user")
    p.add_argument("--name", required=True)
    p.add_argument("--workspace")
    p.add_argument("--role", default="owner", choices=admin.ROLES)
    p = asub.add_parser("issue-token")
    p.add_argument("--user", required=True)
    p = asub.add_parser("add-member")
    p.add_argument("--workspace", required=True)
    p.add_argument("--user", required=True)
    p.add_argument("--role", required=True, choices=admin.ROLES)
    p = asub.add_parser("list")
    p.add_argument("what", choices=("users", "workspaces", "members", "nodes", "tokens"))
    p = asub.add_parser("revoke-token")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--token-stdin", action="store_true", help="read the raw token from stdin")
    g.add_argument("--token-id")
    g.add_argument("--user")
    g.add_argument("--node")
    sub.add_parser("cleanup", help="run TTL sweep + retention once")
    b = sub.add_parser("backup", help="write a verified backup directory")
    b.add_argument("--out", required=True)
    r = sub.add_parser("restore", help="restore from a backup directory")
    r.add_argument("--from", dest="source", required=True)
    r.add_argument("--force", action="store_true")
    r.add_argument("--verify-only", action="store_true")
    sub.add_parser("verify-artifacts", help="re-hash all available artifact blobs")
    h = sub.add_parser("healthcheck", help="probe the local server (Docker HEALTHCHECK)")
    h.add_argument("--ready", action="store_true")
    args = ap.parse_args(argv)
    try:
        cfg = Config.load(file_path=args.config)
    except ConfigError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 2
    setup_logging(cfg.log_level)
    handlers = {
        "serve": cmd_serve, "migrate": cmd_migrate, "admin": cmd_admin, "cleanup": cmd_cleanup,
        "backup": cmd_backup, "restore": cmd_restore, "verify-artifacts": cmd_verify_artifacts,
        "healthcheck": cmd_healthcheck,
    }
    try:
        return handlers[args.cmd](cfg, args)
    except (StorageError, SchemaError) as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 3
