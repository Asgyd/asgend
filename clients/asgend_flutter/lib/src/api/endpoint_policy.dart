import 'api_error.dart';

/// Validation of a Control Plane endpoint entered by the user or taken from a
/// pairing link.
///
/// Rules (docs/ROADMAP.md A1.2, contracts/v1 §1):
/// * `https://` is required;
/// * `http://` is accepted only for a loopback host (127.0.0.0/8, ::1,
///   `localhost`) **or** when the explicit developer flag is on;
/// * no user-info, query or fragment (tokens must never travel in URLs);
/// * there is no default endpoint — in particular never `127.0.0.1` on a phone.
class EndpointPolicy {
  const EndpointPolicy({this.allowInsecureDev = false});

  /// Developer-only escape hatch (Settings → Developer). Even with this flag the
  /// Android release manifest denies cleartext traffic for platform HTTP
  /// stacks; Dart's HttpClient is additionally guarded by this check.
  final bool allowInsecureDev;

  static bool isLoopbackHost(String host) {
    final h = host.toLowerCase();
    if (h == 'localhost' || h == '::1' || h == '[::1]') return true;
    final parts = h.split('.');
    if (parts.length != 4 || parts[0] != '127') return false;
    final octets = parts.map((part) => int.tryParse(part)).toList();
    return octets.every((value) => value != null && value >= 0 && value <= 255);
  }

  /// Returns the normalised base URI (`scheme://host[:port][/base]` without a
  /// trailing slash) or throws [ApiException] with code `insecure_endpoint`.
  Uri validate(String raw) {
    final text = raw.trim();
    if (text.isEmpty) {
      throw ApiException(ApiErrorCode.insecureEndpoint, 'Enter the Control Plane address (https://…).');
    }
    final uri = Uri.tryParse(text);
    if (uri == null || !uri.hasScheme || uri.host.isEmpty) {
      throw ApiException(ApiErrorCode.insecureEndpoint, 'Invalid address. Use https://host[:port].');
    }
    if (uri.userInfo.isNotEmpty) {
      throw ApiException(ApiErrorCode.insecureEndpoint, 'Credentials in the URL are not allowed.');
    }
    if (uri.hasQuery || uri.hasFragment) {
      throw ApiException(ApiErrorCode.insecureEndpoint, 'The address must not contain ? or # parts.');
    }
    final scheme = uri.scheme.toLowerCase();
    if (scheme == 'http') {
      if (!isLoopbackHost(uri.host) && !allowInsecureDev) {
        throw ApiException(ApiErrorCode.insecureEndpoint,
            'Cleartext http:// is refused. Use https:// (http is allowed only for loopback or with the developer flag).');
      }
    } else if (scheme != 'https') {
      throw ApiException(ApiErrorCode.insecureEndpoint, 'Unsupported scheme "$scheme". Use https://.');
    }
    var path = uri.path;
    while (path.endsWith('/')) {
      path = path.substring(0, path.length - 1);
    }
    if (path.endsWith('/v1')) path = path.substring(0, path.length - 3);
    return Uri(scheme: scheme, host: uri.host, port: uri.hasPort ? uri.port : null, path: path);
  }

  /// Resolves a server-supplied relative URL (e.g. download ticket URL) against
  /// [base] and refuses anything that would leave the Control Plane origin.
  Uri resolveSameOrigin(Uri base, String reference) {
    final resolved = base.resolve(reference.startsWith('/') && base.path.isNotEmpty ? '${base.path}$reference' : reference);
    if (resolved.scheme != base.scheme || resolved.host != base.host || resolved.port != base.port) {
      throw ApiException(ApiErrorCode.insecureEndpoint, 'Server returned a URL outside the Control Plane origin.');
    }
    return resolved;
  }
}

/// Parsed pairing link / QR payload:
/// `asgend-pair://pair?endpoint=https%3A%2F%2Fcp.example&code=ABCD-1234`.
/// The code is single-use and short-lived (contract §6), it is not persisted.
class PairingLink {
  const PairingLink({required this.endpoint, required this.code});
  final String endpoint;
  final String code;

  static PairingLink? tryParse(String raw) {
    final uri = Uri.tryParse(raw.trim());
    if (uri == null || uri.scheme.toLowerCase() != 'asgend-pair') return null;
    final endpoint = uri.queryParameters['endpoint'] ?? '';
    final code = uri.queryParameters['code'] ?? '';
    if (endpoint.isEmpty || code.isEmpty) return null;
    return PairingLink(endpoint: endpoint, code: code);
  }

  @override
  String toString() => 'PairingLink($endpoint, code: [REDACTED])';
}
