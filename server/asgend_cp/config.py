"""Configuration: defaults < optional JSON file < environment variables.

Every key ``foo_bar`` maps to env ``ASGEND_CP_FOO_BAR``. Secret keys can also be
provided as ``ASGEND_CP_<KEY>_FILE`` (Docker/Kubernetes secret mounts); the
file content (stripped) is used. Secrets are never logged.
"""

from __future__ import annotations

import ipaddress
import json
import os
from dataclasses import dataclass, field, fields

ENV_PREFIX = "ASGEND_CP_"
SECRET_KEYS = ("db_url", "db_password", "metrics_token")


class ConfigError(Exception):
    pass


@dataclass
class Config:
    bind: str = "127.0.0.1"
    port: int = 8787
    db_url: str = "sqlite:///./data/control-plane.sqlite3"
    db_password: str = ""
    data_dir: str = "./data/artifacts"
    allow_insecure_http: bool = False
    tls_cert: str = ""
    tls_key: str = ""
    hsts: bool = False
    metrics_token: str = ""
    allowed_hosts: list = field(default_factory=list)
    cors_origins: list = field(default_factory=list)
    trusted_proxies: list = field(default_factory=list)
    auto_migrate: bool = False
    log_level: str = "INFO"
    db_pool_size: int = 16
    # protocol timings (contract defaults)
    heartbeat_interval_seconds: int = 15
    node_lease_seconds: int = 45
    command_lease_seconds: int = 60
    token_rotation_grace_seconds: int = 60
    pairing_code_ttl_seconds: int = 600
    enrollment_code_ttl_seconds: int = 900
    download_ticket_ttl_seconds: int = 300
    default_command_ttl_seconds: int = 3600
    # quotas and limits
    max_json_body_bytes: int = 1024 * 1024
    max_chunk_bytes: int = 8 * 1024 * 1024
    max_artifact_bytes: int = 256 * 1024 * 1024
    workspace_artifact_quota_bytes: int = 10 * 1024 * 1024 * 1024
    max_active_tasks: int = 200
    rate_limit_per_minute: int = 1200
    rate_limit_burst: int = 200
    auth_rate_limit_per_minute: int = 10
    auth_rate_limit_burst: int = 5
    failed_auth_per_minute: int = 30
    artifact_retention_days: float = 30.0
    upload_abandon_hours: float = 24.0
    sweep_interval_seconds: float = 5.0
    cleanup_interval_seconds: float = 300.0
    idle_timeout_seconds: float = 30.0
    shutdown_grace_seconds: float = 20.0

    # ---------------------------------------------------------------- loading
    @classmethod
    def load(cls, env: dict | None = None, file_path: str | None = None) -> "Config":
        env = dict(os.environ if env is None else env)
        cfg = cls()
        path = file_path or env.get(ENV_PREFIX + "CONFIG")
        if path:
            try:
                with open(path, "r", encoding="utf-8") as fh:
                    data = json.load(fh)
            except (OSError, ValueError) as exc:
                raise ConfigError(f"cannot read config file: {exc.__class__.__name__}") from None
            if not isinstance(data, dict):
                raise ConfigError("config file must contain a JSON object")
            for k, v in data.items():
                if hasattr(cfg, k):
                    cfg._set(k, v)
        for f in fields(cls):
            key = ENV_PREFIX + f.name.upper()
            if key in env and env[key] != "":
                cfg._set(f.name, env[key])
            if f.name in SECRET_KEYS and env.get(key + "_FILE"):
                try:
                    with open(env[key + "_FILE"], "r", encoding="utf-8") as fh:
                        cfg._set(f.name, fh.read().strip())
                except OSError as e:
                    # Name the failure class (never the path contents) so operators can fix ownership:
                    # Compose bind-mounts file secrets with host owner/mode intact.
                    reason = e.strerror or type(e).__name__
                    hint = (f"; the file must be readable by UID {os.getuid()}"
                            if isinstance(e, PermissionError) and hasattr(os, "getuid") else "")
                    raise ConfigError(f"cannot read secret file for {f.name}: {reason}{hint}") from None
        cfg.validate()
        return cfg

    def _set(self, name: str, value) -> None:
        current = getattr(self, name)
        try:
            if isinstance(current, bool):
                if isinstance(value, bool):
                    v = value
                else:
                    v = str(value).strip().lower() in ("1", "true", "yes", "on")
            elif isinstance(current, int):
                v = int(value)
            elif isinstance(current, float):
                v = float(value)
            elif isinstance(current, list):
                if isinstance(value, list):
                    v = [str(x).strip() for x in value if str(x).strip()]
                else:
                    v = [x.strip() for x in str(value).split(",") if x.strip()]
            else:
                v = str(value)
        except (TypeError, ValueError):
            raise ConfigError(f"invalid value for {name}") from None
        setattr(self, name, v)

    def validate(self) -> None:
        if not (0 <= self.port <= 65535):
            raise ConfigError("port out of range")
        if bool(self.tls_cert) != bool(self.tls_key):
            raise ConfigError("tls_cert and tls_key must be set together")
        if self.max_chunk_bytes <= 0 or self.max_artifact_bytes < 0:
            raise ConfigError("invalid size limits")
        for p in self.trusted_proxies:
            try:
                ipaddress.ip_network(p, strict=False)
            except ValueError:
                raise ConfigError("invalid trusted_proxies entry") from None

    # ------------------------------------------------------------- semantics
    @property
    def tls_enabled(self) -> bool:
        return bool(self.tls_cert and self.tls_key)

    def bind_is_loopback(self) -> bool:
        host = self.bind.strip("[]")
        if host in ("localhost",):
            return True
        try:
            return ipaddress.ip_address(host).is_loopback
        except ValueError:
            return False

    def check_transport(self) -> None:
        """Plain HTTP only on loopback or with explicit opt-in (contract §1)."""
        if self.tls_enabled or self.bind_is_loopback() or self.allow_insecure_http:
            return
        raise ConfigError(
            "refusing plain HTTP on a non-loopback address: configure tls_cert/tls_key "
            "or set ASGEND_CP_ALLOW_INSECURE_HTTP=1 behind a TLS reverse proxy"
        )

    def redacted(self) -> dict:
        out = {}
        for f in fields(self):
            v = getattr(self, f.name)
            if f.name in SECRET_KEYS:
                v = "[REDACTED]" if v else ""
                if f.name == "db_url" and getattr(self, f.name):
                    v = redact_db_url(getattr(self, f.name))
            out[f.name] = v
        return out


def redact_db_url(url: str) -> str:
    # postgresql://user:password@host/db -> postgresql://user:[REDACTED]@host/db
    if "://" in url and "@" in url:
        scheme, rest = url.split("://", 1)
        creds, host = rest.rsplit("@", 1)
        if ":" in creds:
            creds = creds.split(":", 1)[0] + ":[REDACTED]"
        return f"{scheme}://{creds}@{host}"
    return url
