import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'src/api/endpoint_policy.dart';
import 'src/storage/settings.dart';
import 'src/storage/stores.dart';
import 'src/ui/remote/remote_home.dart';
import 'src/util/redact.dart';

void main() => runApp(const AsgendApp());

class AsgendClient {
  const AsgendClient({required this.host, required this.port, required this.token});

  final String host;
  final int port;
  final String token;

  Future<Map<String, dynamic>> rpc(Map<String, dynamic> payload,
      {Duration timeout = const Duration(seconds: 60)}) async {
    final socket = await Socket.connect(host, port, timeout: const Duration(seconds: 5));
    try {
      final request = <String, dynamic>{...payload};
      if (token.isNotEmpty) request['auth_token'] = token;
      socket.write('${jsonEncode(request)}\n');
      await socket.flush();
      final line = await utf8.decoder
          .bind(socket)
          .transform(const LineSplitter())
          .first
          .timeout(timeout);
      final decoded = jsonDecode(line);
      if (decoded is! Map<String, dynamic>) {
        throw const FormatException('Core returned a non-object JSON response.');
      }
      if (decoded['ok'] != true) {
        throw StateError(redactWith((decoded['error'] ?? 'Request failed').toString(), [token]));
      }
      return decoded;
    } finally {
      socket.destroy();
    }
  }
}

class AsgendApp extends StatefulWidget {
  const AsgendApp({super.key});

  @override
  State<AsgendApp> createState() => _AsgendAppState();
}

class _AsgendAppState extends State<AsgendApp> {
  String _host = '127.0.0.1';
  int _port = 8765;
  String _token = '';
  bool _loaded = false;
  bool _connected = false;
  String? _connectionError;
  final SecretStore _secretStore = PlatformSecretStore();

  AsgendClient get client => AsgendClient(host: _host, port: _port, token: _token);

  @override
  void initState() {
    super.initState();
    _loadSettings();
  }

  Future<void> _loadSettings() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      // Migrate legacy tokens out of SharedPreferences (which is not a secret
      // store). Only delete the old value after the secure write succeeds.
      final secureSettings = SettingsRepository(
        prefs: SharedPrefsStore(),
        secrets: _secretStore,
      );
      final token = await secureSettings.migrateLegacyCoreToken(
        legacyToken: prefs.getString('core_token'),
        removeLegacyPreference: () => prefs.remove('core_token'),
      );
      if (!mounted) return;
      setState(() {
        _host = prefs.getString('core_host') ?? _host;
        _port = prefs.getInt('core_port') ?? _port;
        _token = token;
        _loaded = true;
      });
      await _checkConnection();
    } catch (_) {
      // Never fall back to cleartext credential persistence when the platform
      // secure store is unavailable. The user can retry after fixing the keyring.
      if (!mounted) return;
      setState(() {
        _loaded = true;
        _connected = false;
        _connectionError = 'Secure credential storage is unavailable. Reopen the app after the device key store is available.';
      });
    }
  }

  Future<void> _checkConnection() async {
    try {
      await client.rpc({'op': 'ping'}, timeout: const Duration(seconds: 5));
      if (mounted) setState(() { _connected = true; _connectionError = null; });
    } catch (error) {
      if (mounted) setState(() { _connected = false; _connectionError = safeError(error, [_token]); });
    }
  }

  Future<void> _editConnection() async {
    final hostCtl = TextEditingController(text: _host);
    final portCtl = TextEditingController(text: '$_port');
    final tokenCtl = TextEditingController(text: _token);
    var acknowledgedHost = '';
    final result = await showDialog<Map<String, dynamic>>(
      context: context,
      builder: (dialogContext) => StatefulBuilder(
        builder: (dialogContext, setDialogState) {
          final host = hostCtl.text.trim();
          final needsTunnelAcknowledgement = host.isNotEmpty && !EndpointPolicy.isLoopbackHost(host);
          final acknowledgementCurrent = acknowledgedHost == host.toLowerCase();
          final port = int.tryParse(portCtl.text.trim());
          final validPort = port != null && port >= 1 && port <= 65535;
          final canSave = host.isNotEmpty && validPort &&
              (!needsTunnelAcknowledgement || acknowledgementCurrent);
          return AlertDialog(
            title: const Text('Connection to ASGEND core'),
            content: SizedBox(
              width: 460,
              child: SingleChildScrollView(
                child: Column(mainAxisSize: MainAxisSize.min, children: [
                  TextField(
                    controller: hostCtl,
                    onChanged: (_) => setDialogState(() {}),
                    decoration: const InputDecoration(labelText: 'Host / IP address'),
                  ),
                  TextField(
                    controller: portCtl,
                    keyboardType: TextInputType.number,
                    onChanged: (_) => setDialogState(() {}),
                    decoration: const InputDecoration(labelText: 'TCP port'),
                  ),
                  TextField(
                    controller: tokenCtl,
                    obscureText: true,
                    decoration: const InputDecoration(labelText: 'Core auth token'),
                  ),
                  const SizedBox(height: 8),
                  const Text(
                    'The core protocol is unencrypted TCP. Never expose this port to the public Internet; use loopback, a VPN or a protected SSH tunnel.',
                    style: TextStyle(fontSize: 12),
                  ),
                  if (needsTunnelAcknowledgement)
                    CheckboxListTile(
                      contentPadding: EdgeInsets.zero,
                      value: acknowledgementCurrent,
                      onChanged: (value) => setDialogState(() {
                        acknowledgedHost = value == true ? host.toLowerCase() : '';
                      }),
                      controlAffinity: ListTileControlAffinity.leading,
                      title: const Text('I confirm this remote endpoint is reachable only through a trusted VPN/tunnel.'),
                    ),
                ]),
              ),
            ),
            actions: [
              TextButton(onPressed: () => Navigator.pop(dialogContext), child: const Text('Cancel')),
              FilledButton(
                onPressed: canSave ? () {
                  Navigator.pop(dialogContext, {'host': host, 'port': port, 'token': tokenCtl.text});
                } : null,
                child: const Text('Save and connect'),
              ),
            ],
          );
        },
      ),
    );
    hostCtl.dispose();
    portCtl.dispose();
    tokenCtl.clear();
    tokenCtl.dispose();
    if (result == null) return;
    final prefs = await SharedPreferences.getInstance();
    final newToken = result['token'] as String;
    try {
      if (newToken.isEmpty) {
        await _secretStore.delete('core_token');
      } else {
        await _secretStore.write('core_token', newToken);
      }
      await prefs.remove('core_token'); // cleanup for older app versions
      await prefs.setString('core_host', result['host'] as String);
      await prefs.setInt('core_port', result['port'] as int);
    } catch (_) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('Could not save the credential securely. Check the platform key store and retry.'),
        ));
      }
      return;
    }
    if (!mounted) return;
    setState(() { _host = result['host'] as String; _port = result['port'] as int; _token = newToken; });
    await _checkConnection();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ASGEND',
      theme: ThemeData(colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF5267A7)), useMaterial3: true),
      home: !_loaded
          ? const Scaffold(body: Center(child: CircularProgressIndicator()))
          : DefaultTabController(
              length: 4,
              child: Builder(builder: (context) => Scaffold(
                appBar: AppBar(
                  title: const Row(mainAxisSize: MainAxisSize.min, children: [Icon(Icons.hub_outlined), SizedBox(width: 10), Text('ASGEND')]),
                  actions: [
                    Padding(padding: const EdgeInsets.symmetric(horizontal: 8), child: Center(child: Tooltip(
                      message: _connectionError ?? 'Connected to $_host:$_port',
                      child: Icon(_connected ? Icons.cloud_done_outlined : Icons.cloud_off_outlined,
                          color: _connected ? Colors.green : Theme.of(context).colorScheme.error),
                    ))),
                    IconButton(tooltip: 'Control Plane (HTTPS)', onPressed: () { Navigator.of(context).push(MaterialPageRoute(builder: (_) => const RemoteHomePage())); }, icon: const Icon(Icons.cloud_sync_outlined)),
                    IconButton(tooltip: 'Connection settings', onPressed: _editConnection, icon: const Icon(Icons.settings_ethernet)),
                    IconButton(tooltip: 'Check connection', onPressed: _checkConnection, icon: const Icon(Icons.refresh)),
                  ],
                  bottom: const TabBar(tabs: [
                    Tab(icon: Icon(Icons.forum_outlined), text: 'Tasks'),
                    Tab(icon: Icon(Icons.hub_outlined), text: 'Nodes'),
                    Tab(icon: Icon(Icons.groups_outlined), text: 'Agents'),
                    Tab(icon: Icon(Icons.library_books_outlined), text: 'Resources'),
                  ]),
                ),
                body: TabBarView(children: [
                  TaskDashboard(client: client, onConnectionError: (e) { setState(() { _connected = false; _connectionError = e; }); }),
                  NodesPage(client: client),
                  AgentsPage(client: client),
                  ResourcesPage(client: client),
                ]),
                bottomNavigationBar: _connected ? null : Material(
                  color: Theme.of(context).colorScheme.errorContainer,
                  child: Padding(padding: const EdgeInsets.all(8), child: Row(children: [
                    const Icon(Icons.warning_amber_rounded), const SizedBox(width: 8),
                    Expanded(child: Text(_connectionError ?? 'Not connected to the core', maxLines: 2, overflow: TextOverflow.ellipsis)),
                    TextButton(onPressed: _editConnection, child: const Text('Configure')),
                  ])),
                ),
              ))),
    );
  }
}

String _asString(dynamic value, [String fallback = '']) => value is String ? value : fallback;
Map<String, dynamic> _map(dynamic value) => value is Map<String, dynamic> ? value : <String, dynamic>{};
List<dynamic> _list(dynamic value) => value is List<dynamic> ? value : <dynamic>[];
String _pretty(String value) => value.replaceAll('_', ' ');
String _statusLabel(String status) => switch (status) {
  'queued' => 'Queued', 'running' => 'Running', 'waiting_input' => 'Waiting for your answer',
  'completed' => 'Completed', 'completed_without_model' => 'Completed (fallback)',
  'failed' => 'Failed', 'cancelled' => 'Cancelled', _ => status,
};

class TaskDashboard extends StatefulWidget {
  const TaskDashboard({super.key, required this.client, required this.onConnectionError});
  final AsgendClient client;
  final ValueChanged<String> onConnectionError;
  @override
  State<TaskDashboard> createState() => _TaskDashboardState();
}

class _TaskDashboardState extends State<TaskDashboard> {
  final _taskCtl = TextEditingController();
  int _priority = 5;
  bool _busy = false;
  String? _error;
  List<dynamic> _tasks = [];
  Timer? _timer;
  bool _refreshing = false;

  @override
  void initState() {
    super.initState();
    _refresh();
    _timer = Timer.periodic(const Duration(seconds: 3), (_) => _refresh());
  }

  @override
  void dispose() { _timer?.cancel(); _taskCtl.dispose(); super.dispose(); }

  Future<void> _refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      final result = await widget.client.rpc({'op': 'list_tasks', 'limit': 100}, timeout: const Duration(seconds: 8));
      if (mounted) setState(() { _tasks = _list(result['tasks']); _error = null; });
    } catch (error) {
      if (mounted) setState(() => _error = '$error');
      widget.onConnectionError('$error');
    } finally {
      _refreshing = false;
    }
  }

  Future<void> _submit() async {
    final text = _taskCtl.text.trim();
    if (text.isEmpty || _busy) return;
    setState(() { _busy = true; _error = null; });
    try {
      final response = await widget.client.rpc({'op': 'submit', 'task': text, 'priority': _priority}, timeout: const Duration(seconds: 8));
      _taskCtl.clear();
      await _refresh();
      if (!mounted) return;
      await Navigator.of(context).push(MaterialPageRoute(builder: (_) => TaskChatPage(client: widget.client, taskId: _asString(response['task_id']))));
      await _refresh();
    } catch (error) {
      if (mounted) setState(() => _error = '$error');
    } finally { if (mounted) setState(() => _busy = false); }
  }

  void _openTask(dynamic row) {
    final map = _map(row);
    final id = _asString(map['task_id']);
    if (id.isEmpty) return;
    Navigator.of(context).push(MaterialPageRoute(builder: (_) => TaskChatPage(client: widget.client, taskId: id)));
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, constraints) => ListView(padding: const EdgeInsets.all(16), children: [
      Card(child: Padding(padding: const EdgeInsets.all(16), child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text('New task', style: Theme.of(context).textTheme.titleLarge),
        const SizedBox(height: 10),
        TextField(controller: _taskCtl, minLines: 2, maxLines: 5, enabled: !_busy,
          decoration: const InputDecoration(border: OutlineInputBorder(), hintText: 'Describe the result you need…')),
        Row(children: [const Text('Priority'), Expanded(child: Slider(value: _priority.toDouble(), min: 0, max: 9, divisions: 9, label: '$_priority', onChanged: (value) => setState(() => _priority = value.round()))), Text('$_priority')]),
        Align(alignment: Alignment.centerRight, child: FilledButton.icon(onPressed: _busy ? null : _submit,
          icon: _busy ? const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2)) : const Icon(Icons.play_arrow),
          label: const Text('Start task and open chat'))),
        if (_error != null) Padding(padding: const EdgeInsets.only(top: 8), child: Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error))),
      ]))),
      const SizedBox(height: 8),
      Row(children: [Expanded(child: Text('Tasks', style: Theme.of(context).textTheme.titleLarge)), IconButton(onPressed: _refresh, icon: const Icon(Icons.refresh))]),
      if (_tasks.isEmpty) const Padding(padding: EdgeInsets.symmetric(vertical: 32), child: Center(child: Text('No tasks yet. Submit a task above to open its live chat.'))),
      for (final row in _tasks)
        Builder(builder: (context) {
          final task = _map(row);
          final id = _asString(task['task_id']);
          final status = _asString(task['status'], 'unknown');
          return Card(child: ListTile(
            leading: CircleAvatar(child: Icon(status == 'waiting_input' ? Icons.help_outline : status == 'failed' ? Icons.error_outline : status == 'completed' || status == 'completed_without_model' ? Icons.check : Icons.pending_actions)),
            title: Text(_asString(task['task'], _asString(task['task_id'], 'Task')), maxLines: 2, overflow: TextOverflow.ellipsis),
            subtitle: Text('${_statusLabel(status)} · priority ${task['priority'] ?? 5} · ${id.isNotEmpty ? id.substring(0, id.length > 8 ? 8 : id.length) : ''}'),
            trailing: const Icon(Icons.chevron_right), onTap: () => _openTask(task),
          ));
        }),
    ]));
  }
}

class TaskChatPage extends StatefulWidget {
  const TaskChatPage({super.key, required this.client, required this.taskId});
  final AsgendClient client;
  final String taskId;
  @override
  State<TaskChatPage> createState() => _TaskChatPageState();
}

class _TaskChatPageState extends State<TaskChatPage> {
  final _events = <dynamic>[];
  Timer? _timer;
  int _lastEvent = 0;
  String _status = 'queued';
  Map<String, dynamic> _statusReply = {};
  String? _error;
  int? _activeQuestionEvent;
  bool _loading = true;
  bool _refreshing = false;
  final _followupCtl = TextEditingController();
  bool _sendingFollowup = false;
  final _scrollCtl = ScrollController();

  @override
  void initState() {
    super.initState();
    _refresh();
    _timer = Timer.periodic(const Duration(seconds: 2), (_) => _refresh());
  }

  @override
  void dispose() { _timer?.cancel(); _followupCtl.dispose(); _scrollCtl.dispose(); super.dispose(); }

  Future<void> _refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      final status = await widget.client.rpc({'op': 'status', 'task_id': widget.taskId}, timeout: const Duration(seconds: 8));
      final eventReply = await widget.client.rpc({'op': 'events', 'task_id': widget.taskId, 'after_id': _lastEvent, 'limit': 100}, timeout: const Duration(seconds: 8));
      final newEvents = _list(eventReply['events']);
      if (!mounted) return;
      setState(() {
        _statusReply = status;
        _status = _asString(status['status'], 'unknown');
        _loading = false;
        _error = null;
        for (final value in newEvents) {
          final event = _map(value);
          _events.add(event);
          final eventId = (event['event_id'] as num?)?.toInt() ?? 0;
          if (eventId > _lastEvent) _lastEvent = eventId;
        }
      });
      for (final value in newEvents) {
        final event = _map(value);
        if (event['type'] == 'question' || event['type'] == 'agent_question') {
          final eventId = (event['event_id'] as num?)?.toInt() ?? 0;
          await _maybeAsk(eventId, _questionFromEvent(event));
        }
      }
      if (_status == 'waiting_input') {
        final result = _map(status['result']);
        final question = _map(result['question']);
        if (question.isNotEmpty) await _maybeAsk(_lastEvent, question);
      }
      if (newEvents.isNotEmpty && _scrollCtl.hasClients) {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (_scrollCtl.hasClients) _scrollCtl.animateTo(_scrollCtl.position.maxScrollExtent, duration: const Duration(milliseconds: 180), curve: Curves.easeOut);
        });
      }
    } catch (error) {
      if (mounted) setState(() { _loading = false; _error = '$error'; });
    } finally {
      _refreshing = false;
    }
  }

  Map<String, dynamic> _questionFromEvent(Map<String, dynamic> event) {
    final payload = _map(event['payload']);
    if (payload['question'] is Map<String, dynamic>) return _map(payload['question']);
    return payload;
  }

  Future<void> _maybeAsk(int eventId, Map<String, dynamic> question) async {
    if (!mounted || question.isEmpty || _activeQuestionEvent == eventId) return;
    final text = _asString(question['question']);
    if (text.isEmpty) return;
    _activeQuestionEvent = eventId;
    final answer = await showDialog<Map<String, String>>(
      context: context, barrierDismissible: false,
      builder: (dialogContext) => _QuestionDialog(question: question),
    );
    if (answer == null || !mounted) return;
    try {
      await widget.client.rpc({'op': 'answer', 'task_id': widget.taskId,
        'answer': answer['answer'] ?? '', 'option_id': answer['option_id'] ?? ''});
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Answer sent. Task resumed.')));
      await _refresh();
    } catch (error) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Could not send answer: $error')));
      _activeQuestionEvent = null;
    }
  }

  Future<void> _cancel() async {
    try { await widget.client.rpc({'op': 'cancel', 'task_id': widget.taskId}); await _refresh(); }
    catch (error) { if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('$error'))); }
  }

  Future<void> _followup() async {
    final text = _followupCtl.text.trim();
    if (text.isEmpty || _sendingFollowup) return;
    setState(() => _sendingFollowup = true);
    try {
      final response = await widget.client.rpc({'op': 'submit', 'task': text, 'session_id': 'task-${widget.taskId}'});
      _followupCtl.clear();
      if (!mounted) return;
      await Navigator.of(context).push(MaterialPageRoute(builder: (_) => TaskChatPage(client: widget.client, taskId: _asString(response['task_id']))));
    } catch (error) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('$error')));
    } finally { if (mounted) setState(() => _sendingFollowup = false); }
  }

  @override
  Widget build(BuildContext context) {
    final result = _map(_statusReply['result']);
    final statusError = _asString(_statusReply['error']);
    final isTerminal = const {'completed', 'completed_without_model', 'failed', 'cancelled'}.contains(_status);
    return Scaffold(
      appBar: AppBar(title: const Text('Task chat'), actions: [
        Padding(padding: const EdgeInsets.symmetric(horizontal: 10), child: Center(child: Chip(label: Text(_statusLabel(_status))))),
        if (!isTerminal) IconButton(tooltip: 'Cancel task', onPressed: _cancel, icon: const Icon(Icons.stop_circle_outlined)),
        IconButton(tooltip: 'Refresh', onPressed: _refresh, icon: const Icon(Icons.refresh)),
      ]),
      body: Column(children: [
        Material(color: Theme.of(context).colorScheme.surfaceContainerLow, child: Padding(padding: const EdgeInsets.all(12), child: Row(children: [
          const Icon(Icons.tag), const SizedBox(width: 8), Expanded(child: SelectableText(widget.taskId)),
          Text(_statusLabel(_status), style: Theme.of(context).textTheme.labelLarge),
        ]))),
        if (_error != null) Material(color: Theme.of(context).colorScheme.errorContainer, child: Padding(padding: const EdgeInsets.all(8), child: Text(_error!))),
        if (statusError.isNotEmpty) Padding(padding: const EdgeInsets.all(8), child: Text(statusError)),
        Expanded(child: _loading && _events.isEmpty
          ? const Center(child: CircularProgressIndicator())
          : _events.isEmpty ? const Center(child: Text('Waiting for core events…')) : ListView.builder(
              controller: _scrollCtl, padding: const EdgeInsets.all(12), itemCount: _events.length,
              itemBuilder: (context, index) => _EventCard(event: _map(_events[index])))),
        if (result.isNotEmpty && isTerminal) ExpansionTile(title: const Text('Final result'), children: [
          Padding(padding: const EdgeInsets.all(12), child: SelectableText(const JsonEncoder.withIndent('  ').convert(result))),
        ]),
        if (isTerminal) SafeArea(child: Padding(padding: const EdgeInsets.fromLTRB(12, 4, 12, 12), child: Row(children: [
          Expanded(child: TextField(controller: _followupCtl, minLines: 1, maxLines: 3, decoration: const InputDecoration(labelText: 'Continue this session', border: OutlineInputBorder()))),
          const SizedBox(width: 8), IconButton.filled(onPressed: _sendingFollowup ? null : _followup, icon: const Icon(Icons.send)),
        ]))),
      ]),
    );
  }
}

class _QuestionDialog extends StatefulWidget {
  const _QuestionDialog({required this.question});
  final Map<String, dynamic> question;
  @override
  State<_QuestionDialog> createState() => _QuestionDialogState();
}

class _QuestionDialogState extends State<_QuestionDialog> {
  String? _selectedId;
  String? _selectedLabel;
  final _customCtl = TextEditingController();
  @override
  void dispose() { _customCtl.dispose(); super.dispose(); }
  @override
  Widget build(BuildContext context) {
    final options = _list(widget.question['options']);
    return AlertDialog(
      title: const Row(children: [Icon(Icons.help_outline), SizedBox(width: 8), Expanded(child: Text('The agent needs your input'))]),
      content: SizedBox(width: 480, child: SingleChildScrollView(child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
        Text(_asString(widget.question['question'], 'Please clarify how to proceed.')),
        const SizedBox(height: 12),
        for (final value in options)
          if (value is Map<String, dynamic>) RadioListTile<String>(
            value: _asString(value['id']), groupValue: _selectedId,
            title: Text(_asString(value['label'], _asString(value['id']))), dense: true,
            onChanged: (id) => setState(() { _selectedId = id; _selectedLabel = _asString(value['label'], id ?? ''); _customCtl.clear(); }),
          ) else ListTile(title: Text('$value'), onTap: () => setState(() { _selectedId = '$value'; _selectedLabel = '$value'; _customCtl.clear(); })),
        TextField(controller: _customCtl, minLines: 1, maxLines: 3, decoration: const InputDecoration(labelText: 'Or enter your own answer')),
      ]))),
      actions: [FilledButton(onPressed: () {
        final custom = _customCtl.text.trim();
        final answer = custom.isNotEmpty ? custom : (_selectedLabel ?? '');
        if (answer.isEmpty) return;
        Navigator.pop(context, {'answer': answer, 'option_id': custom.isNotEmpty ? '' : (_selectedId ?? '')});
      }, child: const Text('Answer and continue'))],
    );
  }
}

class _EventCard extends StatelessWidget {
  const _EventCard({required this.event});
  final Map<String, dynamic> event;
  @override
  Widget build(BuildContext context) {
    final actor = _asString(event['actor'], 'system');
    final type = _pretty(_asString(event['type'], 'event'));
    final message = _asString(event['message']);
    final stamp = _asString(event['created_at']);
    final payload = _map(event['payload']);
    final text = _asString(payload['content']);
    final tool = _asString(payload['tool']);
    return Align(alignment: Alignment.centerLeft, child: ConstrainedBox(constraints: const BoxConstraints(maxWidth: 940), child: Card(
      margin: const EdgeInsets.only(bottom: 10), child: Padding(padding: const EdgeInsets.all(12), child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [Expanded(child: Text(actor, style: Theme.of(context).textTheme.titleSmall)), Chip(label: Text(type, style: const TextStyle(fontSize: 11))),]),
        if (message.isNotEmpty) Text(message),
        if (tool.isNotEmpty) Padding(padding: const EdgeInsets.only(top: 4), child: Text('Tool: $tool', style: Theme.of(context).textTheme.labelMedium)),
        if (text.isNotEmpty) Padding(padding: const EdgeInsets.only(top: 8), child: SelectableText(text)),
        if (stamp.isNotEmpty) Padding(padding: const EdgeInsets.only(top: 6), child: Text(stamp, style: Theme.of(context).textTheme.bodySmall)),
      ])),
    )));
  }
}

class ResourcesPage extends StatefulWidget {
  const ResourcesPage({super.key, required this.client});
  final AsgendClient client;
  @override
  State<ResourcesPage> createState() => _ResourcesPageState();
}

class _ResourcesPageState extends State<ResourcesPage> {
  Map<String, dynamic> _catalog = {};
  bool _loading = true;
  String? _error;

  @override
  void initState() { super.initState(); _refresh(); }

  Future<void> _refresh() async {
    try {
      final result = await widget.client.rpc({'op': 'resources'}, timeout: const Duration(seconds: 10));
      if (mounted) setState(() { _catalog = result; _loading = false; _error = null; });
    } catch (error) {
      if (mounted) setState(() { _loading = false; _error = '$error'; });
    }
  }

  @override
  Widget build(BuildContext context) {
    final files = _map(_catalog['files']);
    final dirs = _map(_catalog['directories']);
    const categories = <String, String>{
      'skills': 'Skills', 'system_prompts': 'System prompts',
      'tools': 'Tools and MCP descriptions', 'knowledge': 'Knowledge',
      'instructions': 'Decision instructions',
    };
    return _loading ? const Center(child: CircularProgressIndicator()) : RefreshIndicator(
      onRefresh: _refresh,
      child: ListView(padding: const EdgeInsets.all(16), children: [
        Row(children: [Expanded(child: Text('Live resource catalogue', style: Theme.of(context).textTheme.headlineSmall)), IconButton(onPressed: _refresh, icon: const Icon(Icons.refresh))]),
        const Text('Skills, system prompts, tools and knowledge are synced into a separate local BM25 index automatically. Project files use a separate project-memory index.'),
        if (_error != null) Padding(padding: const EdgeInsets.all(8), child: Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error))),
        Card(child: ListTile(leading: const Icon(Icons.hub_outlined), title: const Text('Resource retrieval'), subtitle: Text(_catalog['selection_mode'] == 'local_bm25' ? 'Local BM25 · automatic index sync' : _asString(_catalog['decision_model_id'], '(not configured)')), trailing: Icon((_catalog['resource_index_auto_sync'] == true) ? Icons.sync : Icons.warning_amber))),
        Card(child: ListTile(leading: const Icon(Icons.folder_open_outlined), title: const Text('Project memory'), subtitle: Text('${_asString(_catalog['project_memory_root'], '(not configured)')}\n${_asString(_catalog['project_memory_index_path'], 'index not created yet')}'), isThreeLine: true)),
        for (final entry in categories.entries) Card(child: ExpansionTile(
          leading: Icon(switch (entry.key) { 'skills' => Icons.psychology_outlined, 'system_prompts' => Icons.description_outlined, 'tools' => Icons.build_outlined, 'knowledge' => Icons.menu_book_outlined, _ => Icons.description_outlined }),
          title: Text(entry.value),
          subtitle: Text('${_list(files[entry.key]).length} files · ${_asString(dirs[entry.key], 'directory not reported')}'),
          children: [
            for (final file in _list(files[entry.key])) ListTile(dense: true, leading: const Icon(Icons.insert_drive_file_outlined), title: Text('$file', softWrap: true)),
            if (_list(files[entry.key]).isEmpty) const ListTile(title: Text('No Markdown files discovered.')),
          ],
        )),
      ]),
    );
  }
}

class NodesPage extends StatefulWidget {
  const NodesPage({super.key, required this.client});
  final AsgendClient client;
  @override
  State<NodesPage> createState() => _NodesPageState();
}

class _NodesPageState extends State<NodesPage> {
  Map<String, dynamic> _configs = {};
  Map<String, dynamic> _modelStatus = {};
  Map<String, dynamic> _resources = {};
  bool _loading = true;
  bool _saving = false;
  String? _error;

  @override
  void initState() { super.initState(); _refresh(); }

  Future<void> _refresh() async {
    try {
      final results = await Future.wait([
        widget.client.rpc({'op': 'config_get'}),
        widget.client.rpc({'op': 'models'}),
      ]);
      if (!mounted) return;
      setState(() { _configs = results[0]; _modelStatus = _map(results[1]['model_status']); _resources = _map(results[1]['resources']); _loading = false; _error = null; });
    } catch (error) { if (mounted) setState(() { _loading = false; _error = '$error'; }); }
  }

  Future<void> _saveConfigs(Map<String, dynamic> configs) async {
    setState(() { _saving = true; _error = null; });
    try {
      final result = await widget.client.rpc({'op': 'config_save', 'main': configs['main'], 'models': configs['models'], 'agents': configs['agents']});
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(_asString(result['message'], 'Configuration saved; restart may be required.'))));
        await _refresh();
      }
    } catch (error) { if (mounted) setState(() => _error = '$error'); }
    finally { if (mounted) setState(() => _saving = false); }
  }

  Future<void> _changeModel(Map<String, dynamic> model, bool decision, String operation) async {
    final id = _asString(model['id']);
    try {
      await widget.client.rpc({'op': operation, 'model_id': id, 'kind': decision ? 'decision' : 'llm'});
      await _refresh();
    } catch (error) { if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('$error'))); }
  }

  Future<void> _toggleNode(Map<String, dynamic> node, bool enabled) async {
    final configs = jsonDecode(jsonEncode(_configs)) as Map<String, dynamic>;
    final modelsRoot = _map(configs['models']);
    final nodeList = _list(modelsRoot['nodes']);
    final nodeId = _asString(node['id']);
    if (!enabled) {
      for (final raw in [..._list(modelsRoot['models']).map((e) => {'row': e, 'decision': false}),
                        ..._list(modelsRoot['decision_models']).map((e) => {'row': e, 'decision': true})]) {
        final entry = _map(raw); final model = _map(entry['row']);
        if (_asString(model['node_id']) == nodeId) {
          model['enabled'] = false;
          await _changeModel(model, entry['decision'] == true, 'model_disable');
        }
      }
    }
    final target = nodeList.map(_map).firstWhere((row) => _asString(row['id']) == nodeId, orElse: () => <String, dynamic>{});
    if (target.isEmpty) return;
    target['enabled'] = enabled;
    await _saveConfigs(configs);
  }

  Future<void> _addNode() async {
    final result = await showDialog<Map<String, dynamic>>(context: context, builder: (_) => const _NodeEditor());
    if (result == null) return;
    final configs = jsonDecode(jsonEncode(_configs)) as Map<String, dynamic>;
    final modelsRoot = _map(configs['models']);
    final nodes = _list(modelsRoot['nodes']);
    if (nodes.any((raw) => _asString(_map(raw)['id']) == result['id'])) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('A node with this ID already exists.')));
      return;
    }
    nodes.add(result);
    modelsRoot['nodes'] = nodes;
    await _saveConfigs(configs);
  }

  Future<void> _addModel(bool decision) async {
    final nodes = _list(_map(_configs['models'])['nodes']).map(_map).toList();
    final result = await showDialog<Map<String, dynamic>>(context: context,
        builder: (_) => _ModelEditor(nodes: nodes, decision: decision));
    if (result == null) return;
    final configs = jsonDecode(jsonEncode(_configs)) as Map<String, dynamic>;
    final modelsRoot = _map(configs['models']);
    final key = decision ? 'decision_models' : 'models';
    final list = _list(modelsRoot[key]);
    if (_list(modelsRoot['models']).any((row) => _asString(_map(row)['id']) == result['id']) ||
        _list(modelsRoot['decision_models']).any((row) => _asString(_map(row)['id']) == result['id'])) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Model ID already exists.')));
      return;
    }
    list.add(result); modelsRoot[key] = list;
    await _saveConfigs(configs);
  }

  @override
  Widget build(BuildContext context) {
    final regular = _list(_map(_configs['models'])['models']).map(_map).toList();
    final decisions = _list(_map(_configs['models'])['decision_models']).map(_map).toList();
    final nodes = _list(_map(_configs['models'])['nodes']).map(_map).toList();
    final runtimeModels = _list(_modelStatus['models']).map(_map).toList();
    final runtimeDecisions = _list(_modelStatus['decision_models']).map(_map).toList();
    Map<String, dynamic> withRuntime(Map<String, dynamic> model, bool decision) {
      final statuses = decision ? runtimeDecisions : runtimeModels;
      final id = _asString(model['id']);
      Map<String, dynamic>? runtime;
      for (final row in statuses) {
        if (_asString(row['id']) == id) { runtime = row; break; }
      }
      return {...model, 'running': runtime?['running'] == true};
    }
    return _loading ? const Center(child: CircularProgressIndicator()) : RefreshIndicator(onRefresh: _refresh, child: ListView(padding: const EdgeInsets.all(16), children: [
      Row(children: [Expanded(child: Text('Nodes and models', style: Theme.of(context).textTheme.headlineSmall)), IconButton(onPressed: _refresh, icon: const Icon(Icons.refresh))]),
      if (_error != null) Padding(padding: const EdgeInsets.all(8), child: Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error))),
      Card(child: Padding(padding: const EdgeInsets.all(14), child: Wrap(spacing: 20, runSpacing: 10, children: [
        _ResourceMetric(label: 'Free RAM', value: _resourceValue(_resources['ram_available_mb'])),
        _ResourceMetric(label: 'Free VRAM', value: _resourceValue(_resources['vram_free_mb'])),
        _ResourceMetric(label: 'Running models', value: '${_list(_resources['running_models']).length}'),
      ]))),
      Row(children: [Expanded(child: Text('Nodes', style: Theme.of(context).textTheme.titleLarge)), FilledButton.tonalIcon(onPressed: _saving ? null : _addNode, icon: const Icon(Icons.add), label: const Text('Add node'))]),
      for (final node in nodes) Card(child: SwitchListTile(
        title: Text(_asString(node['name'], _asString(node['id']))),
        subtitle: Text('${_asString(node['type'])} · ${_asString(node['endpoint'], 'local runtime')}'),
        value: node['enabled'] == true, onChanged: _saving ? null : (value) => _toggleNode(node, value),
      )),
      const SizedBox(height: 12),
      Row(children: [Expanded(child: Text('LLM models', style: Theme.of(context).textTheme.titleLarge)), FilledButton.tonalIcon(onPressed: _saving ? null : () => _addModel(false), icon: const Icon(Icons.add), label: const Text('Add LLM'))]),
      for (final model in regular) _ModelCard(model: withRuntime(model, false), decision: false, onChange: _changeModel),
      Row(children: [Expanded(child: Text('Decision models', style: Theme.of(context).textTheme.titleLarge)), FilledButton.tonalIcon(onPressed: _saving ? null : () => _addModel(true), icon: const Icon(Icons.add), label: const Text('Add decision'))]),
      for (final model in decisions) _ModelCard(model: withRuntime(model, true), decision: true, onChange: _changeModel),
    ]));
  }

  String _resourceValue(dynamic value) => value is num && value >= 0 ? '${(value / 1024).toStringAsFixed(1)} GB' : 'Unknown';
}

class _ResourceMetric extends StatelessWidget {
  const _ResourceMetric({required this.label, required this.value});
  final String label; final String value;
  @override
  Widget build(BuildContext context) => SizedBox(width: 135, child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
    Text(label, style: Theme.of(context).textTheme.labelMedium), Text(value, style: Theme.of(context).textTheme.titleMedium),
  ]));
}

class _ModelCard extends StatelessWidget {
  const _ModelCard({required this.model, required this.decision, required this.onChange});
  final Map<String, dynamic> model; final bool decision;
  final Future<void> Function(Map<String, dynamic>, bool, String) onChange;
  @override
  Widget build(BuildContext context) {
    final enabled = model['enabled'] == true; final running = model['running'] == true;
    return Card(child: Padding(padding: const EdgeInsets.all(10), child: Column(children: [
      Row(children: [Expanded(child: Text(_asString(model['name'], _asString(model['id'])), style: Theme.of(context).textTheme.titleMedium)),
        Switch(value: enabled, onChanged: (value) => onChange(model, decision, value ? 'model_enable' : 'model_disable'))]),
      Align(alignment: Alignment.centerLeft, child: Text('${_asString(model['id'])} · ${_asString(model['tier'], decision ? 'decision' : 'medium')} · ${_asString(model['node_id'])}')),
      Align(alignment: Alignment.centerLeft, child: Text('${running ? 'Running' : 'Stopped'} · RAM ${model['ram_estimate_mb'] ?? 0} MB · VRAM ${model['vram_estimate_mb'] ?? 0} MB')),
      Row(mainAxisAlignment: MainAxisAlignment.end, children: [
        TextButton.icon(onPressed: enabled ? () => onChange(model, decision, running ? 'model_stop' : 'model_start') : null,
          icon: Icon(running ? Icons.stop : Icons.play_arrow), label: Text(running ? 'Stop process' : 'Start process')),
      ]),
    ])));
  }
}

class _NodeEditor extends StatefulWidget { const _NodeEditor(); @override State<_NodeEditor> createState() => _NodeEditorState(); }
class _NodeEditorState extends State<_NodeEditor> {
  final _id = TextEditingController(); final _name = TextEditingController(); final _endpoint = TextEditingController();
  String _type = 'local';
  @override void dispose() { _id.dispose(); _name.dispose(); _endpoint.dispose(); super.dispose(); }
  @override Widget build(BuildContext context) => AlertDialog(
    title: const Text('Add node'), content: SizedBox(width: 440, child: Column(mainAxisSize: MainAxisSize.min, children: [
      TextField(controller: _id, decoration: const InputDecoration(labelText: 'ID (letters, digits, _ and -)')),
      TextField(controller: _name, decoration: const InputDecoration(labelText: 'Name')),
      DropdownButtonFormField<String>(value: _type, decoration: const InputDecoration(labelText: 'Type'), items: const [
        DropdownMenuItem(value: 'local', child: Text('Local device')), DropdownMenuItem(value: 'lan', child: Text('Local network')),
        DropdownMenuItem(value: 'internet', child: Text('Internet (HTTPS only)')),
      ], onChanged: (v) => setState(() => _type = v ?? 'local')),
      TextField(controller: _endpoint, decoration: const InputDecoration(labelText: 'Inference endpoint (optional for local)')),
    ])), actions: [TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      FilledButton(onPressed: () {
        final id = _id.text.trim(); final name = _name.text.trim(); final endpoint = _endpoint.text.trim();
        if (id.isEmpty || name.isEmpty || (_type == 'internet' && !endpoint.startsWith('https://')) ||
            (_type == 'lan' && endpoint.isNotEmpty && !endpoint.startsWith('http://') && !endpoint.startsWith('https://'))) {
          return;
        }
        Navigator.pop(context, {'id': id, 'name': name, 'type': _type, 'enabled': true, 'endpoint': endpoint,
          'health_url': '', 'api_key': '', 'api_key_env': '', 'description': 'Added from desktop/Android client.'});
      }, child: const Text('Add node'))]);
}

class _ModelEditor extends StatefulWidget {
  const _ModelEditor({required this.nodes, required this.decision});
  final List<Map<String, dynamic>> nodes; final bool decision;
  @override State<_ModelEditor> createState() => _ModelEditorState();
}
class _ModelEditorState extends State<_ModelEditor> {
  final _id = TextEditingController(); final _name = TextEditingController(); final _modelName = TextEditingController();
  final _baseUrl = TextEditingController(); final _executable = TextEditingController(); final _modelPath = TextEditingController();
  final _args = TextEditingController(text: '-m\n{model_path}\n--host\n127.0.0.1\n--port\n{port}');
  final _port = TextEditingController(text: '8103'); final _ram = TextEditingController(text: '0'); final _vram = TextEditingController(text: '0');
  String _nodeId = 'local'; String _tier = 'large';
  @override void initState() { super.initState(); if (widget.nodes.isNotEmpty && !widget.nodes.any((n) => n['id'] == _nodeId)) _nodeId = _asString(widget.nodes.first['id']); }
  @override void dispose() { for (final c in [_id, _name, _modelName, _baseUrl, _executable, _modelPath, _args, _port, _ram, _vram]) { c.dispose(); } super.dispose(); }
  @override Widget build(BuildContext context) {
    final currentNode = widget.nodes.firstWhere((n) => _asString(n['id']) == _nodeId, orElse: () => <String, dynamic>{'type': 'local'});
    final type = _asString(currentNode['type'], 'local');
    return AlertDialog(title: Text(widget.decision ? 'Add decision model' : 'Add LLM model'), content: SizedBox(width: 520, child: SingleChildScrollView(child: Column(mainAxisSize: MainAxisSize.min, children: [
      TextField(controller: _id, decoration: const InputDecoration(labelText: 'Unique model ID')),
      TextField(controller: _name, decoration: const InputDecoration(labelText: 'Display name')),
      TextField(controller: _modelName, decoration: const InputDecoration(labelText: 'Model/API name')),
      DropdownButtonFormField<String>(value: _nodeId, decoration: const InputDecoration(labelText: 'Node'), items: widget.nodes.map((n) => DropdownMenuItem(value: _asString(n['id']), child: Text(_asString(n['name'], _asString(n['id']))))).toList(), onChanged: (v) => setState(() { _nodeId = v ?? _nodeId; final selected = widget.nodes.firstWhere((n) => _asString(n['id']) == _nodeId, orElse: () => {}); _baseUrl.text = _asString(selected['endpoint']); })),
      DropdownButtonFormField<String>(value: _tier, decoration: const InputDecoration(labelText: 'Capability tier'), items: const [DropdownMenuItem(value: 'large', child: Text('Large — analysis/decomposition/review')), DropdownMenuItem(value: 'medium', child: Text('Medium — agent model')), DropdownMenuItem(value: 'fast', child: Text('Fast — simple subtasks')), DropdownMenuItem(value: 'ultra_fast', child: Text('Ultra fast — lightweight tasks'))], onChanged: widget.decision ? null : (v) => setState(() => _tier = v ?? 'medium')),
      TextField(controller: _baseUrl, decoration: const InputDecoration(labelText: 'OpenAI-compatible base URL')),
      if (type == 'local') ...[
        TextField(controller: _executable, decoration: const InputDecoration(labelText: 'Executable path (optional if external service)')),
        TextField(controller: _modelPath, decoration: const InputDecoration(labelText: 'Weights directory/file (optional)')),
        TextField(controller: _args, minLines: 2, maxLines: 4, decoration: const InputDecoration(labelText: 'One launch argument per line')),
        TextField(controller: _port, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'Port')),
      ],
      Row(children: [Expanded(child: TextField(controller: _ram, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'RAM estimate, MB'))), const SizedBox(width: 12), Expanded(child: TextField(controller: _vram, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'VRAM estimate, MB')))]),
      const SizedBox(height: 8), const Text('New profiles are disabled until you verify paths and endpoints.', style: TextStyle(fontSize: 12)),
    ]))), actions: [TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      FilledButton(onPressed: () {
        final id = _id.text.trim(); final name = _name.text.trim(); final modelName = _modelName.text.trim();
        final port = int.tryParse(_port.text) ?? 0; final ram = int.tryParse(_ram.text) ?? -1; final vram = int.tryParse(_vram.text) ?? -1;
        final baseUrl = _baseUrl.text.trim();
        if (id.isEmpty || name.isEmpty || modelName.isEmpty || baseUrl.isEmpty || ram < 0 || vram < 0 || (type == 'local' && _executable.text.trim().isNotEmpty && (port < 1 || port > 65535))) return;
        final args = _args.text.split('\n').map((s) => s.trim()).where((s) => s.isNotEmpty).toList();
        Navigator.pop(context, {'id': id, 'name': name, 'enabled': false, 'tier': widget.decision ? 'decision' : _tier,
          'auto_start': false, 'start_at_boot': false, 'executable': _executable.text.trim(), 'model_path': _modelPath.text.trim(),
          'working_directory': '.', 'args': args, 'port': port, 'base_url': baseUrl, 'health_url': '', 'model_name': modelName,
          'api_key': '', 'startup_timeout_seconds': 120, 'shutdown_timeout_seconds': 10, 'request_timeout_seconds': 120,
          'temperature': widget.decision ? 0.1 : 0.2, 'node_id': _nodeId, 'location_type': type == 'local' ? 'local' : 'remote',
          'ram_estimate_mb': ram, 'vram_estimate_mb': vram});
      }, child: const Text('Add model'))]);
  }
}

class AgentsPage extends StatefulWidget { const AgentsPage({super.key, required this.client}); final AsgendClient client; @override State<AgentsPage> createState() => _AgentsPageState(); }
class _AgentsPageState extends State<AgentsPage> {
  Map<String, dynamic> _configs = {}; bool _loading = true; bool _saving = false; String? _error;
  @override void initState() { super.initState(); _load(); }
  Future<void> _load() async { try { final data = await widget.client.rpc({'op': 'config_get'}); if (mounted) setState(() { _configs = data; _loading = false; _error = null; }); } catch (e) { if (mounted) setState(() { _error = '$e'; _loading = false; }); } }
  Future<void> _save(Map<String, dynamic> data) async { setState(() { _saving = true; _error = null; }); try { await widget.client.rpc({'op': 'config_save', 'main': data['main'], 'models': data['models'], 'agents': data['agents']}); await _load(); if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Execution profile saved. Restart the core to apply changes.'))); } catch (e) { if (mounted) setState(() => _error = '$e'); } finally { if (mounted) setState(() => _saving = false); } }
  Future<void> _edit(Map<String, dynamic>? existing) async {
    final result = await showDialog<Map<String, dynamic>>(context: context, builder: (_) => _AgentEditor(existing: existing, models: _list(_map(_configs['models'])['models']).map(_map).toList()));
    if (result == null) return;
    final data = jsonDecode(jsonEncode(_configs)) as Map<String, dynamic>; final agents = _list(data['agents']);
    if (existing == null && agents.any((a) => _asString(_map(a)['id']) == result['id'])) { if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Profile ID already exists.'))); return; }
    if (existing == null) { agents.add(result); } else { final idx = agents.indexWhere((a) => _asString(_map(a)['id']) == _asString(existing['id'])); if (idx >= 0) agents[idx] = result; }
    data['agents'] = agents; await _save(data);
  }
  @override Widget build(BuildContext context) {
    final agents = _list(_configs['agents']).map(_map).toList(); final models = _list(_map(_configs['models'])['models']).map(_map).toList();
    return _loading ? const Center(child: CircularProgressIndicator()) : RefreshIndicator(onRefresh: _load, child: ListView(padding: const EdgeInsets.all(16), children: [
      Row(children: [Expanded(child: Text('Execution profiles', style: Theme.of(context).textTheme.headlineSmall)), FilledButton.icon(onPressed: _saving ? null : () => _edit(null), icon: const Icon(Icons.add), label: const Text('Add profile'))]),
      const Padding(padding: EdgeInsets.only(bottom: 12), child: Text('Execution profiles define capabilities, permissions and instructions. The decision model selects one profile for the whole task; independent resource classifiers choose resources for each stage.')),
      if (_error != null) Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error)),
      for (final agent in agents) Card(child: ListTile(leading: const Icon(Icons.smart_toy_outlined), title: Text(_asString(agent['name'], _asString(agent['id']))), subtitle: Text('${_asString(agent['id'])} · model ${_asString(agent['model_id'], '(default)')}\n${_asString(agent['description'])}'), isThreeLine: true, trailing: IconButton(onPressed: _saving ? null : () => _edit(agent), icon: const Icon(Icons.edit_outlined)))),
      if (models.isEmpty) const Text('No regular LLM profiles are configured yet. Add them in Nodes.'),
    ]));
  }
}

class _AgentEditor extends StatefulWidget { const _AgentEditor({required this.existing, required this.models}); final Map<String, dynamic>? existing; final List<Map<String, dynamic>> models; @override State<_AgentEditor> createState() => _AgentEditorState(); }
class _AgentEditorState extends State<_AgentEditor> {
  late final TextEditingController _id, _name, _description, _prompt, _skills, _tools; String _model = '';
  @override void initState() { super.initState(); final a = widget.existing ?? {}; _id = TextEditingController(text: _asString(a['id'])); _name = TextEditingController(text: _asString(a['name'])); _description = TextEditingController(text: _asString(a['description'])); _prompt = TextEditingController(text: _asString(a['system_prompt'], 'You are a flexible agent. Follow the assigned subtask, selected skills, allowed tools and supplied knowledge.')); _skills = TextEditingController(text: _list(a['skills']).join(', ')); _tools = TextEditingController(text: _list(a['tools']).join(', ')); _model = _asString(a['model_id']); }
  @override void dispose() { for (final c in [_id, _name, _description, _prompt, _skills, _tools]) { c.dispose(); } super.dispose(); }
  @override Widget build(BuildContext context) => AlertDialog(title: Text(widget.existing == null ? 'Add execution profile' : 'Edit execution profile'), content: SizedBox(width: 560, child: SingleChildScrollView(child: Column(mainAxisSize: MainAxisSize.min, children: [
    TextField(controller: _id, enabled: widget.existing == null, decoration: const InputDecoration(labelText: 'Unique ID')),
    TextField(controller: _name, decoration: const InputDecoration(labelText: 'Name')),
    TextField(controller: _description, decoration: const InputDecoration(labelText: 'Description')),
    TextField(controller: _prompt, minLines: 3, maxLines: 7, decoration: const InputDecoration(labelText: 'System prompt')),
    DropdownButtonFormField<String>(value: _model, decoration: const InputDecoration(labelText: 'Preferred model (optional)'), items: [const DropdownMenuItem(value: '', child: Text('(Use default model)')), ...widget.models.map((m) => DropdownMenuItem(value: _asString(m['id']), child: Text(_asString(m['name'], _asString(m['id'])))))], onChanged: (v) => setState(() => _model = v ?? '')),
    TextField(controller: _skills, decoration: const InputDecoration(labelText: 'Default skill IDs, comma-separated')),
    TextField(controller: _tools, decoration: const InputDecoration(labelText: 'Allowed tool IDs, comma-separated')),
  ]))), actions: [TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')), FilledButton(onPressed: () {
    final id = _id.text.trim(); final name = _name.text.trim(); if (id.isEmpty || name.isEmpty) return;
    List<String> split(TextEditingController c) => c.text.split(',').map((v) => v.trim()).where((v) => v.isNotEmpty).toList();
    Navigator.pop(context, {'id': id, 'name': name, 'description': _description.text.trim(), 'system_prompt': _prompt.text.trim(), 'model_id': _model, 'skills': split(_skills), 'tools': split(_tools)});
  }, child: const Text('Save agent'))]);
}
