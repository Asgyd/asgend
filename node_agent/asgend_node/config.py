"""Node agent configuration (TOML file + CLI overrides)."""

from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass, field, fields
from typing import Any


def default_state_dir() -> str:
    env = os.environ.get("ASGEND_NODE_STATE_DIR")
    if env:
        return env
    if os.name == "posix" and os.geteuid() == 0:  # type: ignore[attr-defined]
        return "/var/lib/asgend-node"
    base = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(base, "asgend-node")


@dataclass
class NodeConfig:
    state_dir: str = field(default_factory=default_state_dir)
    # Control Plane (server_url normally comes from the credentials file written by enroll)
    server_url: str | None = None
    ca_file: str | None = None
    allow_insecure_http: bool = False
    http_timeout: float = 30.0
    # Local core
    core_host: str = "127.0.0.1"
    core_port: int = 8765
    core_unix_socket: str | None = None
    core_auth_token: str | None = None
    core_auth_token_file: str | None = None
    core_timeout: float = 30.0
    core_max_response_bytes: int = 8 * 1024 * 1024
    # Loops
    lease_max: int = 10
    lease_wait_ms: int = 20000
    event_poll_interval: float = 1.0
    event_batch_size: int = 500
    core_events_page: int = 100
    heartbeat_interval_seconds: float | None = None  # None -> value returned by enroll
    backoff_initial: float = 1.0
    backoff_max: float = 60.0
    # Artifacts
    artifacts_enabled: bool = True
    max_artifact_bytes: int = 256 * 1024 * 1024
    artifact_read_chunk: int = 1024 * 1024
    # Events
    forward_core_payloads: bool = True
    log_level: str = "INFO"

    def resolved_core_token(self) -> str | None:
        if self.core_auth_token:
            return self.core_auth_token
        env = os.environ.get("ASGEND_CORE_TOKEN")
        if env:
            return env
        if self.core_auth_token_file:
            with open(self.core_auth_token_file, "r", encoding="utf-8") as fh:
                return fh.read().strip() or None
        return None


_SECTIONS = {
    "node": {"state_dir": "state_dir", "log_level": "log_level"},
    "control_plane": {"url": "server_url", "ca_file": "ca_file", "allow_insecure_http": "allow_insecure_http",
                      "http_timeout": "http_timeout", "lease_wait_ms": "lease_wait_ms",
                      "lease_max": "lease_max", "heartbeat_interval_seconds": "heartbeat_interval_seconds",
                      "backoff_initial": "backoff_initial", "backoff_max": "backoff_max"},
    "core": {"host": "core_host", "port": "core_port", "unix_socket": "core_unix_socket",
             "auth_token": "core_auth_token", "auth_token_file": "core_auth_token_file",
             "timeout": "core_timeout", "max_response_bytes": "core_max_response_bytes"},
    "events": {"poll_interval": "event_poll_interval", "batch_size": "event_batch_size",
               "core_page": "core_events_page", "forward_core_payloads": "forward_core_payloads"},
    "artifacts": {"enabled": "artifacts_enabled", "max_bytes": "max_artifact_bytes",
                  "read_chunk": "artifact_read_chunk"},
}


def load_config(path: str | None, overrides: dict[str, Any] | None = None) -> NodeConfig:
    cfg = NodeConfig()
    known = {f.name for f in fields(NodeConfig)}
    if path:
        with open(path, "rb") as fh:
            data = tomllib.load(fh)
        for section, mapping in _SECTIONS.items():
            values = data.get(section) or {}
            if not isinstance(values, dict):
                raise ValueError(f"config section [{section}] must be a table")
            for key, value in values.items():
                if key not in mapping:
                    raise ValueError(f"unknown config key [{section}].{key}")
                setattr(cfg, mapping[key], value)
    for key, value in (overrides or {}).items():
        if value is not None and key in known:
            setattr(cfg, key, value)
    cfg.event_batch_size = max(1, min(int(cfg.event_batch_size), 500))
    cfg.lease_wait_ms = max(0, min(int(cfg.lease_wait_ms), 30000))
    cfg.lease_max = max(1, min(int(cfg.lease_max), 50))
    cfg.artifact_read_chunk = max(1, min(int(cfg.artifact_read_chunk), 1048576))
    cfg.core_events_page = max(1, min(int(cfg.core_events_page), 500))
    return cfg
