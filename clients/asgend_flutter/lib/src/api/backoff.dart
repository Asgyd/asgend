import 'dart:math';

/// Exponential backoff with "full jitter": delay = random(0, min(cap, base·2^n)),
/// never below [minDelay]. A server `Retry-After` overrides the computed value.
class Backoff {
  Backoff({
    this.base = const Duration(seconds: 1),
    this.cap = const Duration(seconds: 60),
    this.minDelay = const Duration(milliseconds: 250),
    Random? random,
  }) : _random = random ?? Random();

  final Duration base;
  final Duration cap;
  final Duration minDelay;
  final Random _random;
  int _attempt = 0;

  int get attempt => _attempt;

  Duration next({Duration? retryAfter}) {
    final n = _attempt;
    _attempt = n < 30 ? n + 1 : n;
    if (retryAfter != null) return retryAfter < minDelay ? minDelay : retryAfter;
    final exp = base.inMilliseconds * pow(2, n);
    final ceiling = min(cap.inMilliseconds.toDouble(), exp.toDouble());
    final ms = (_random.nextDouble() * ceiling).round();
    return Duration(milliseconds: max(ms, minDelay.inMilliseconds));
  }

  void reset() => _attempt = 0;
}
