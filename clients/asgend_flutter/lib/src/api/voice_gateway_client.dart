import 'dart:convert';

import 'package:http/http.dart' as http;

class VoiceGatewayException implements Exception {
  const VoiceGatewayException(this.message, {this.statusCode});
  final String message;
  final int? statusCode;

  @override
  String toString() => message;
}

/// HTTP client for the LFM3 voice/report gateway.
///
/// The gateway is deliberately separate from CoreClient: user audio and
/// selections are accumulated into a report first. Only [confirm] can enqueue
/// the final report into ASGEND's project queue.
class VoiceGatewayClient {
  VoiceGatewayClient({
    required String baseUrl,
    required String token,
    http.Client? httpClient,
    this.timeout = const Duration(minutes: 5),
  })  : baseUri = Uri.parse(_normalizeBase(baseUrl)),
        _token = token,
        _http = httpClient ?? http.Client(),
        _ownsClient = httpClient == null {
    if (token.trim().isEmpty) throw ArgumentError.value(token, 'token', 'must not be empty');
  }

  final Uri baseUri;
  final String _token;
  final http.Client _http;
  final bool _ownsClient;
  final Duration timeout;

  static String _normalizeBase(String value) {
    final trimmed = value.trim();
    final uri = Uri.tryParse(trimmed);
    if (uri == null || !uri.hasScheme || uri.host.isEmpty ||
        !const {'http', 'https'}.contains(uri.scheme.toLowerCase()) ||
        uri.userInfo.isNotEmpty || uri.hasQuery || uri.hasFragment ||
        (uri.path.isNotEmpty && uri.path != '/')) {
      throw ArgumentError.value(value, 'baseUrl', 'must be an HTTP(S) origin without credentials, path, query or fragment');
    }
    return uri.replace(path: '').toString().replaceFirst(RegExp(r'/$'), '');
  }

  /// Returns a validation message, or null if the endpoint is safe for this client.
  static String? validateEndpoint(String value, {required bool isAndroid}) {
    final uri = Uri.tryParse(value.trim());
    if (uri == null || !uri.hasScheme || uri.host.isEmpty ||
        !const {'http', 'https'}.contains(uri.scheme.toLowerCase()) ||
        uri.userInfo.isNotEmpty || uri.hasQuery || uri.hasFragment ||
        (uri.path.isNotEmpty && uri.path != '/')) {
      return 'Enter a base HTTP(S) URL without credentials, path, query or fragment.';
    }
    final host = uri.host.toLowerCase();
    final scheme = uri.scheme.toLowerCase();
    final loopback = host == 'localhost' || host == '::1' || host == '127.0.0.1' || host == '[::1]';
    if (isAndroid && scheme != 'https') {
      return 'Android requires HTTPS for audio and gateway-token traffic. Use a VPN with TLS or a secured reverse proxy.';
    }
    if (!isAndroid && scheme == 'http' && !loopback) {
      return 'Unencrypted HTTP is allowed only for a loopback gateway on desktop.';
    }
    return null;
  }

  Uri _uri(String path) => baseUri.resolve(path);

  Future<Map<String, dynamic>> _request(
    String method,
    String path, {
    Map<String, dynamic>? body,
    bool authorized = true,
  }) async {
    final headers = <String, String>{'Accept': 'application/json'};
    if (body != null) headers['Content-Type'] = 'application/json; charset=utf-8';
    if (authorized) headers['X-AO-Token'] = _token;
    late http.Response response;
    try {
      final uri = _uri(path);
      final request = http.Request(method, uri)..headers.addAll(headers);
      if (body != null) request.body = jsonEncode(body);
      final streamed = await _http.send(request).timeout(timeout);
      response = await http.Response.fromStream(streamed).timeout(timeout);
    } on Exception catch (error) {
      throw VoiceGatewayException('Voice gateway connection failed: ${error.toString()}');
    }
    Map<String, dynamic> decoded = <String, dynamic>{};
    if (response.body.isNotEmpty) {
      try {
        final value = jsonDecode(response.body);
        if (value is Map<String, dynamic>) decoded = value;
      } on FormatException {
        throw VoiceGatewayException('Voice gateway returned invalid JSON (HTTP ${response.statusCode}).', statusCode: response.statusCode);
      }
    }
    if (response.statusCode < 200 || response.statusCode >= 300 || decoded['ok'] == false) {
      final message = decoded['error'] is String ? decoded['error'] as String : 'HTTP ${response.statusCode}';
      throw VoiceGatewayException(message, statusCode: response.statusCode);
    }
    return decoded;
  }

  Future<String> createSession(String projectState) async {
    final response = await _request('POST', '/v1/sessions', body: {'project_state': projectState});
    final id = response['session_id'];
    if (id is! String || !RegExp(r'^[0-9a-fA-F-]{36}$').hasMatch(id)) {
      throw const VoiceGatewayException('Voice gateway did not return a valid session ID.');
    }
    return id;
  }

  Future<Map<String, dynamic>> sendTurn({
    required String sessionId,
    String text = '',
    List<Map<String, String>> selections = const [],
    List<VoiceAttachment> attachments = const [],
    String projectState = '',
  }) {
    if (!RegExp(r'^[0-9a-fA-F-]{36}$').hasMatch(sessionId)) {
      throw ArgumentError.value(sessionId, 'sessionId', 'must be a UUID');
    }
    final payload = <String, dynamic>{
      'text': text,
      'project_state': projectState,
      'selections': selections,
      'attachments': attachments.map((attachment) => attachment.toJson()).toList(),
    };
    return _request('POST', '/v1/sessions/$sessionId/turn', body: payload);
  }

  Future<Map<String, dynamic>> confirm({required String sessionId, required int revision}) {
    if (!RegExp(r'^[0-9a-fA-F-]{36}$').hasMatch(sessionId)) {
      throw ArgumentError.value(sessionId, 'sessionId', 'must be a UUID');
    }
    if (revision < 1) throw ArgumentError.value(revision, 'revision', 'must be positive');
    return _request('POST', '/v1/sessions/$sessionId/confirm', body: {'revision': revision});
  }

  Future<Map<String, dynamic>> health() => _request('GET', '/health', authorized: false);

  void close() {
    if (_ownsClient) _http.close();
  }
}

class VoiceAttachment {
  const VoiceAttachment({required this.name, required this.mimeType, required this.dataBase64});
  final String name;
  final String mimeType;
  final String dataBase64;

  Map<String, String> toJson() => {
        'name': name,
        'mime_type': mimeType,
        'data_base64': dataBase64,
      };
}
