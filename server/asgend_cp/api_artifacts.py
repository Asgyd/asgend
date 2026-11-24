"""Artifact upload (node), metadata/ticket (user) and ticket download."""

from __future__ import annotations

import re
from urllib.parse import quote

from . import validate as V
from .http import ApiError, Response
from .util import SHA256_RE, new_id, new_token, rfc3339, sha256_hex

_CONTENT_RANGE_RE = re.compile(r"^bytes (\d{1,15})-(\d{1,15})/(\d{1,15})$")
_RANGE_RE = re.compile(r"^bytes=(\d{0,15})-(\d{0,15})$")


def content_disposition(name: str) -> str:
    fallback = re.sub(r"[^A-Za-z0-9._ -]", "_", name).strip() or "artifact"
    fallback = fallback.replace('"', "_")
    return f"attachment; filename=\"{fallback}\"; filename*=UTF-8''{quote(name, safe='')}"


class ArtifactApi:
    # ----------------------------------------------------------- node side
    def h_artifact_init(self, req):
        p = req.principal
        body = req.json(self.config.max_json_body_bytes)
        task_id = V.req_str(body, "task_id", 80)
        name = V.sanitize_artifact_name(body.get("name"))
        mime = V.normalize_mime(body.get("mime"))
        size = body.get("size")
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise V.bad("size", "must be a non-negative integer")
        digest = body.get("sha256")
        if not isinstance(digest, str) or not SHA256_RE.match(digest.lower()):
            raise V.bad("sha256", "must be 64 hex characters")
        digest = digest.lower()
        if size > self.config.max_artifact_bytes:
            self.metrics.inc("asgend_cp_quota_rejections_total", {"quota": "artifact_size"},
                             help="Requests rejected by quotas")
            raise ApiError("payload_too_large", "artifact exceeds maximum size",
                           {"limit": self.config.max_artifact_bytes})
        now = self.clock.now_ms()
        with self.storage.tx() as t:
            self.lock_ws(t, p.workspace_id)
            task = t.q1("SELECT * FROM tasks WHERE task_id = ?", (task_id,))
            if task is None or task["node_id"] != p.node_id:
                raise ApiError("not_found", "task not found", {"task_id": task_id})
            existing = t.q1("SELECT * FROM artifacts WHERE task_id = ? AND name = ? AND sha256 = ? AND "
                            "size = ? AND state = 'uploading' ORDER BY artifact_id LIMIT 1",
                            (task_id, name, digest, size))
            if existing:  # resumable re-init after node restart
                return Response.json(self._upload_info(existing), 200)
            used = t.q1("SELECT COALESCE(SUM(size), 0) AS b FROM artifacts WHERE workspace_id = ? AND "
                        "state IN ('uploading', 'available')", (task["workspace_id"],))
            if used["b"] + size > self.config.workspace_artifact_quota_bytes:
                self.metrics.inc("asgend_cp_quota_rejections_total", {"quota": "workspace_artifact_bytes"},
                                 help="Requests rejected by quotas")
                raise ApiError("quota_exceeded", "workspace artifact storage quota exceeded",
                               {"quota": "workspace_artifact_bytes",
                                "limit": self.config.workspace_artifact_quota_bytes,
                                "used": int(used["b"])}, headers={"Retry-After": "3600"})
            aid = new_id("art_")
            t.x("INSERT INTO artifacts (artifact_id, task_id, workspace_id, node_id, name, mime, size, "
                "sha256, state, received, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'uploading', 0, ?)",
                (aid, task_id, task["workspace_id"], p.node_id, name, mime, size, digest, now))
            a = t.q1("SELECT * FROM artifacts WHERE artifact_id = ?", (aid,))
        return Response.json(self._upload_info(a), 201)

    def _upload_info(self, a: dict) -> dict:
        return {
            "artifact_id": a["artifact_id"],
            "upload_url": f"/v1/node/artifacts/{a['artifact_id']}/content",
            "chunk_size": self.config.max_chunk_bytes,
            "received": int(a["received"]),
            "state": a["state"],
        }

    def _node_artifact(self, t, p, aid: str) -> dict:
        a = t.q1("SELECT * FROM artifacts WHERE artifact_id = ?", (aid,))
        if a is None or a["node_id"] != p.node_id:
            raise ApiError("not_found", "artifact not found")
        return a

    def h_artifact_received(self, req):
        with self.storage.read() as t:
            a = self._node_artifact(t, req.principal, req.params["art"])
        return Response.json({"artifact_id": a["artifact_id"], "received": int(a["received"]),
                              "size": int(a["size"]), "state": a["state"]})

    def h_artifact_put(self, req):
        p = req.principal
        aid = req.params["art"]
        with self.storage.read() as t:
            a = self._node_artifact(t, p, aid)
        if a["state"] != "uploading":
            raise ApiError("conflict", "artifact is not accepting uploads", {"state": a["state"]})
        m = _CONTENT_RANGE_RE.match(req.header("Content-Range").strip())
        if not m:
            raise ApiError("invalid_request", "Content-Range: bytes a-b/total is required")
        start, end, total = (int(x) for x in m.groups())
        size = int(a["size"])
        if total != size or start > end or end >= size:
            raise ApiError("invalid_request", "Content-Range does not match the declared size",
                           {"size": size})
        length = end - start + 1
        if length > self.config.max_chunk_bytes:
            raise ApiError("payload_too_large", "chunk too large", {"limit": self.config.max_chunk_bytes})
        if "chunked" in req.header("Transfer-Encoding").lower():
            raise ApiError("invalid_request", "chunked transfer encoding is not supported for content")
        if req.content_length() != length:
            raise ApiError("invalid_request", "Content-Length must equal the Content-Range length")
        with self.blobs.lock_for(aid):
            with self.storage.read() as t:
                a = self._node_artifact(t, p, aid)
            received = min(int(a["received"]), self.blobs.tmp_size(aid))
            if a["state"] != "uploading":
                raise ApiError("conflict", "artifact is not accepting uploads", {"state": a["state"]})
            if start != received:
                raise ApiError("conflict", "chunk does not start at the received offset",
                               {"received": received})
            written = self.blobs.write_chunk(aid, start, req.handler.rfile, length)
            req.body_consumed = written == length
            if written != length:
                self.blobs.truncate(aid, start)
                req.handler.close_connection = True
                raise ApiError("invalid_request", "truncated chunk", {"received": received})
            with self.storage.tx() as t:
                t.x("UPDATE artifacts SET received = ? WHERE artifact_id = ? AND state = 'uploading'",
                    (end + 1, aid))
        self.metrics.inc("asgend_cp_artifact_upload_bytes_total", value=length,
                         help="Artifact bytes received from nodes")
        return Response.json({"artifact_id": aid, "received": end + 1, "size": size})

    def h_artifact_complete(self, req):
        p = req.principal
        aid = req.params["art"]
        req.json(self.config.max_json_body_bytes)  # body ignored; drained
        with self.blobs.lock_for(aid):
            with self.storage.read() as t:
                a = self._node_artifact(t, p, aid)
            if a["state"] == "available":
                return Response.json(self.artifact_out(a))
            if a["state"] != "uploading":
                raise ApiError("conflict", "artifact is not uploading", {"state": a["state"]})
            digest, n = self.blobs.hash_tmp(aid)
            size = int(a["size"])
            if n < size:
                raise ApiError("conflict", "upload incomplete", {"received": n, "size": size})
            now = self.clock.now_ms()
            if n != size or digest != a["sha256"]:
                self.blobs.discard_tmp(aid)
                with self.storage.tx() as t:
                    t.x("UPDATE artifacts SET state = 'rejected', received = 0, completed_at = ? "
                        "WHERE artifact_id = ?", (now, aid))
                    self.audit(t, a["workspace_id"], p.actor, "artifact.reject", aid, "denied", req,
                               {"reason": "checksum_mismatch"})
                self.metrics.inc("asgend_cp_artifact_checksum_mismatch_total",
                                 help="Artifact uploads rejected by SHA-256/size verification")
                raise ApiError("checksum_mismatch", "SHA-256 or size verification failed; object deleted",
                               {"artifact_id": aid})
            exp = now + int(self.config.artifact_retention_days * 86_400_000) \
                if self.config.artifact_retention_days > 0 else None
            with self.blob_lock:
                self.blobs.commit(aid, digest)
                with self.storage.tx() as t:
                    self.lock_ws(t, a["workspace_id"])
                    t.x("UPDATE artifacts SET state = 'available', blob_key = ?, received = ?, "
                        "completed_at = ?, expires_at = ? WHERE artifact_id = ?",
                        (digest, size, now, exp, aid))
                    self.emit(t, a["workspace_id"], a["task_id"], "artifact_available",
                              {"artifact_id": aid, "name": a["name"], "mime": a["mime"], "size": size,
                               "sha256": digest}, node_id=p.node_id)
                    a = t.q1("SELECT * FROM artifacts WHERE artifact_id = ?", (aid,))
        self.blobs.drop_lock(aid)
        return Response.json(self.artifact_out(a))

    # ----------------------------------------------------------- user side
    def _artifact_for_user(self, req, t):
        a = t.q1("SELECT * FROM artifacts WHERE artifact_id = ?", (req.params["art"],))
        if a is None:
            raise ApiError("not_found", "not found")
        self.require_role(req, t, a["workspace_id"], "viewer")
        return a

    def h_task_artifacts(self, req):
        with self.storage.read() as t:
            task = self._task_for_user(req, t)
            rows = t.q("SELECT * FROM artifacts WHERE task_id = ? ORDER BY artifact_id", (task["task_id"],))
        return Response.json({"artifacts": [self.artifact_out(a) for a in rows]})

    def h_artifact_get(self, req):
        with self.storage.read() as t:
            a = self._artifact_for_user(req, t)
        return Response.json(self.artifact_out(a))

    def h_download_ticket(self, req):
        with self.storage.read() as t:
            a = self._artifact_for_user(req, t)
        if a["state"] == "expired":
            raise ApiError("gone", "artifact expired")
        if a["state"] != "available":
            raise ApiError("conflict", "artifact is not available", {"state": a["state"]})
        now = self.clock.now_ms()
        exp = now + self.config.download_ticket_ttl_seconds * 1000
        ticket = new_token("dlt_")
        with self.storage.tx() as t:
            t.x("INSERT INTO download_tickets (ticket_hash, artifact_id, user_id, workspace_id, created_at, "
                "expires_at) VALUES (?, ?, ?, ?, ?, ?)",
                (sha256_hex(ticket), a["artifact_id"], req.principal.user_id, a["workspace_id"], now, exp))
            self.audit(t, a["workspace_id"], req.principal.actor, "artifact.download_ticket",
                       a["artifact_id"], "ok", req)
        return Response.json({"ticket": ticket, "url": f"/v1/downloads/{ticket}",
                              "expires_at": rfc3339(exp), "artifact_id": a["artifact_id"]}, 201)

    def h_download(self, req):
        ticket = req.params["ticket"]
        if not ticket.startswith("dlt_"):
            raise ApiError("not_found", "not found")
        now = self.clock.now_ms()
        with self.storage.read() as t:
            tk = t.q1("SELECT * FROM download_tickets WHERE ticket_hash = ?", (sha256_hex(ticket),))
            if tk is None:
                raise ApiError("not_found", "not found")
            if tk["expires_at"] <= now:
                raise ApiError("gone", "download ticket expired")
            # ticket is bound to user + artifact; membership is re-checked on every use
            role = self.role_in(t, tk["user_id"], tk["workspace_id"])
            user = t.q1("SELECT disabled FROM users WHERE user_id = ?", (tk["user_id"],))
            a = t.q1("SELECT * FROM artifacts WHERE artifact_id = ?", (tk["artifact_id"],))
        if role is None or user is None or user["disabled"] or a is None or a["workspace_id"] != tk["workspace_id"]:
            raise ApiError("not_found", "not found")
        if a["state"] != "available":
            raise ApiError("gone", "artifact no longer available")
        size = int(a["size"])
        start, end, status = 0, size - 1, 200
        rng = req.header("Range").strip()
        headers = {
            "Content-Type": a["mime"],
            "X-Checksum-SHA256": a["sha256"],
            "Content-Disposition": content_disposition(a["name"]),
            "Accept-Ranges": "bytes",
            "ETag": f"\"{a['sha256']}\"",
            "Cache-Control": "private, no-store",
        }
        if rng:
            m = _RANGE_RE.match(rng)
            if m and (m.group(1) or m.group(2)):
                s, e = m.group(1), m.group(2)
                if s == "":
                    n = int(e)
                    start, end = max(0, size - n), size - 1
                else:
                    start = int(s)
                    end = min(int(e), size - 1) if e else size - 1
                if size == 0 or start >= size or start > end or (s == "" and int(e) == 0):
                    raise ApiError("invalid_request", "range not satisfiable", {"size": size}, status=416,
                                   headers={"Content-Range": f"bytes */{size}"})
                status = 206
                headers["Content-Range"] = f"bytes {start}-{end}/{size}"
            # malformed or multi-range: ignored, full content served (RFC 9110)
        length = max(0, end - start + 1) if size else 0
        try:
            fh = self.blobs.open_blob(a["blob_key"])
        except (OSError, ValueError):
            raise ApiError("gone", "artifact content missing") from None
        fh.seek(start)
        if start == 0:
            self.audit_now(a["workspace_id"], tk["user_id"], "artifact.download", a["artifact_id"], "ok",
                           req, {"range": status == 206})
        self.metrics.inc("asgend_cp_artifact_download_bytes_total", value=length,
                         help="Artifact bytes served to clients")
        return Response(status, b"", headers, stream=fh, length=length)
