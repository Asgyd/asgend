"""Identifiers, tokens, time helpers and small validation utilities."""

from __future__ import annotations

import datetime as _dt
import hashlib
import hmac
import json
import os
import re
import secrets
import threading
import time

CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

TOKEN_PREFIXES = ("ucp_", "ncp_", "dlt_")

_ulid_lock = threading.Lock()
_ulid_last_ms = 0
_ulid_last_rand = 0


def _b32(value: int, length: int) -> str:
    out = []
    for _ in range(length):
        out.append(CROCKFORD[value & 31])
        value >>= 5
    return "".join(reversed(out))


def new_id(prefix: str, now_ms: int | None = None) -> str:
    """prefix + 26 chars Crockford base32 (48-bit ms time + 80-bit randomness).

    Monotonic within a process: ids generated in the same millisecond are
    strictly increasing, so lexical order == creation order.
    """
    global _ulid_last_ms, _ulid_last_rand
    ms = int(time.time() * 1000) if now_ms is None else int(now_ms)
    with _ulid_lock:
        if ms <= _ulid_last_ms:
            ms = _ulid_last_ms
            rnd = (_ulid_last_rand + 1) & ((1 << 80) - 1)
        else:
            rnd = int.from_bytes(os.urandom(10), "big") & ((1 << 79) - 1)
        _ulid_last_ms, _ulid_last_rand = ms, rnd
    return prefix + _b32(ms & ((1 << 48) - 1), 10) + _b32(rnd, 16)


def new_token(prefix: str) -> str:
    assert prefix in TOKEN_PREFIXES
    return prefix + secrets.token_urlsafe(32)


def new_code(length: int) -> str:
    """Human-typable single-use code (Crockford base32, no ambiguous chars)."""
    return "".join(secrets.choice(CROCKFORD) for _ in range(length))


def normalize_code(code: str) -> str:
    code = code.strip().upper().replace("-", "").replace(" ", "")
    return code.replace("O", "0").replace("I", "1").replace("L", "1")


def sha256_hex(data: str | bytes) -> str:
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def consteq(a: str, b: str) -> bool:
    return hmac.compare_digest(a.encode("utf-8"), b.encode("utf-8"))


def canonical_json(obj) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def dumps(obj) -> str:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


class Clock:
    """Wall clock with an adjustable offset (tests advance it to exercise TTLs)."""

    def __init__(self) -> None:
        self.offset_ms = 0

    def now_ms(self) -> int:
        return int(time.time() * 1000) + self.offset_ms

    def advance(self, seconds: float) -> None:
        self.offset_ms += int(seconds * 1000)


def rfc3339(ms: int | None) -> str | None:
    if ms is None:
        return None
    dt = _dt.datetime.fromtimestamp(ms / 1000.0, tz=_dt.timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (ms % 1000)


_RFC3339_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}[Tt ]\d{2}:\d{2}:\d{2}(\.\d{1,9})?([Zz]|[+-]\d{2}:\d{2})$"
)


def parse_rfc3339(value: str) -> int:
    """Return epoch milliseconds; raises ValueError on malformed input."""
    if not isinstance(value, str) or not _RFC3339_RE.match(value):
        raise ValueError("not RFC3339")
    v = value.replace("z", "Z").replace("t", "T").replace(" ", "T")
    if v.endswith("Z"):
        v = v[:-1] + "+00:00"
    main, _, rest = v.partition(".")
    frac = ""
    tz = ""
    if rest:
        m = re.match(r"(\d+)(.*)$", rest)
        frac, tz = m.group(1), m.group(2)
        v = main + "." + frac[:6].ljust(6, "0") + tz
    dt = _dt.datetime.fromisoformat(v)
    return int(dt.timestamp() * 1000)


IDEMPOTENCY_RE = re.compile(r"^[A-Za-z0-9._:-]{8,128}$")
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
OBJECT_ID_RE = r"[A-Za-z0-9_]{1,80}"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
