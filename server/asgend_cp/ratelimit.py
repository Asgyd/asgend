"""Bounded in-memory token-bucket rate limiter.

The bucket table is a hard memory bound. Once the per-key table is full,
previously unseen keys share a single overflow bucket until stale entries are
pruned. This is deliberately fail-safe: unique-key churn cannot grow memory
without limit or let every new key receive a fresh burst. Use a distributed
limiter for multi-replica production deployments.
"""

from __future__ import annotations

import math
import threading
import time


_OVERFLOW_KEY = object()


class RateLimiter:
    def __init__(self, max_keys: int = 100_000):
        self._max_keys = max(1, int(max_keys))
        # Reserve one slot for the shared bucket used by unknown keys when the
        # table is full. max_keys=1 intentionally means all keys share it.
        self._tracked_capacity = max(0, self._max_keys - 1)
        self._buckets: dict[object, tuple[float, float]] = {}
        self._lock = threading.Lock()
        self._last_prune = 0.0

    def _prune(self, now: float) -> None:
        stale = [k for k, (_, last) in self._buckets.items() if now - last > 600]
        for k in stale:
            self._buckets.pop(k, None)

    def _resolve_key(self, key: str, now: float) -> object:
        if key in self._buckets:
            return key
        # Only scan for stale entries when a new key cannot be stored.
        normal_count = len(self._buckets) - (1 if _OVERFLOW_KEY in self._buckets else 0)
        if normal_count >= self._tracked_capacity:
            # Unique-key churn must not force an O(max_keys) scan per request.
            # Buckets older than 600s are safe to retain until the next sweep;
            # limiting opportunistic sweeps to once per minute bounds CPU work.
            if now - self._last_prune >= 60.0:
                self._prune(now)
                self._last_prune = now
            normal_count = len(self._buckets) - (1 if _OVERFLOW_KEY in self._buckets else 0)
        if normal_count < self._tracked_capacity:
            return key
        return _OVERFLOW_KEY

    def hit(self, key: str, per_minute: float, burst: float, cost: float = 1.0) -> float:
        """Consume ``cost`` tokens. Returns 0 when allowed, else seconds to wait.

        ``per_minute <= 0`` disables limiting for this policy. Invalid token
        costs are rejected so callers cannot accidentally mint tokens with a
        negative cost. A cost above bucket capacity is rejected as not
        satisfiable instead of returning a retry delay that can never help.
        """
        capacity = max(1.0, float(burst))
        if not math.isfinite(cost) or cost <= 0:
            raise ValueError("cost must be a finite positive number")
        if cost > capacity:
            raise ValueError("cost cannot exceed bucket capacity")
        if per_minute <= 0:
            return 0.0

        rate = per_minute / 60.0
        now = time.monotonic()
        with self._lock:
            effective_key = self._resolve_key(key, now)
            tokens, last = self._buckets.get(effective_key, (capacity, now))
            tokens = min(capacity, tokens + (now - last) * rate)
            if tokens >= cost:
                self._buckets[effective_key] = (tokens - cost, now)
                return 0.0
            self._buckets[effective_key] = (tokens, now)
            return max(1.0, math.ceil((cost - tokens) / rate))

    def peek_blocked(self, key: str, per_minute: float, burst: float) -> float:
        """Seconds until one token is available, without consuming."""
        if per_minute <= 0:
            return 0.0
        rate = per_minute / 60.0
        capacity = max(1.0, float(burst))
        now = time.monotonic()
        with self._lock:
            effective_key = self._resolve_key(key, now)
            tokens, last = self._buckets.get(effective_key, (capacity, now))
            tokens = min(capacity, tokens + (now - last) * rate)
            if tokens >= 1.0:
                return 0.0
            return max(1.0, math.ceil((1.0 - tokens) / rate))

    def prune(self) -> None:
        with self._lock:
            now = time.monotonic()
            self._prune(now)
            self._last_prune = now

    def reset(self) -> None:
        with self._lock:
            self._buckets.clear()
