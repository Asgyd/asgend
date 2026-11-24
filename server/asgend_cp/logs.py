"""Structured JSON logging with secret redaction.

Rules: no request/response bodies are logged, task text is never logged, and
any token-looking value (ucp_/ncp_/dlt_ prefixes, Bearer credentials,
password-in-URL) is redacted before the record is written.
"""

from __future__ import annotations

import json
import logging
import re
import sys
import time

LOGGER_NAME = "asgend_cp"

_TOKEN_RE = re.compile(r"\b(ucp_|ncp_|dlt_)[A-Za-z0-9_\-]+")
_BEARER_RE = re.compile(r"(?i)\b(bearer)\s+[^\s\"',;]+")
_URL_PW_RE = re.compile(r"(://[^:/@\s]+:)[^@\s/]+@")
_SECRET_KEYS = {
    "authorization", "token", "node_token", "ticket", "code", "enrollment_code",
    "password", "secret", "metrics_token", "db_password", "answer", "task",
    "payload", "question", "body",
}


def redact(value):
    if isinstance(value, str):
        value = _TOKEN_RE.sub(lambda m: m.group(1) + "[REDACTED]", value)
        value = _BEARER_RE.sub(lambda m: m.group(1) + " [REDACTED]", value)
        value = _URL_PW_RE.sub(lambda m: m.group(1) + "[REDACTED]@", value)
        return value
    if isinstance(value, dict):
        return {
            k: ("[REDACTED]" if str(k).lower() in _SECRET_KEYS and v not in (None, "") else redact(v))
            for k, v in value.items()
        }
    if isinstance(value, (list, tuple)):
        return [redact(v) for v in value]
    return value


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        out = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(record.created))
            + ".%03dZ" % int(record.msecs),
            "level": record.levelname.lower(),
            "logger": record.name,
            "msg": record.getMessage(),
        }
        fields = getattr(record, "fields", None)
        if isinstance(fields, dict):
            for k, v in fields.items():
                if k not in out:
                    out[k] = v
        if record.exc_info:
            # exception type only: messages/tracebacks can carry user data
            out["exc_type"] = record.exc_info[0].__name__ if record.exc_info[0] else None
        return json.dumps(redact(out), ensure_ascii=False, default=str)


def get_logger(name: str | None = None) -> logging.Logger:
    return logging.getLogger(LOGGER_NAME if not name else f"{LOGGER_NAME}.{name}")


def setup_logging(level: str = "INFO", stream=None) -> logging.Handler:
    logger = logging.getLogger(LOGGER_NAME)
    logger.setLevel(getattr(logging, str(level).upper(), logging.INFO))
    handler = logging.StreamHandler(stream or sys.stderr)
    handler.setFormatter(JsonFormatter())
    for h in list(logger.handlers):
        if getattr(h, "_asgend_cp", False):
            logger.removeHandler(h)
    handler._asgend_cp = True  # type: ignore[attr-defined]
    logger.addHandler(handler)
    logger.propagate = False
    return handler


def log(logger: logging.Logger, level: int, msg: str, **fields) -> None:
    if logger.isEnabledFor(level):
        logger.log(level, msg, extra={"fields": fields})
