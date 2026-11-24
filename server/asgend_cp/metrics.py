"""Prometheus text exposition (no client library).

Label values are restricted to fixed vocabularies (route names, states,
reasons) or opaque workspace ids -- never prompts, paths, names or secrets.
"""

from __future__ import annotations

import threading

LATENCY_BUCKETS = (0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30, 60, 300, 900, 3600)


def _label_str(labels: dict) -> str:
    if not labels:
        return ""
    parts = []
    for k in sorted(labels):
        v = str(labels[k]).replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')
        parts.append(f'{k}="{v}"')
    return "{" + ",".join(parts) + "}"


class Histogram:
    def __init__(self, buckets=LATENCY_BUCKETS):
        self.buckets = tuple(buckets)
        self.counts = [0] * len(self.buckets)
        self.total = 0
        self.sum = 0.0

    def observe(self, v: float) -> None:
        v = max(0.0, float(v))
        self.total += 1
        self.sum += v
        for i, b in enumerate(self.buckets):
            if v <= b:
                self.counts[i] += 1


class Metrics:
    def __init__(self):
        self._lock = threading.Lock()
        self.counters: dict = {}     # (name, frozenset(labels)) -> value
        self.histograms: dict = {}   # name -> Histogram
        self.help: dict = {}

    def inc(self, name: str, labels: dict | None = None, value: float = 1.0, help: str = "") -> None:
        key = (name, tuple(sorted((labels or {}).items())))
        with self._lock:
            self.counters[key] = self.counters.get(key, 0.0) + value
            if help:
                self.help.setdefault(name, help)

    def observe(self, name: str, value: float, help: str = "") -> None:
        with self._lock:
            h = self.histograms.get(name)
            if h is None:
                h = self.histograms[name] = Histogram()
            h.observe(value)
            if help:
                self.help.setdefault(name, help)

    def render(self, gauges: list) -> str:
        """gauges: list of (name, help, [(labels, value), ...])."""
        out = []
        with self._lock:
            by_name: dict = {}
            for (name, labels), v in self.counters.items():
                by_name.setdefault(name, []).append((dict(labels), v))
            for name in sorted(by_name):
                out.append(f"# HELP {name} {self.help.get(name, name)}")
                out.append(f"# TYPE {name} counter")
                for labels, v in sorted(by_name[name], key=lambda x: _label_str(x[0])):
                    out.append(f"{name}{_label_str(labels)} {v:g}")
            for name in sorted(self.histograms):
                h = self.histograms[name]
                out.append(f"# HELP {name} {self.help.get(name, name)}")
                out.append(f"# TYPE {name} histogram")
                for b, c in zip(h.buckets, h.counts):
                    out.append(f'{name}_bucket{{le="{b:g}"}} {c}')
                out.append(f'{name}_bucket{{le="+Inf"}} {h.total}')
                out.append(f"{name}_sum {h.sum:g}")
                out.append(f"{name}_count {h.total}")
        for name, help_text, samples in gauges:
            out.append(f"# HELP {name} {help_text}")
            out.append(f"# TYPE {name} gauge")
            for labels, v in samples:
                out.append(f"{name}{_label_str(labels)} {float(v):g}")
        return "\n".join(out) + "\n"
