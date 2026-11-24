"""Administrative operations shared by the CLI and tests (direct DB access)."""

from __future__ import annotations

from .db import Storage
from .util import dumps, new_id, new_token, sha256_hex
import time

ROLES = ("owner", "operator", "viewer")


class AdminError(Exception):
    pass


def _now() -> int:
    return int(time.time() * 1000)


def _audit(t, ws, action, target, details=None):
    t.x("INSERT INTO audit (audit_id, workspace_id, actor, action, target, result, request_id, created_at, "
        "details_json) VALUES (?, ?, 'system', ?, ?, 'ok', NULL, ?, ?)",
        (new_id("aud_"), ws, action, target, _now(), dumps(details or {})))


def create_workspace(st: Storage, name: str, owner: str | None = None) -> str:
    ws = new_id("ws_")
    with st.tx() as t:
        t.x("INSERT INTO workspaces (workspace_id, name, created_at, event_seq) VALUES (?, ?, ?, 0)",
            (ws, name, _now()))
        _audit(t, ws, "admin.create_workspace", ws)
        if owner:
            _add_member(t, ws, owner, "owner")
    return ws


def issue_user_token(t, user_id: str, name: str = "admin-cli") -> tuple:
    token = new_token("ucp_")
    tid = new_id("tok_")
    t.x("INSERT INTO tokens (token_hash, token_id, kind, user_id, name, created_at) VALUES "
        "(?, ?, 'user', ?, ?, ?)", (sha256_hex(token), tid, user_id, name, _now()))
    return token, tid


def create_user(st: Storage, name: str, workspace: str | None = None, role: str = "owner") -> dict:
    """Creates a user and prints-once token (returned to the caller only)."""
    uid = new_id("usr_")
    with st.tx() as t:
        t.x("INSERT INTO users (user_id, name, created_at) VALUES (?, ?, ?)", (uid, name, _now()))
        token, tid = issue_user_token(t, uid)
        _audit(t, None, "admin.create_user", uid, {"token_id": tid})
        if workspace:
            _add_member(t, workspace, uid, role)
    return {"user_id": uid, "token": token, "token_id": tid}


def new_user_token(st: Storage, user_id: str, name: str = "admin-cli") -> dict:
    with st.tx() as t:
        if t.q1("SELECT user_id FROM users WHERE user_id = ?", (user_id,)) is None:
            raise AdminError("user not found")
        token, tid = issue_user_token(t, user_id, name)
        _audit(t, None, "auth.token_issued", user_id, {"token_id": tid})
    return {"user_id": user_id, "token": token, "token_id": tid}


def _add_member(t, ws, uid, role):
    if role not in ROLES:
        raise AdminError("role must be owner|operator|viewer")
    if t.q1("SELECT workspace_id FROM workspaces WHERE workspace_id = ?", (ws,)) is None:
        raise AdminError("workspace not found")
    if t.q1("SELECT user_id FROM users WHERE user_id = ?", (uid,)) is None:
        raise AdminError("user not found")
    old = t.q1("SELECT role FROM memberships WHERE workspace_id = ? AND user_id = ?", (ws, uid))
    if old:
        t.x("UPDATE memberships SET role = ? WHERE workspace_id = ? AND user_id = ?", (role, ws, uid))
    else:
        t.x("INSERT INTO memberships (workspace_id, user_id, role, created_at) VALUES (?, ?, ?, ?)",
            (ws, uid, role, _now()))
    _audit(t, ws, "member.role_change", uid, {"old_role": old["role"] if old else None, "new_role": role})


def add_member(st: Storage, ws: str, uid: str, role: str) -> None:
    with st.tx() as t:
        _add_member(t, ws, uid, role)


def revoke_token(st: Storage, token: str | None = None, token_id: str | None = None,
                 user_id: str | None = None, node_id: str | None = None) -> int:
    now = _now()
    with st.tx() as t:
        if token:
            n = t.x("UPDATE tokens SET revoked_at = ? WHERE token_hash = ? AND revoked_at IS NULL",
                    (now, sha256_hex(token)))
        elif token_id:
            n = t.x("UPDATE tokens SET revoked_at = ? WHERE token_id = ? AND revoked_at IS NULL", (now, token_id))
        elif user_id:
            n = t.x("UPDATE tokens SET revoked_at = ? WHERE user_id = ? AND revoked_at IS NULL", (now, user_id))
        elif node_id:
            n = t.x("UPDATE tokens SET revoked_at = ? WHERE node_id = ? AND revoked_at IS NULL", (now, node_id))
        else:
            raise AdminError("specify --token-stdin, --token-id, --user or --node")
        _audit(t, None, "admin.revoke_token", token_id or user_id or node_id or "token", {"revoked": n})
    return n


def listing(st: Storage, what: str) -> list:
    queries = {
        "users": "SELECT user_id, name, created_at, disabled FROM users ORDER BY user_id",
        "workspaces": "SELECT workspace_id, name, created_at FROM workspaces ORDER BY workspace_id",
        "members": "SELECT workspace_id, user_id, role, created_at FROM memberships ORDER BY workspace_id, user_id",
        "nodes": "SELECT node_id, workspace_id, name, revoked_at, last_seen_at, created_at FROM nodes ORDER BY node_id",
        "tokens": "SELECT token_id, kind, user_id, node_id, name, created_at, expires_at, revoked_at, last_used_at "
                  "FROM tokens ORDER BY token_id",
    }
    if what not in queries:
        raise AdminError("list one of: " + ", ".join(sorted(queries)))
    with st.read() as t:
        return t.q(queries[what])
