import 'dart:convert';

import '../models/json.dart';
import '../models/remote_models.dart';
import 'stores.dart';

class CachedTasks {
  const CachedTasks({required this.tasks, required this.fetchedAt});
  final List<RemoteTask> tasks;
  final DateTime fetchedAt;

  Duration age(DateTime now) => now.difference(fetchedAt);

  /// Data is stale when it is older than [maxAge]. The UI additionally treats
  /// cached data as stale whenever the last refresh failed.
  bool isStale(DateTime now, {Duration maxAge = TaskCache.defaultMaxAge}) => age(now) > maxAge;
}

class CachedEvents {
  const CachedEvents({required this.events, required this.cursor});
  final List<TaskEvent> events;
  final int cursor;
}

/// Last known remote state with timestamps (ROADMAP A1.1 p.7). Stored in
/// app-private preferences under `cache.*`; removed on logout/re-pair.
/// Contains task titles/events but never credentials.
class TaskCache {
  TaskCache(this.store, {this.maxEventsPerTask = 300, DateTime Function()? clock}) : _clock = clock ?? DateTime.now;

  static const defaultMaxAge = Duration(minutes: 2);

  final KeyValueStore store;
  final int maxEventsPerTask;
  final DateTime Function() _clock;

  DateTime now() => _clock();

  String _tasksKey(String ws) => 'cache.tasks.$ws';
  String _eventsKey(String task) => 'cache.events.$task';
  String _cursorKey(String task) => 'cache.cursor.$task';

  Future<void> saveTasks(String workspaceId, List<RemoteTask> tasks) async {
    final payload = {
      'fetched_at': _clock().toUtc().toIso8601String(),
      'tasks': tasks.map((t) => t.toJson()).toList(),
    };
    await store.setString(_tasksKey(workspaceId), jsonEncode(payload));
  }

  Future<CachedTasks?> loadTasks(String workspaceId) async {
    final raw = await store.getString(_tasksKey(workspaceId));
    if (raw == null) return null;
    try {
      final j = asMap(jsonDecode(raw));
      final fetched = asDate(j['fetched_at']);
      if (fetched == null) return null;
      return CachedTasks(tasks: asList(j['tasks']).map(RemoteTask.fromJson).toList(), fetchedAt: fetched);
    } on FormatException {
      await store.remove(_tasksKey(workspaceId));
      return null;
    }
  }

  /// Per-task `after_event_id` cursor. Never decreases.
  Future<int> cursor(String taskId) async => int.tryParse(await store.getString(_cursorKey(taskId)) ?? '') ?? 0;

  Future<void> saveCursor(String taskId, int cursor) async {
    final current = await this.cursor(taskId);
    if (cursor > current) await store.setString(_cursorKey(taskId), '$cursor');
  }

  Future<CachedEvents> loadEvents(String taskId) async {
    final c = await cursor(taskId);
    final raw = await store.getString(_eventsKey(taskId));
    if (raw == null) return CachedEvents(events: const [], cursor: c);
    try {
      final events = asList(jsonDecode(raw)).map(TaskEvent.fromJson).where((e) => e.eventId > 0).toList();
      return CachedEvents(events: events, cursor: c);
    } on FormatException {
      return CachedEvents(events: const [], cursor: c);
    }
  }

  /// Appends [events] (deduplicated by event_id) and advances the cursor.
  Future<List<TaskEvent>> appendEvents(String taskId, List<TaskEvent> existing, List<TaskEvent> incoming, int cursor) async {
    final seen = existing.map((e) => e.eventId).toSet();
    final merged = [...existing, ...incoming.where((e) => seen.add(e.eventId))]..sort((a, b) => a.eventId.compareTo(b.eventId));
    final bounded = merged.length > maxEventsPerTask ? merged.sublist(merged.length - maxEventsPerTask) : merged;
    await store.setString(_eventsKey(taskId), jsonEncode(bounded.map((e) => e.toJson()).toList()));
    await saveCursor(taskId, cursor);
    return bounded;
  }
}
