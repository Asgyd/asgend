import 'dart:async';

import 'package:flutter/foundation.dart';

import '../api/api_error.dart';
import '../api/backoff.dart';
import '../api/control_plane_client.dart';
import '../models/remote_models.dart';
import '../storage/task_cache.dart';
import '../util/redact.dart';

enum FollowStatus { idle, connecting, live, offline, rateLimited, permissionDenied, notFound, unauthorized, paused }

/// Follows one task: long-polls `/v1/tasks/{id}/events?after_event_id=` with
/// the cursor persisted in [TaskCache], so a restart, app resume or network
/// change continues exactly after the last applied event (no gaps, no
/// duplicates — events are also deduplicated by `event_id`).
class EventFollower extends ChangeNotifier {
  EventFollower({
    required this.client,
    required this.cache,
    required this.taskId,
    this.onUnauthorized,
    this.waitMs = 25000,
    this.minPollInterval = const Duration(milliseconds: 500),
    Backoff? backoff,
    Future<void> Function(Duration)? sleeper,
  })  : _backoff = backoff ?? Backoff(),
        _sleeper = sleeper;

  final ControlPlaneClient client;
  final TaskCache cache;
  final String taskId;
  final Future<void> Function()? onUnauthorized;
  final int waitMs;
  final Duration minPollInterval;
  final Backoff _backoff;
  final Future<void> Function(Duration)? _sleeper;

  FollowStatus status = FollowStatus.idle;
  RemoteTask? task;
  List<TaskEvent> events = const [];
  List<RemoteArtifact> artifacts = const [];
  int cursor = 0;
  String? error;
  DateTime? nextRetryAt;
  DateTime? lastSuccessAt;

  int _generation = 0;
  bool _running = false;
  Completer<void>? _wake;
  bool _disposed = false;

  bool get isRunning => _running;

  static const _taskChangingEvents = {
    'state_changed', 'question', 'answer_submitted', 'command_status', 'delivery_failed', 'artifact_available',
  };

  /// Loads cached events/cursor, then starts the loop.
  Future<void> start() async {
    final cached = await cache.loadEvents(taskId);
    events = cached.events;
    cursor = cached.cursor;
    _notify();
    await refreshTask();
    resume();
  }

  Future<void> refreshTask() async {
    try {
      task = await client.getTask(taskId);
      _notify();
    } on ApiException catch (e) {
      await _handleTerminalErrors(e);
    }
  }

  Future<void> refreshArtifacts() async {
    try {
      artifacts = await client.listArtifacts(taskId);
      _notify();
    } on ApiException catch (e) {
      await _handleTerminalErrors(e);
    }
  }

  /// Returns true when the error stops the loop.
  Future<bool> _handleTerminalErrors(ApiException e) async {
    switch (e.code) {
      case ApiErrorCode.unauthorized:
        _stop(FollowStatus.unauthorized, e.userMessage);
        await onUnauthorized?.call();
        return true;
      case ApiErrorCode.forbidden:
        _stop(FollowStatus.permissionDenied, e.userMessage);
        return true;
      case ApiErrorCode.notFound:
      case ApiErrorCode.gone:
        _stop(FollowStatus.notFound, e.userMessage);
        return true;
      default:
        error = e.userMessage;
        _notify();
        return false;
    }
  }

  void _stop(FollowStatus s, String message) {
    _generation++;
    _running = false;
    status = s;
    error = message;
    _wakeUp();
    _notify();
  }

  /// Stop polling (app in background / page closed). In-flight results of the
  /// abandoned loop are discarded.
  void pause() {
    if (!_running) return;
    _generation++;
    _running = false;
    status = FollowStatus.paused;
    _wakeUp();
    _notify();
  }

  /// (Re)start immediately from the persisted cursor, resetting the backoff.
  void resume() {
    if (_disposed) return;
    if (status == FollowStatus.permissionDenied || status == FollowStatus.notFound || status == FollowStatus.unauthorized) return;
    if (_running) {
      _backoff.reset();
      _wakeUp();
      return;
    }
    _running = true;
    _backoff.reset();
    final gen = ++_generation;
    unawaited(_loop(gen));
  }

  /// User pressed "retry now" while offline.
  void retryNow() {
    _backoff.reset();
    if (_running) {
      _wakeUp();
    } else {
      resume();
    }
  }

  Future<void> _loop(int gen) async {
    status = FollowStatus.connecting;
    _notify();
    while (_running && gen == _generation && !_disposed) {
      final started = DateTime.now();
      try {
        final page = await client.events(taskId, afterEventId: cursor, waitMs: waitMs);
        if (gen != _generation || _disposed) return;
        _backoff.reset();
        error = null;
        nextRetryAt = null;
        lastSuccessAt = DateTime.now();
        status = FollowStatus.live;
        final fresh = page.events.where((e) => e.eventId > cursor).toList();
        if (fresh.isNotEmpty) {
          events = await cache.appendEvents(taskId, events, fresh, page.lastEventId);
          cursor = page.lastEventId;
          _notify();
          if (fresh.any((e) => _taskChangingEvents.contains(e.type))) {
            await refreshTask();
            if (fresh.any((e) => e.type == 'artifact_available')) await refreshArtifacts();
          }
        } else if (page.lastEventId > cursor) {
          cursor = page.lastEventId;
          await cache.saveCursor(taskId, cursor);
        }
        _notify();
        final elapsed = DateTime.now().difference(started);
        if (elapsed < minPollInterval) await _sleep(minPollInterval - elapsed);
      } on ApiException catch (e) {
        if (gen != _generation || _disposed) return;
        AppLog.log('events $taskId: $e');
        if (await _handleTerminalErrors(e)) return;
        final delay = _backoff.next(retryAfter: e.code == ApiErrorCode.rateLimited ? e.retryAfter : null);
        status = e.code == ApiErrorCode.rateLimited ? FollowStatus.rateLimited : FollowStatus.offline;
        error = e.userMessage;
        nextRetryAt = DateTime.now().add(delay);
        _notify();
        await _sleep(delay);
        if (gen == _generation && _running) {
          status = FollowStatus.connecting;
          _notify();
        }
      }
    }
  }

  Future<void> _sleep(Duration d) {
    final custom = _sleeper;
    if (custom != null) return custom(d);
    final wake = Completer<void>();
    _wake = wake;
    final timer = Timer(d, () {
      if (!wake.isCompleted) wake.complete();
    });
    return wake.future.whenComplete(timer.cancel);
  }

  void _wakeUp() {
    final w = _wake;
    _wake = null;
    if (w != null && !w.isCompleted) w.complete();
  }

  void _notify() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    _generation++;
    _running = false;
    _wakeUp();
    super.dispose();
  }
}
