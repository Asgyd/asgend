import 'dart:async';
import 'dart:convert';
import 'dart:io' show HandshakeException, SocketException, TlsException;
import 'dart:math';

import 'package:http/http.dart' as http;

import '../models/json.dart';
import '../models/remote_models.dart';
import 'api_error.dart';
import 'endpoint_policy.dart';

/// HTTPS client for the ASGEND Control Plane user API (contracts/v1 §6).
///
/// * TLS certificates are verified by the platform; this class never installs
///   a `badCertificateCallback`.
/// * The bearer token is sent only in the `Authorization` header, never in a
///   URL, and is never included in exceptions or logs.
/// * Download tickets are used without the user token (§1: tickets are only
///   valid on `/v1/downloads/{ticket}` and are the sole credential there).
class ControlPlaneClient {
  ControlPlaneClient({
    required this.baseUri,
    String? token,
    http.Client? httpClient,
    this.policy = const EndpointPolicy(),
    this.requestTimeout = const Duration(seconds: 20),
    this.userAgent = 'asgend-flutter/0.10',
  })  : _token = token,
        _http = httpClient ?? http.Client();

  final Uri baseUri;
  final EndpointPolicy policy;
  final Duration requestTimeout;
  final String userAgent;
  final http.Client _http;
  final String? _token;
  final Random _random = Random();

  bool get hasToken => _token != null && _token.isNotEmpty;

  /// Builds a client after validating [endpoint] with [policy].
  factory ControlPlaneClient.forEndpoint(String endpoint,
      {String? token, http.Client? httpClient, EndpointPolicy policy = const EndpointPolicy()}) {
    return ControlPlaneClient(baseUri: policy.validate(endpoint), token: token, httpClient: httpClient, policy: policy);
  }

  ControlPlaneClient withToken(String? token) =>
      ControlPlaneClient(baseUri: baseUri, token: token, httpClient: _http, policy: policy, requestTimeout: requestTimeout, userAgent: userAgent);

  void close() => _http.close();

  // ---------------------------------------------------------------- endpoints

  Future<bool> health() async {
    final j = await _send('GET', '/v1/health', auth: false);
    return asString(j['status']) == 'ok';
  }

  /// `POST /v1/pairing/redeem` — exchanges a single-use code for a `ucp_` token.
  Future<PairingResult> redeemPairing({required String code, required String deviceName}) async {
    final j = await _send('POST', '/v1/pairing/redeem', auth: false, body: {'code': code.trim(), 'device_name': deviceName});
    final result = PairingResult.fromJson(j);
    if (!result.token.startsWith('ucp_') || result.workspaceId.isEmpty) {
      throw ApiException(ApiErrorCode.badResponse, 'Pairing response did not contain a user token/workspace.');
    }
    return result;
  }

  Future<MeInfo> me() async => MeInfo.fromJson(await _send('GET', '/v1/me'));

  /// Revokes the presenting token server-side.
  Future<void> logout() async {
    await _send('POST', '/v1/auth/logout', body: const <String, dynamic>{});
  }

  Future<List<RemoteNode>> listNodes(String workspaceId) async {
    final j = await _send('GET', '/v1/workspaces/${_seg(workspaceId)}/nodes');
    final rows = j['nodes'] is List ? j['nodes'] : j['items'];
    return asList(rows).map(RemoteNode.fromJson).where((n) => n.nodeId.isNotEmpty).toList();
  }

  Future<TaskPage> listTasks(String workspaceId, {String? state, String? nodeId, int limit = 50, String? cursor}) async {
    final query = <String, String>{
      'limit': '${limit.clamp(1, 200)}',
      if (state != null && state.isNotEmpty) 'state': state,
      if (nodeId != null && nodeId.isNotEmpty) 'node_id': nodeId,
      if (cursor != null && cursor.isNotEmpty) 'cursor': cursor,
    };
    return TaskPage.fromJson(await _send('GET', '/v1/workspaces/${_seg(workspaceId)}/tasks', query: query));
  }

  /// `POST /v1/workspaces/{ws}/tasks`. 201 → new, 200 → same task for a
  /// repeated [idempotencyKey]; 409 → the key was used with another body.
  Future<RemoteTask> createTask(
    String workspaceId, {
    required String nodeId,
    required String task,
    required String idempotencyKey,
    String? agent,
    String? sessionId,
    int? priority,
    int? ttlSeconds,
  }) async {
    final body = <String, dynamic>{
      'node_id': nodeId,
      'task': task,
      'idempotency_key': idempotencyKey,
      if (agent != null && agent.isNotEmpty) 'agent': agent,
      if (sessionId != null && sessionId.isNotEmpty) 'session_id': sessionId,
      if (priority != null) 'priority': priority.clamp(0, 9),
      if (ttlSeconds != null) 'ttl_seconds': ttlSeconds.clamp(10, 86400),
    };
    final j = await _send('POST', '/v1/workspaces/${_seg(workspaceId)}/tasks', body: body);
    return RemoteTask.fromJson(j['task'] is Map ? j['task'] : j);
  }

  Future<RemoteTask> getTask(String taskId) async {
    final j = await _send('GET', '/v1/tasks/${_seg(taskId)}');
    return RemoteTask.fromJson(j['task'] is Map ? j['task'] : j);
  }

  /// Long-poll: blocks up to [waitMs] on the server when there is nothing new.
  Future<EventPage> events(String taskId, {required int afterEventId, int limit = 100, int waitMs = 25000}) async {
    final wait = waitMs.clamp(0, 30000);
    final j = await _send(
      'GET',
      '/v1/tasks/${_seg(taskId)}/events',
      query: {'after_event_id': '${max(0, afterEventId)}', 'limit': '${limit.clamp(1, 500)}', 'wait_ms': '$wait'},
      timeout: Duration(milliseconds: wait) + requestTimeout,
    );
    return EventPage.fromJson(j, fallbackCursor: afterEventId);
  }

  Future<CommandEnvelope> answer(String taskId, {required String answer, required String idempotencyKey}) async =>
      _command(taskId, 'answer', {'answer': answer, 'idempotency_key': idempotencyKey});

  Future<CommandEnvelope> cancel(String taskId, {required String idempotencyKey}) async =>
      _command(taskId, 'cancel', {'idempotency_key': idempotencyKey});

  Future<CommandEnvelope> retry(String taskId, {required String idempotencyKey}) async =>
      _command(taskId, 'retry', {'idempotency_key': idempotencyKey});

  Future<List<RemoteArtifact>> listArtifacts(String taskId) async {
    final j = await _send('GET', '/v1/tasks/${_seg(taskId)}/artifacts');
    return asList(j['artifacts']).map(RemoteArtifact.fromJson).where((a) => a.artifactId.isNotEmpty).toList();
  }

  Future<DownloadTicket> downloadTicket(String artifactId) async {
    final j = await _send('POST', '/v1/artifacts/${_seg(artifactId)}/download-ticket', body: const <String, dynamic>{});
    final ticket = DownloadTicket.fromJson(j);
    if (ticket.url.isEmpty) throw ApiException(ApiErrorCode.badResponse, 'Download ticket response has no URL.');
    return ticket;
  }

  /// Opens the byte stream for a ticket. The caller must drain or cancel the
  /// stream. Errors (410 expired ticket etc.) are mapped to [ApiException].
  Future<http.StreamedResponse> openDownload(DownloadTicket ticket, {int? rangeStart}) async {
    final uri = policy.resolveSameOrigin(baseUri, ticket.url);
    if (!uri.path.contains('/v1/downloads/')) {
      throw ApiException(ApiErrorCode.badResponse, 'Unexpected download URL.');
    }
    final request = http.Request('GET', uri)
      ..headers.addAll({
        'Accept': '*/*',
        'User-Agent': userAgent,
        'X-Request-ID': _requestId(),
        if (rangeStart != null && rangeStart > 0) 'Range': 'bytes=$rangeStart-',
      });
    final http.StreamedResponse response;
    try {
      response = await _http.send(request).timeout(requestTimeout);
    } catch (e) {
      throw _transportError(e);
    }
    if (response.statusCode != 200 && response.statusCode != 206) {
      final body = await _readLimited(response.stream);
      throw ApiException.fromResponse(response.statusCode, body, response.headers);
    }
    return response;
  }

  // ---------------------------------------------------------------- internals

  Future<CommandEnvelope> _command(String taskId, String verb, Map<String, dynamic> body) async {
    final j = await _send('POST', '/v1/tasks/${_seg(taskId)}/$verb', body: body);
    return CommandEnvelope.fromJson(j['command'] is Map ? j['command'] : j);
  }

  static String _seg(String id) => Uri.encodeComponent(id);

  String _requestId() {
    const alphabet = 'abcdefghijklmnopqrstuvwxyz0123456789';
    return 'fl-${List.generate(16, (_) => alphabet[_random.nextInt(alphabet.length)]).join()}';
  }

  Uri _uri(String path, Map<String, String>? query) {
    final base = baseUri.path;
    return baseUri.replace(path: '$base$path', queryParameters: (query == null || query.isEmpty) ? null : query);
  }

  Future<Map<String, dynamic>> _send(
    String method,
    String path, {
    bool auth = true,
    Map<String, dynamic>? body,
    Map<String, String>? query,
    Duration? timeout,
  }) async {
    if (auth && !hasToken) {
      throw ApiException(ApiErrorCode.unauthorized, 'Not paired.');
    }
    final request = http.Request(method, _uri(path, query))
      ..headers.addAll({
        'Accept': 'application/json',
        'User-Agent': userAgent,
        'X-Request-ID': _requestId(),
        if (auth) 'Authorization': 'Bearer $_token',
        if (body != null) 'Content-Type': 'application/json; charset=utf-8',
      });
    if (body != null) request.body = jsonEncode(body);

    final http.Response response;
    try {
      final streamed = await _http.send(request).timeout(timeout ?? requestTimeout);
      response = await http.Response.fromStream(streamed).timeout(timeout ?? requestTimeout);
    } catch (e) {
      throw _transportError(e);
    }

    final status = response.statusCode;
    if (status < 200 || status >= 300) {
      throw ApiException.fromResponse(status, response.body, response.headers);
    }
    final version = response.headers['x-asgend-api-version'];
    if (version != null && version.trim() != '1') {
      throw ApiException(ApiErrorCode.badResponse, 'Incompatible Control Plane API version $version (client supports 1).');
    }
    if (response.body.isEmpty) return <String, dynamic>{};
    final dynamic decoded;
    try {
      decoded = jsonDecode(utf8.decode(response.bodyBytes));
    } on FormatException {
      throw ApiException(ApiErrorCode.badResponse, 'Server returned non-JSON response (HTTP $status).', status: status);
    }
    if (decoded is! Map) {
      throw ApiException(ApiErrorCode.badResponse, 'Server returned a non-object JSON response.', status: status);
    }
    final map = asMap(decoded);
    final apiVersion = map['api_version'];
    if (apiVersion != null && apiVersion != 1) {
      throw ApiException(ApiErrorCode.badResponse, 'Incompatible api_version $apiVersion (client supports 1).');
    }
    return map;
  }

  static ApiException _transportError(Object e) {
    if (e is ApiException) return e;
    if (e is HandshakeException || e is TlsException) {
      return ApiException(ApiErrorCode.network, 'TLS handshake/certificate verification failed.');
    }
    if (e is TimeoutException) return ApiException(ApiErrorCode.network, 'Request timed out.');
    if (e is SocketException) return ApiException(ApiErrorCode.network, 'Network unreachable.');
    if (e is http.ClientException) return ApiException(ApiErrorCode.network, 'Connection failed.');
    return ApiException(ApiErrorCode.network, 'Connection failed (${e.runtimeType}).');
  }

  static Future<String> _readLimited(Stream<List<int>> stream, {int limit = 64 * 1024}) async {
    final bytes = <int>[];
    await for (final chunk in stream) {
      final room = limit - bytes.length;
      if (room <= 0) break;
      bytes.addAll(chunk.length > room ? chunk.sublist(0, room) : chunk);
    }
    return utf8.decode(bytes, allowMalformed: true);
  }
}
