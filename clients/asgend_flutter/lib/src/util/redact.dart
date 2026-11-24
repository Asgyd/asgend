/// Redaction of credentials before anything is logged, shown in an error
/// banner or exported.
///
/// Covered: Control Plane user/device tokens (`ucp_…`), node tokens
/// (`ncp_…`), download tickets (`dlt_…`), `Authorization: Bearer …` values and
/// JSON/query fields whose name denotes a secret (`token`, `auth_token`,
/// `api_key`, `code`, `pairing_code`, `password`, `secret`, `ticket`, …).
library;

const String redactedMarker = '[REDACTED]';

final RegExp _prefixedToken = RegExp(r'\b(ucp|ncp|dlt)_[A-Za-z0-9._~+/=-]+');
final RegExp _bearer = RegExp(r'(Bearer\s+)[^\s",;]+', caseSensitive: false);
final RegExp _jsonSecretField = RegExp(
  r'("(?:auth_token|token|node_token|access_token|refresh_token|api_key|apikey|password|secret|code|pairing_code|enrollment_code|ticket)"\s*:\s*")((?:[^"\\]|\\.)*)(")',
  caseSensitive: false,
);
final RegExp _querySecretField = RegExp(
  r'([?&](?:auth_token|token|access_token|api_key|code|ticket)=)[^&#\s]+',
  caseSensitive: false,
);
final RegExp _downloadPath = RegExp(r'(/v1/downloads/)[^/?#\s"]+');

/// Returns [input] with every recognised secret replaced by [redactedMarker].
String redact(String input) {
  if (input.isEmpty) return input;
  var out = input;
  out = out.replaceAllMapped(_jsonSecretField, (m) => '${m[1]}$redactedMarker${m[3]}');
  out = out.replaceAllMapped(_bearer, (m) => '${m[1]}$redactedMarker');
  out = out.replaceAllMapped(_querySecretField, (m) => '${m[1]}$redactedMarker');
  out = out.replaceAllMapped(_downloadPath, (m) => '${m[1]}$redactedMarker');
  out = out.replaceAllMapped(_prefixedToken, (m) => '${m[1]}_$redactedMarker');
  return out;
}

/// Redacts additional caller-known secrets (e.g. the raw core auth token, which
/// has no recognisable prefix) on top of [redact].
String redactWith(String input, Iterable<String?> knownSecrets) {
  var out = input;
  for (final secret in knownSecrets) {
    if (secret == null || secret.length < 4) continue;
    out = out.replaceAll(secret, redactedMarker);
  }
  return redact(out);
}

/// Formats an arbitrary error object for UI/log output without secrets.
String safeError(Object? error, [Iterable<String?> knownSecrets = const []]) =>
    redactWith('$error', knownSecrets);

/// Tiny in-memory diagnostic log. Every line is redacted before it is stored,
/// so even a careless call site cannot persist a token. Nothing is written to
/// stdout/logcat in release builds.
class AppLog {
  AppLog._();

  static const int capacity = 300;
  static final List<String> _lines = <String>[];
  static final List<String> _knownSecrets = <String>[];

  static List<String> get lines => List.unmodifiable(_lines);

  /// Registers a secret without a recognisable prefix (e.g. core token) so it
  /// is removed from future log lines too.
  static void registerSecret(String? secret) {
    if (secret == null || secret.length < 4 || _knownSecrets.contains(secret)) return;
    _knownSecrets.add(secret);
  }

  static void forgetSecrets() => _knownSecrets.clear();

  static void log(String message) {
    final line = '${DateTime.now().toUtc().toIso8601String()} ${redactWith(message, _knownSecrets)}';
    _lines.add(line);
    if (_lines.length > capacity) _lines.removeRange(0, _lines.length - capacity);
  }

  static void clear() => _lines.clear();
}
