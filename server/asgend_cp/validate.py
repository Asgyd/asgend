"""Request field validation helpers (unknown fields are always ignored)."""

from __future__ import annotations

import re

from .http import ApiError
from .util import IDEMPOTENCY_RE

_LABEL_RE = re.compile(r"^[A-Za-z0-9._:/=+-]{1,64}$")


def bad(field: str, why: str) -> ApiError:
    return ApiError("invalid_request", f"invalid field '{field}': {why}", {"field": field})


def req_str(body: dict, field: str, max_len: int = 1000, min_len: int = 1,
            required: bool = True, default=None):
    v = body.get(field)
    if v is None:
        if required:
            raise bad(field, "required")
        return default
    if not isinstance(v, str):
        raise bad(field, "must be a string")
    if len(v) < min_len or len(v) > max_len:
        raise bad(field, f"length must be {min_len}..{max_len}")
    if "\x00" in v:
        raise bad(field, "NUL not allowed")
    return v


def req_int(body: dict, field: str, lo: int, hi: int, required: bool = True, default=None):
    v = body.get(field)
    if v is None:
        if required:
            raise bad(field, "required")
        return default
    if isinstance(v, bool) or not isinstance(v, int):
        raise bad(field, "must be an integer")
    if v < lo or v > hi:
        raise bad(field, f"must be in {lo}..{hi}")
    return v


def query_int(query: dict, field: str, lo: int, hi: int, default: int) -> int:
    v = query.get(field)
    if v is None or v == "":
        return default
    try:
        iv = int(v)
    except ValueError:
        raise bad(field, "must be an integer") from None
    if iv < lo or iv > hi:
        raise bad(field, f"must be in {lo}..{hi}")
    return iv


def idem_key(body: dict, field: str = "idempotency_key") -> str:
    v = body.get(field)
    if not isinstance(v, str) or not IDEMPOTENCY_RE.match(v):
        raise bad(field, "must match ^[A-Za-z0-9._:-]{8,128}$")
    return v


def labels(body: dict, field: str = "labels", default=None):
    v = body.get(field)
    if v is None:
        return default if default is not None else []
    if not isinstance(v, list) or len(v) > 32:
        raise bad(field, "must be a list of at most 32 labels")
    out = []
    for x in v:
        if not isinstance(x, str) or not _LABEL_RE.match(x):
            raise bad(field, "labels must match ^[A-Za-z0-9._:/=+-]{1,64}$")
        if x not in out:
            out.append(x)
    return out


def sanitize_artifact_name(name) -> str:
    """Basename only: no '/', '\\', NUL, control chars or '..'; <=255 bytes UTF-8."""
    if not isinstance(name, str) or not name:
        raise bad("name", "required")
    if "/" in name or "\\" in name:
        raise bad("name", "path separators are not allowed")
    if "\x00" in name or any(ord(ch) < 32 or ord(ch) == 127 for ch in name):
        raise bad("name", "control characters are not allowed")
    if ".." in name or name.strip() in (".", ""):
        raise bad("name", "'.' / '..' are not allowed")
    if len(name) > 255 or len(name.encode("utf-8")) > 255:
        raise bad("name", "must be at most 255 bytes")
    return name


SAFE_MIME = {
    "text/plain", "text/markdown", "text/csv", "text/x-diff", "text/x-patch", "text/tab-separated-values",
    "application/json", "application/x-ndjson", "application/yaml", "application/pdf",
    "application/zip", "application/gzip", "application/x-gzip", "application/x-tar",
    "application/x-xz", "application/x-bzip2", "application/x-7z-compressed", "application/zstd",
    "image/png", "image/jpeg", "image/gif", "image/webp", "application/octet-stream",
}
_ACTIVE_TEXT = {"text/html", "text/javascript", "text/css", "text/xml", "text/xsl", "text/ecmascript",
                "text/vtt"}
_MIME_RE = re.compile(r"^[a-z0-9][a-z0-9!#$&^_.+-]{0,63}/[a-z0-9][a-z0-9!#$&^_.+-]{0,126}$")


def normalize_mime(mime) -> str:
    """Map a declared MIME type to a sniff-safe stored type (allow-list)."""
    if not isinstance(mime, str):
        return "application/octet-stream"
    m = mime.split(";", 1)[0].strip().lower()
    if not _MIME_RE.match(m):
        return "application/octet-stream"
    if m in SAFE_MIME:
        return m
    if m.startswith("text/") and m not in _ACTIVE_TEXT:
        return "text/plain"
    return "application/octet-stream"
