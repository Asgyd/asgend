// Control Plane v1 entities (contracts/v1/CONTRACT_SKETCH.md §5). Field names
// are normative; unknown fields are ignored; missing optional fields get safe
// defaults so a newer server never crashes an older client.
import 'json.dart';

enum WorkspaceRole {
  owner,
  operator,
  viewer,
  unknown;

  static WorkspaceRole parse(dynamic value) => switch (value) {
        'owner' => WorkspaceRole.owner,
        'operator' => WorkspaceRole.operator,
        'viewer' => WorkspaceRole.viewer,
        _ => WorkspaceRole.unknown,
      };

  /// submit/answer/cancel/retry (contract §4).
  bool get canOperate => this == WorkspaceRole.owner || this == WorkspaceRole.operator;
}

class TaskStates {
  TaskStates._();
  static const all = <String>[
    'pending_delivery', 'delivered', 'queued', 'running', 'waiting_input', 'completed',
    'completed_without_model', 'failed', 'cancelled', 'delivery_failed',
  ];
  static const terminal = <String>{'completed', 'completed_without_model', 'failed', 'cancelled', 'delivery_failed'};
  static const retryable = <String>{'failed', 'delivery_failed'};

  static String label(String state) => switch (state) {
        'pending_delivery' => 'Waiting for node',
        'delivered' => 'Delivered to node',
        'queued' => 'Queued',
        'running' => 'Running',
        'waiting_input' => 'Waiting for your answer',
        'completed' => 'Completed',
        'completed_without_model' => 'Completed (fallback)',
        'failed' => 'Failed',
        'cancelled' => 'Cancelled',
        'delivery_failed' => 'Delivery failed',
        _ => state,
      };
}

class QuestionOption {
  const QuestionOption({required this.id, required this.label});
  final String id;
  final String label;
}

/// A question that blocks a task. Accepts both the Control Plane shape
/// `{"text","options":["…"]}` and the local core shape
/// `{"question","options":[{"id","label"}|"…"]}`.
class QuestionPrompt {
  const QuestionPrompt({required this.text, this.options = const []});
  final String text;
  final List<QuestionOption> options;

  static QuestionPrompt? fromJson(dynamic raw) {
    final json = asMap(raw);
    if (json.isEmpty) return null;
    final text = asString(json['text'], asString(json['question']));
    if (text.isEmpty) return null;
    final options = <QuestionOption>[];
    for (final value in asList(json['options'])) {
      if (value is String) {
        options.add(QuestionOption(id: value, label: value));
      } else if (value is Map) {
        final m = asMap(value);
        final id = asString(m['id'], asString(m['label']));
        if (id.isEmpty) continue;
        options.add(QuestionOption(id: id, label: asString(m['label'], id)));
      }
    }
    return QuestionPrompt(text: text, options: options);
  }
}

class RemoteTask {
  const RemoteTask({
    required this.taskId,
    required this.workspaceId,
    required this.nodeId,
    required this.title,
    required this.state,
    this.coreTaskId,
    this.agent,
    this.sessionId,
    this.priority = 5,
    this.question,
    this.attempts = 0,
    this.createdAt,
    this.updatedAt,
    this.lastEventId = 0,
    this.stale = false,
    this.idempotencyKey = '',
    this.resultSummary,
    this.error,
    this.raw = const {},
  });

  final String taskId;
  final String workspaceId;
  final String nodeId;
  final String? coreTaskId;
  final String title;
  final String? agent;
  final String? sessionId;
  final int priority;
  final String state;
  final QuestionPrompt? question;
  final int attempts;
  final DateTime? createdAt;
  final DateTime? updatedAt;
  final int lastEventId;
  final bool stale;
  final String idempotencyKey;
  final String? resultSummary;
  final String? error;

  /// Original JSON, kept so the cache round-trips fields this client does not
  /// know yet.
  final Map<String, dynamic> raw;

  bool get isTerminal => TaskStates.terminal.contains(state);
  bool get canRetry => TaskStates.retryable.contains(state);
  bool get waitingInput => state == 'waiting_input';

  factory RemoteTask.fromJson(dynamic value) {
    final j = asMap(value);
    return RemoteTask(
      taskId: asString(j['task_id']),
      workspaceId: asString(j['workspace_id']),
      nodeId: asString(j['node_id']),
      coreTaskId: asStringOrNull(j['core_task_id']),
      title: asString(j['title'], asString(j['task_id'], 'Task')),
      agent: asStringOrNull(j['agent']),
      sessionId: asStringOrNull(j['session_id']),
      priority: asInt(j['priority'], 5),
      state: asString(j['state'], 'unknown'),
      question: QuestionPrompt.fromJson(j['question']),
      attempts: asInt(j['attempts']),
      createdAt: asDate(j['created_at']),
      updatedAt: asDate(j['updated_at']),
      lastEventId: asInt(j['last_event_id']),
      stale: asBool(j['stale']),
      idempotencyKey: asString(j['idempotency_key']),
      resultSummary: asStringOrNull(j['result_summary']),
      error: asStringOrNull(j['error']),
      raw: j,
    );
  }

  Map<String, dynamic> toJson() => raw.isNotEmpty
      ? raw
      : {
          'task_id': taskId, 'workspace_id': workspaceId, 'node_id': nodeId, 'title': title,
          'state': state, 'priority': priority, 'attempts': attempts, 'last_event_id': lastEventId,
          'stale': stale,
        };
}

class TaskEvent {
  const TaskEvent({
    required this.eventId,
    required this.taskId,
    required this.type,
    this.nodeId,
    this.seq,
    this.occurredAt,
    this.receivedAt,
    this.schemaVersion = 1,
    this.payload = const {},
    this.raw = const {},
  });

  final int eventId;
  final String taskId;
  final String? nodeId;
  final int? seq;
  final String type;
  final DateTime? occurredAt;
  final DateTime? receivedAt;
  final int schemaVersion;
  final Map<String, dynamic> payload;
  final Map<String, dynamic> raw;

  factory TaskEvent.fromJson(dynamic value) {
    final j = asMap(value);
    return TaskEvent(
      eventId: asInt(j['event_id']),
      taskId: asString(j['task_id']),
      nodeId: asStringOrNull(j['node_id']),
      seq: asIntOrNull(j['seq']),
      type: asString(j['type'], 'event'),
      occurredAt: asDate(j['occurred_at']),
      receivedAt: asDate(j['received_at']),
      schemaVersion: asInt(j['schema_version'], 1),
      payload: asMap(j['payload']),
      raw: j,
    );
  }

  Map<String, dynamic> toJson() => raw.isNotEmpty
      ? raw
      : {'event_id': eventId, 'task_id': taskId, 'type': type, 'payload': payload};

  /// Short human readable description for the timeline.
  String get summary {
    final p = payload;
    final candidates = [p['message'], p['text'], p['content'], p['state'], p['status'], p['answer'], p['error']];
    for (final c in candidates) {
      if (c is String && c.isNotEmpty) return c;
    }
    return '';
  }
}

class EventPage {
  const EventPage({required this.events, required this.lastEventId});
  final List<TaskEvent> events;
  final int lastEventId;

  factory EventPage.fromJson(dynamic value, {int fallbackCursor = 0}) {
    final j = asMap(value);
    final events = asList(j['events']).map(TaskEvent.fromJson).where((e) => e.eventId > 0).toList()
      ..sort((a, b) => a.eventId.compareTo(b.eventId));
    var last = asInt(j['last_event_id'], fallbackCursor);
    if (events.isNotEmpty && events.last.eventId > last) last = events.last.eventId;
    // The cursor never moves backwards, even if a buggy server says so.
    if (last < fallbackCursor) last = fallbackCursor;
    return EventPage(events: events, lastEventId: last);
  }
}

class TaskPage {
  const TaskPage({required this.tasks, this.nextCursor});
  final List<RemoteTask> tasks;
  final String? nextCursor;

  factory TaskPage.fromJson(dynamic value) {
    final j = asMap(value);
    return TaskPage(
      tasks: asList(j['tasks']).map(RemoteTask.fromJson).where((t) => t.taskId.isNotEmpty).toList(),
      nextCursor: asStringOrNull(j['next_cursor']),
    );
  }
}

class RemoteNode {
  const RemoteNode({
    required this.nodeId,
    required this.name,
    required this.status,
    this.workspaceId = '',
    this.labels = const [],
    this.protocolVersion = 1,
    this.coreVersion,
    this.lastSeenAt,
    this.leaseExpiresAt,
  });

  final String nodeId;
  final String workspaceId;
  final String name;
  final List<String> labels;
  final int protocolVersion;
  final String? coreVersion;
  final String status;
  final DateTime? lastSeenAt;
  final DateTime? leaseExpiresAt;

  bool get online => status == 'online';

  factory RemoteNode.fromJson(dynamic value) {
    final j = asMap(value);
    final id = asString(j['node_id']);
    return RemoteNode(
      nodeId: id,
      workspaceId: asString(j['workspace_id']),
      name: asString(j['name'], id),
      labels: asStringList(j['labels']),
      protocolVersion: asInt(j['protocol_version'], 1),
      coreVersion: asStringOrNull(j['core_version']),
      status: asString(j['status'], 'offline'),
      lastSeenAt: asDate(j['last_seen_at']),
      leaseExpiresAt: asDate(j['lease_expires_at']),
    );
  }
}

class RemoteArtifact {
  const RemoteArtifact({
    required this.artifactId,
    required this.taskId,
    required this.name,
    required this.size,
    required this.sha256,
    required this.state,
    this.mime = 'application/octet-stream',
    this.createdAt,
    this.expiresAt,
  });

  final String artifactId;
  final String taskId;
  final String name;
  final String mime;
  final int size;
  final String sha256;
  final String state;
  final DateTime? createdAt;
  final DateTime? expiresAt;

  bool get available => state == 'available';

  factory RemoteArtifact.fromJson(dynamic value) {
    final j = asMap(value);
    return RemoteArtifact(
      artifactId: asString(j['artifact_id']),
      taskId: asString(j['task_id']),
      name: asString(j['name'], 'artifact'),
      mime: asString(j['mime'], 'application/octet-stream'),
      size: asInt(j['size']),
      sha256: asString(j['sha256']).toLowerCase(),
      state: asString(j['state'], 'unknown'),
      createdAt: asDate(j['created_at']),
      expiresAt: asDate(j['expires_at']),
    );
  }
}

class CommandEnvelope {
  const CommandEnvelope({required this.commandId, required this.type, required this.state, this.taskId = '', this.error});
  final String commandId;
  final String taskId;
  final String type;
  final String state;
  final String? error;

  factory CommandEnvelope.fromJson(dynamic value) {
    final j = asMap(value);
    return CommandEnvelope(
      commandId: asString(j['command_id']),
      taskId: asString(j['task_id']),
      type: asString(j['type']),
      state: asString(j['state'], 'pending'),
      error: asStringOrNull(j['error']),
    );
  }
}

class WorkspaceMembership {
  const WorkspaceMembership({required this.workspaceId, required this.role, this.name = ''});
  final String workspaceId;
  final String name;
  final WorkspaceRole role;

  factory WorkspaceMembership.fromJson(dynamic value) {
    final j = asMap(value);
    final id = asString(j['workspace_id'], asString(j['id']));
    return WorkspaceMembership(workspaceId: id, name: asString(j['name'], id), role: WorkspaceRole.parse(j['role']));
  }
}

/// `/v1/me`. The contract only says "user, workspaces + roles"; both a nested
/// `user` object and top-level `user_id` are accepted.
class MeInfo {
  const MeInfo({required this.userId, required this.workspaces, this.displayName = ''});
  final String userId;
  final String displayName;
  final List<WorkspaceMembership> workspaces;

  WorkspaceMembership? workspace(String id) {
    for (final w in workspaces) {
      if (w.workspaceId == id) return w;
    }
    return null;
  }

  factory MeInfo.fromJson(dynamic value) {
    final j = asMap(value);
    final user = asMap(j['user']);
    final userId = asString(user['user_id'], asString(j['user_id']));
    return MeInfo(
      userId: userId,
      displayName: asString(user['name'], asString(user['display_name'], asString(j['name'], userId))),
      workspaces: asList(j['workspaces'])
          .map(WorkspaceMembership.fromJson)
          .where((w) => w.workspaceId.isNotEmpty)
          .toList(),
    );
  }
}

/// `/v1/pairing/redeem` response. [token] is a secret: never log it, never put
/// it in toString().
class PairingResult {
  const PairingResult({required this.token, required this.userId, required this.workspaceId, required this.role});
  final String token;
  final String userId;
  final String workspaceId;
  final WorkspaceRole role;

  factory PairingResult.fromJson(dynamic value) {
    final j = asMap(value);
    return PairingResult(
      token: asString(j['token']),
      userId: asString(j['user_id']),
      workspaceId: asString(j['workspace_id']),
      role: WorkspaceRole.parse(j['role']),
    );
  }

  @override
  String toString() => 'PairingResult(user: $userId, workspace: $workspaceId, role: ${role.name}, token: [REDACTED])';
}

class DownloadTicket {
  const DownloadTicket({required this.url, this.expiresAt});
  final String url;
  final DateTime? expiresAt;

  factory DownloadTicket.fromJson(dynamic value) {
    final j = asMap(value);
    final ticket = asString(j['ticket']);
    return DownloadTicket(
      url: asString(j['url'], ticket.isEmpty ? '' : '/v1/downloads/$ticket'),
      expiresAt: asDate(j['expires_at']),
    );
  }

  @override
  String toString() => 'DownloadTicket([REDACTED], expires: $expiresAt)';
}
