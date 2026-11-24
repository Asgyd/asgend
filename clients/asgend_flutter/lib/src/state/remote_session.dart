import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;

import '../api/api_error.dart';
import '../api/backoff.dart';
import '../api/control_plane_client.dart';
import '../api/endpoint_policy.dart';
import '../api/idempotency.dart';
import '../models/remote_models.dart';
import '../storage/settings.dart';
import '../storage/task_cache.dart';
import '../util/redact.dart';

enum RemoteStatus {
  /// No credentials: show the pairing screen.
  unpaired,
  connecting,
  online,

  /// Network/TLS/5xx failure; cached data is shown with a stale banner.
  offline,

  /// 403 for the current workspace/role.
  permissionDenied,
}

/// Application state for Control Plane (remote) mode.
///
/// Offline policy: mutations are **never queued silently**. A failed submit /
/// answer / cancel / retry surfaces an error; the [PendingAction] (and thus the
/// idempotency key) is kept so that the user's explicit retry is safe even if
/// the first request did reach the server.
class RemoteSession extends ChangeNotifier {
  RemoteSession({
    required this.settings,
    required this.cache,
    http.Client Function()? httpClientFactory,
    this.refreshInterval = const Duration(seconds: 15),
    Backoff? backoff,
  })  : _httpClientFactory = httpClientFactory ?? http.Client.new,
        _backoff = backoff ?? Backoff(base: const Duration(seconds: 2), cap: const Duration(seconds: 60));

  final SettingsRepository settings;
  final TaskCache cache;
  final Duration refreshInterval;
  final http.Client Function() _httpClientFactory;
  final Backoff _backoff;

  RemoteStatus status = RemoteStatus.connecting;
  RemoteCredentials? credentials;
  ControlPlaneClient? client;
  MeInfo? me;
  List<RemoteNode> nodes = const [];
  List<RemoteTask> tasks = const [];
  DateTime? tasksFetchedAt;
  bool lastRefreshFailed = false;
  String? lastError;

  /// Message shown on the pairing screen after a forced re-pair (401).
  String? repairReason;
  String stateFilter = '';
  String nodeFilter = '';
  bool loading = false;

  PendingAction? _pendingSubmit;
  final Map<String, PendingAction> _pendingCommands = {};
  Timer? _timer;
  bool _foreground = true;
  bool _disposed = false;

  WorkspaceRole get role {
    final ws = credentials?.workspaceId;
    final fromMe = ws == null ? null : me?.workspace(ws)?.role;
    return fromMe ?? credentials?.role ?? WorkspaceRole.unknown;
  }

  bool get canOperate => role.canOperate;

  bool get showsStaleData =>
      tasksFetchedAt != null &&
      (lastRefreshFailed || status == RemoteStatus.offline || cache.now().difference(tasksFetchedAt!) > TaskCache.defaultMaxAge);

  RemoteNode? node(String id) {
    for (final n in nodes) {
      if (n.nodeId == id) return n;
    }
    return null;
  }

  Future<EndpointPolicy> policy() async => EndpointPolicy(allowInsecureDev: await settings.devAllowInsecureHttp());

  Future<void> init() async {
    final creds = await settings.remoteCredentials();
    if (creds == null) {
      _setStatus(RemoteStatus.unpaired);
      return;
    }
    await _activate(creds);
    final cached = await cache.loadTasks(creds.workspaceId);
    if (cached != null) {
      tasks = cached.tasks;
      tasksFetchedAt = cached.fetchedAt;
    }
    _notify();
    await refresh();
    // Do not replace the one-shot retry timer installed by _onError when the
    // initial refresh is offline; the periodic poll starts after a success.
    if (status == RemoteStatus.online) _schedule();
  }

  Future<void> _activate(RemoteCredentials creds) async {
    credentials = creds;
    client?.close();
    try {
      client = ControlPlaneClient.forEndpoint(creds.endpoint,
          token: creds.token, httpClient: _httpClientFactory(), policy: await policy());
    } on ApiException catch (e) {
      // Stored endpoint no longer passes the policy (e.g. dev flag disabled).
      client = null;
      lastError = e.userMessage;
      _setStatus(RemoteStatus.unpaired);
    }
  }

  /// Manual code + endpoint (or values from a pairing link/QR).
  Future<void> pair({required String endpoint, required String code, required String deviceName}) async {
    final p = await policy();
    final base = p.validate(endpoint); // throws insecure_endpoint
    final anon = ControlPlaneClient(baseUri: base, httpClient: _httpClientFactory(), policy: p);
    try {
      final result = await anon.redeemPairing(code: code, deviceName: deviceName.isEmpty ? 'ASGEND client' : deviceName);
      final creds = RemoteCredentials(
        endpoint: base.toString(),
        token: result.token,
        workspaceId: result.workspaceId,
        userId: result.userId,
        role: result.role,
      );
      await settings.clearRemoteCredentials();
      await settings.saveRemoteCredentials(creds);
      await settings.setDeviceName(deviceName);
      await settings.setMode(ConnectionMode.remote);
      AppLog.log('paired with ${base.host} workspace ${result.workspaceId} role ${result.role.name}');
      repairReason = null;
      tasks = const [];
      tasksFetchedAt = null;
      await _activate(creds);
      await refresh();
      if (status == RemoteStatus.online) _schedule();
    } finally {
      anon.close();
    }
  }

  /// Logout: server-side revoke (best effort) + local credential deletion
  /// (always, even when offline).
  Future<void> logout() async {
    final c = client;
    if (c != null) {
      try {
        await c.logout();
      } on ApiException catch (e) {
        AppLog.log('logout revoke failed: ${e.code.wire}');
      }
    }
    await _forget(null);
  }

  Future<void> _forget(String? reason) async {
    _timer?.cancel();
    client?.close();
    client = null;
    credentials = null;
    me = null;
    nodes = const [];
    tasks = const [];
    tasksFetchedAt = null;
    _pendingSubmit = null;
    _pendingCommands.clear();
    repairReason = reason;
    await settings.clearRemoteCredentials();
    _setStatus(RemoteStatus.unpaired);
  }

  /// Central handling of 401: credentials are invalid or revoked → delete them
  /// and send the user to the pairing flow.
  Future<void> handleUnauthorized() => _forget('The session expired or this device was revoked. Pair again.');

  Future<void> selectWorkspace(WorkspaceMembership ws) async {
    final c = credentials;
    if (c == null) return;
    await settings.setActiveWorkspace(ws.workspaceId, ws.role);
    credentials = RemoteCredentials(endpoint: c.endpoint, token: c.token, workspaceId: ws.workspaceId, userId: c.userId, role: ws.role);
    tasks = const [];
    tasksFetchedAt = null;
    final cached = await cache.loadTasks(ws.workspaceId);
    if (cached != null) {
      tasks = cached.tasks;
      tasksFetchedAt = cached.fetchedAt;
    }
    _notify();
    await refresh();
  }

  void setFilters({String? state, String? nodeId}) {
    if (state != null) stateFilter = state;
    if (nodeId != null) nodeFilter = nodeId;
    _notify();
    unawaited(refresh());
  }

  /// Reads me/nodes/tasks. Errors are mapped to visible states; nothing throws.
  Future<void> refresh() async {
    final c = client;
    final creds = credentials;
    if (c == null || creds == null) return;
    loading = true;
    if (status != RemoteStatus.online) status = RemoteStatus.connecting;
    _notify();
    try {
      me = await c.me();
      final nodeList = await c.listNodes(creds.workspaceId);
      final page = await c.listTasks(creds.workspaceId, state: stateFilter, nodeId: nodeFilter, limit: 100);
      nodes = nodeList;
      tasks = page.tasks;
      tasksFetchedAt = cache.now();
      lastRefreshFailed = false;
      lastError = null;
      if (stateFilter.isEmpty && nodeFilter.isEmpty) await cache.saveTasks(creds.workspaceId, page.tasks);
      _backoff.reset();
      status = RemoteStatus.online;
    } on ApiException catch (e) {
      await _onError(e);
    } finally {
      loading = false;
      _notify();
    }
  }

  Future<void> _onError(ApiException e) async {
    AppLog.log('refresh failed: $e');
    lastRefreshFailed = true;
    lastError = e.userMessage;
    switch (e.code) {
      case ApiErrorCode.unauthorized:
        await handleUnauthorized();
      case ApiErrorCode.forbidden:
        status = RemoteStatus.permissionDenied;
      default:
        status = RemoteStatus.offline;
        _scheduleRetry(e.retryAfter);
    }
  }

  void _scheduleRetry(Duration? retryAfter) {
    if (!_foreground || _disposed) return;
    _timer?.cancel();
    final delay = _backoff.next(retryAfter: retryAfter);
    _timer = Timer(delay, () async {
      await refresh();
      if (status == RemoteStatus.online) _schedule();
    });
  }

  void _schedule() {
    _timer?.cancel();
    if (!_foreground || _disposed || client == null) return;
    _timer = Timer.periodic(refreshInterval, (_) {
      if (status == RemoteStatus.online && !loading) unawaited(refresh());
    });
  }

  /// App lifecycle: stop polling in background, resume immediately on return
  /// (covers Wi-Fi ↔ mobile switches that happen while the app is hidden).
  void onForegroundChanged(bool foreground) {
    _foreground = foreground;
    if (!foreground) {
      _timer?.cancel();
      return;
    }
    _backoff.reset();
    if (client != null) {
      unawaited(refresh().then((_) {
        if (status == RemoteStatus.online) _schedule();
      }));
    }
  }

  // ---------------------------------------------------------------- mutations

  /// Creates a task. Throws [ApiException]; on failure the same idempotency key
  /// is reused when the user retries with identical input.
  Future<RemoteTask> submitTask({required String nodeId, required String text, String? agent, String? sessionId, int priority = 5}) async {
    final c = _requireClient();
    final ws = credentials!.workspaceId;
    final fingerprint = [ws, nodeId, text, agent ?? '', sessionId ?? '', '$priority'].join('\u0000');
    final action = PendingAction.reuseOrCreate(_pendingSubmit, 'submit', fingerprint);
    _pendingSubmit = action;
    try {
      final task = await c.createTask(ws,
          nodeId: nodeId, task: text, agent: agent, sessionId: sessionId, priority: priority, idempotencyKey: action.key);
      _pendingSubmit = null;
      tasks = [task, ...tasks.where((t) => t.taskId != task.taskId)];
      _notify();
      return task;
    } on ApiException catch (e) {
      if (e.code == ApiErrorCode.unauthorized) await handleUnauthorized();
      // 409 = key reused with another body → the next attempt needs a new key.
      if (e.code == ApiErrorCode.conflict || e.code == ApiErrorCode.invalidRequest) _pendingSubmit = null;
      rethrow;
    }
  }

  /// Exposed for tests/UI ("retry will reuse key …").
  String? get pendingSubmitKey => _pendingSubmit?.key;

  Future<CommandEnvelope> answer(String taskId, String text) =>
      _command(taskId, 'answer', text, (c, key) => c.answer(taskId, answer: text, idempotencyKey: key));

  Future<CommandEnvelope> cancel(String taskId) =>
      _command(taskId, 'cancel', '', (c, key) => c.cancel(taskId, idempotencyKey: key));

  Future<CommandEnvelope> retry(String taskId) =>
      _command(taskId, 'retry', '', (c, key) => c.retry(taskId, idempotencyKey: key));

  String? pendingCommandKey(String verb, String taskId) => _pendingCommands['$verb:$taskId']?.key;

  Future<CommandEnvelope> _command(String taskId, String verb, String fingerprint,
      Future<CommandEnvelope> Function(ControlPlaneClient, String) send) async {
    final c = _requireClient();
    final slot = '$verb:$taskId';
    final action = PendingAction.reuseOrCreate(_pendingCommands[slot], verb, fingerprint);
    _pendingCommands[slot] = action;
    try {
      final cmd = await send(c, action.key);
      _pendingCommands.remove(slot);
      return cmd;
    } on ApiException catch (e) {
      if (e.code == ApiErrorCode.unauthorized) await handleUnauthorized();
      if (!e.isTransient) _pendingCommands.remove(slot);
      rethrow;
    }
  }

  ControlPlaneClient _requireClient() {
    final c = client;
    if (c == null || credentials == null) throw ApiException(ApiErrorCode.unauthorized, 'Not paired.');
    return c;
  }

  void _setStatus(RemoteStatus s) {
    status = s;
    _notify();
  }

  void _notify() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    _timer?.cancel();
    client?.close();
    super.dispose();
  }
}
