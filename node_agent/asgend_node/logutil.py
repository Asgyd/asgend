"""Logging helpers that guarantee credentials never reach log output."""

from __future__ import annotations

import logging
import re
import sys
import threading

LOGGER_NAME = "asgend_node"

_TOKEN_RE = re.compile(r"\b(ncp|ucp|dlt|enr)_[A-Za-z0-9._\-]{4,}")
_BEARER_RE = re.compile(r"(?i)(bearer\s+)[A-Za-z0-9._\-]+")

_secrets_lock = threading.Lock()
_secrets: set[str] = set()


def register_secret(value: str | None) -> None:
    """Remember an exact secret string so it is masked wherever it appears."""
    if value and len(value) >= 4:
        with _secrets_lock:
            _secrets.add(value)


def redact(text: str) -> str:
    if not text:
        return text
    with _secrets_lock:
        secrets = sorted(_secrets, key=len, reverse=True)
    for secret in secrets:
        if secret in text:
            text = text.replace(secret, "***")
    text = _TOKEN_RE.sub(lambda m: m.group(1) + "_***", text)
    text = _BEARER_RE.sub(lambda m: m.group(1) + "***", text)
    return text


class RedactingFilter(logging.Filter):
    def filter(self, record: logging.LogRecord) -> bool:  # noqa: A003 - logging API
        try:
            message = record.getMessage()
        except Exception:  # pragma: no cover - malformed format args
            message = str(record.msg)
        record.msg = redact(message)
        record.args = None
        if record.exc_info and not record.exc_text:
            record.exc_text = logging.Formatter().formatException(record.exc_info)
        if record.exc_text:
            record.exc_text = redact(record.exc_text)
        return True


_logger = logging.getLogger(LOGGER_NAME)
_logger.addFilter(RedactingFilter())


def get_logger() -> logging.Logger:
    """Return the single package logger. Its own filter redacts every record logged
    through it, also for handlers attached by tests or by an embedding process."""
    return _logger


def setup_logging(level: str = "INFO", stream=None) -> logging.Handler:
    """Install a single redacting stderr handler on the package logger."""
    logger = logging.getLogger(LOGGER_NAME)
    for handler in list(logger.handlers):
        if getattr(handler, "_asgend_node", False):
            logger.removeHandler(handler)
    handler = logging.StreamHandler(stream or sys.stderr)
    handler._asgend_node = True  # type: ignore[attr-defined]
    handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s"))
    handler.addFilter(RedactingFilter())
    logger.addHandler(handler)
    logger.setLevel(getattr(logging, str(level).upper(), logging.INFO))
    logger.propagate = False
    return handler
