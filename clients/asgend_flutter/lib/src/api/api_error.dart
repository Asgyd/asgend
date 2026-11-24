import 'dart:convert';

import '../models/json.dart';
import '../util/redact.dart';

/// Error codes from contracts/v1 §2 plus client-side transport conditions.
enum ApiErrorCode {
  invalidRequest('invalid_request'),
  unauthorized('unauthorized'),
  forbidden('forbidden'),
  notFound('not_found'),
  conflict('conflict'),
  gone('gone'),
  payloadTooLarge('payload_too_large'),
  checksumMismatch('checksum_mismatch'),
  unprocessable('unprocessable'),
  rateLimited('rate_limited'),
  quotaExceeded('quota_exceeded'),
  internal('internal'),
  unavailable('unavailable'),

  /// Client side: DNS/socket/TLS failure or timeout — the server was not reached
  /// or the answer was lost. The action may or may not have been applied, so
  /// retries must reuse the same idempotency key.
  network('network'),

  /// Client side: server answered with something that is not v1 JSON.
  badResponse('bad_response'),

  /// Client side: endpoint policy refused the URL (e.g. cleartext http).
  insecureEndpoint('insecure_endpoint'),
  unknown('unknown');

  const ApiErrorCode(this.wire);
  final String wire;

  static ApiErrorCode fromWire(String code, int status) {
    for (final c in ApiErrorCode.values) {
      if (c.wire == code) return c;
    }
    return switch (status) {
      400 => ApiErrorCode.invalidRequest,
      401 => ApiErrorCode.unauthorized,
      403 => ApiErrorCode.forbidden,
      404 => ApiErrorCode.notFound,
      409 => ApiErrorCode.conflict,
      410 => ApiErrorCode.gone,
      413 => ApiErrorCode.payloadTooLarge,
      422 => ApiErrorCode.unprocessable,
      429 => ApiErrorCode.rateLimited,
      503 => ApiErrorCode.unavailable,
      >= 500 => ApiErrorCode.internal,
      _ => ApiErrorCode.unknown,
    };
  }
}

class ApiException implements Exception {
  ApiException(this.code, String message, {this.status, this.requestId, this.retryAfter, this.details = const {}})
      : message = redact(message);

  final ApiErrorCode code;
  final String message;
  final int? status;
  final String? requestId;
  final Duration? retryAfter;
  final Map<String, dynamic> details;

  /// Credentials are invalid/revoked: the UI must drop them and re-pair.
  bool get requiresRepair => code == ApiErrorCode.unauthorized;

  /// Safe to retry automatically (with backoff) for idempotent reads.
  bool get isTransient =>
      code == ApiErrorCode.network ||
      code == ApiErrorCode.unavailable ||
      code == ApiErrorCode.internal ||
      code == ApiErrorCode.rateLimited;

  /// Text for the user. Never contains secrets.
  String get userMessage => switch (code) {
        ApiErrorCode.unauthorized => 'Session expired or revoked. Pair this device again.',
        ApiErrorCode.forbidden => 'Permission denied for your role in this workspace.',
        ApiErrorCode.notFound => 'Not found (it may belong to another workspace or was deleted).',
        ApiErrorCode.gone => 'The link or command has expired. Request it again.',
        ApiErrorCode.rateLimited || ApiErrorCode.quotaExceeded => retryAfter == null
            ? 'Too many requests. Try again later.'
            : 'Too many requests. Try again in ${retryAfter!.inSeconds} s.',
        ApiErrorCode.network => 'Offline or server unreachable. Nothing was queued; retry when online.',
        ApiErrorCode.insecureEndpoint => message,
        ApiErrorCode.checksumMismatch => 'Checksum mismatch: the file was discarded.',
        ApiErrorCode.conflict => message.isEmpty ? 'Conflict with the current task state.' : message,
        _ => message.isEmpty ? code.wire : message,
      };

  @override
  String toString() => 'ApiException(${code.wire}${status == null ? '' : ' $status'}: $message'
      '${requestId == null ? '' : ', request $requestId'})';

  /// Parses a v1 error body (`{"error":{"code","message","request_id","details"}}`).
  static ApiException fromResponse(int status, String body, Map<String, String> headers) {
    var code = '';
    var message = 'HTTP $status';
    String? requestId = headers['x-request-id'];
    var details = <String, dynamic>{};
    try {
      final decoded = asMap(jsonDecode(body));
      final err = asMap(decoded['error']);
      code = asString(err['code']);
      message = asString(err['message'], message);
      requestId = asStringOrNull(err['request_id']) ?? requestId;
      details = asMap(err['details']);
    } on FormatException {
      // Non-JSON error (proxy page etc.) — keep the HTTP status mapping.
    }
    return ApiException(
      ApiErrorCode.fromWire(code, status),
      message,
      status: status,
      requestId: requestId,
      retryAfter: parseRetryAfter(headers['retry-after']),
      details: details,
    );
  }
}

/// `Retry-After` is either delta-seconds or an HTTP date. Clamped to 1 h.
Duration? parseRetryAfter(String? value, {DateTime? now}) {
  if (value == null || value.trim().isEmpty) return null;
  final seconds = int.tryParse(value.trim());
  Duration? d;
  if (seconds != null) {
    d = Duration(seconds: seconds < 0 ? 0 : seconds);
  } else {
    try {
      final date = HttpDateParser.parse(value.trim());
      d = date.difference((now ?? DateTime.now()).toUtc());
      if (d.isNegative) d = Duration.zero;
    } on FormatException {
      return null;
    }
  }
  const max = Duration(hours: 1);
  return d > max ? max : d;
}

/// Minimal RFC 7231 IMF-fixdate parser (avoids dart:io in this file so it
/// stays usable from pure tests).
class HttpDateParser {
  HttpDateParser._();
  static const _months = {
    'Jan': 1, 'Feb': 2, 'Mar': 3, 'Apr': 4, 'May': 5, 'Jun': 6,
    'Jul': 7, 'Aug': 8, 'Sep': 9, 'Oct': 10, 'Nov': 11, 'Dec': 12,
  };

  static DateTime parse(String value) {
    final m = RegExp(r'^\w{3}, (\d{2}) (\w{3}) (\d{4}) (\d{2}):(\d{2}):(\d{2}) GMT$').firstMatch(value);
    if (m == null) throw FormatException('Bad HTTP date', value);
    final month = _months[m[2]];
    if (month == null) throw FormatException('Bad HTTP date month', value);
    return DateTime.utc(int.parse(m[3]!), month, int.parse(m[1]!), int.parse(m[4]!), int.parse(m[5]!), int.parse(m[6]!));
  }
}
