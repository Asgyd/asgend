// Local/desktop core mode: task list + live task chat (raw core JSON API).
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';

import '../../api/core_client.dart';
import '../../models/json.dart';
import '../../models/remote_models.dart';
import '../../util/redact.dart';
import '../common/flutter_screenshot.dart';
import '../common/question_dialog.dart';

String coreStatusLabel(String status) => switch (status) {
      'queued' => 'Queued',
      'running' => 'Running',
      'waiting_input' => 'Waiting for your answer',
      'completed' => 'Completed',
      'completed_without_model' => 'Completed (fallback)',
      'failed' => 'Failed',
      'cancelled' => 'Cancelled',
      _ => status,
    };

String _err(Object e, CoreClient c) => safeError(e, [c.token]);

class TaskDashboard extends StatefulWidget {
  const TaskDashboard({super.key, required this.client, required this.onConnectionError});
  final CoreClient client;
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
  String? _submitKey;
  String? _submitFingerprint;

  @override
  void initState() {
    super.initState();
    _refresh();
    _timer = Timer.periodic(const Duration(seconds: 3), (_) => _refresh());
  }

  @override
  void dispose() {
    _timer?.cancel();
    _taskCtl.dispose();
    super.dispose();
  }

  Future<void> _refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      final result = await widget.client.rpc({'op': 'list_tasks', 'limit': 100}, timeout: const Duration(seconds: 8));
      if (mounted) {
        setState(() {
          _tasks = asList(result['tasks']);
          _error = null;
        });
      }
    } catch (error) {
      final message = _err(error, widget.client);
      if (mounted) setState(() => _error = message);
      widget.onConnectionError(message);
    } finally {
      _refreshing = false;
    }
  }

  Future<void> _submit() async {
    final text = _taskCtl.text.trim();
    if (text.isEmpty || _busy) return;
    // Core `submit` supports idempotency_key: reuse it while the same text and
    // priority are retried after an error, so a lost reply never duplicates.
    final fingerprint = '$_priority\u0000$text';
    if (_submitFingerprint != fingerprint || _submitKey == null) {
      _submitFingerprint = fingerprint;
      _submitKey = 'flutter-${DateTime.now().microsecondsSinceEpoch.toRadixString(36)}-${text.hashCode.toUnsigned(32).toRadixString(36)}';
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final response = await widget.client.rpc(
          {'op': 'submit', 'task': text, 'priority': _priority, 'idempotency_key': _submitKey},
          timeout: const Duration(seconds: 8));
      _submitKey = null;
      _submitFingerprint = null;
      _taskCtl.clear();
      await _refresh();
      if (!mounted) return;
      await Navigator.of(context).push(MaterialPageRoute(
          builder: (_) => TaskChatPage(client: widget.client, taskId: asString(response['task_id']))));
      await _refresh();
    } catch (error) {
      if (mounted) setState(() => _error = _err(error, widget.client));
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  void _openTask(dynamic row) {
    final id = asString(asMap(row)['task_id']);
    if (id.isEmpty) return;
    Navigator.of(context).push(MaterialPageRoute(builder: (_) => TaskChatPage(client: widget.client, taskId: id)));
  }

  @override
  Widget build(BuildContext context) {
    return ListView(padding: const EdgeInsets.all(16), children: [
      Card(
          child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text('New task', style: Theme.of(context).textTheme.titleLarge),
                const SizedBox(height: 10),
                TextField(
                    controller: _taskCtl,
                    minLines: 2,
                    maxLines: 5,
                    enabled: !_busy,
                    decoration: const InputDecoration(border: OutlineInputBorder(), hintText: 'Describe the result you need…')),
                Row(children: [
                  const Text('Priority'),
                  Expanded(
                      child: Slider(
                          value: _priority.toDouble(),
                          min: 0,
                          max: 9,
                          divisions: 9,
                          label: '$_priority',
                          onChanged: (value) => setState(() => _priority = value.round()))),
                  Text('$_priority'),
                ]),
                Align(
                    alignment: Alignment.centerRight,
                    child: FilledButton.icon(
                        onPressed: _busy ? null : _submit,
                        icon: _busy
                            ? const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2))
                            : const Icon(Icons.play_arrow),
                        label: const Text('Start task and open chat'))),
                if (_error != null)
                  Padding(
                      padding: const EdgeInsets.only(top: 8),
                      child: Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error))),
              ]))),
      const SizedBox(height: 8),
      Row(children: [
        Expanded(child: Text('Tasks', style: Theme.of(context).textTheme.titleLarge)),
        IconButton(onPressed: _refresh, icon: const Icon(Icons.refresh)),
      ]),
      if (_tasks.isEmpty)
        const Padding(
            padding: EdgeInsets.symmetric(vertical: 32),
            child: Center(child: Text('No tasks yet. Submit a task above to open its live chat.'))),
      for (final row in _tasks)
        Builder(builder: (context) {
          final task = asMap(row);
          final id = asString(task['task_id']);
          final status = asString(task['status'], 'unknown');
          return Card(
              child: ListTile(
            leading: CircleAvatar(
                child: Icon(status == 'waiting_input'
                    ? Icons.help_outline
                    : status == 'failed'
                        ? Icons.error_outline
                        : status == 'completed' || status == 'completed_without_model'
                            ? Icons.check
                            : Icons.pending_actions)),
            title: Text(asString(task['task'], asString(task['task_id'], 'Task')), maxLines: 2, overflow: TextOverflow.ellipsis),
            subtitle: Text(
                '${coreStatusLabel(status)} · priority ${task['priority'] ?? 5} · ${id.isNotEmpty ? id.substring(0, id.length > 8 ? 8 : id.length) : ''}'),
            trailing: const Icon(Icons.chevron_right),
            onTap: () => _openTask(task),
          ));
        }),
    ]);
  }
}

class TaskChatPage extends StatefulWidget {
  const TaskChatPage({super.key, required this.client, required this.taskId});
  final CoreClient client;
  final String taskId;
  @override
  State<TaskChatPage> createState() => _TaskChatPageState();
}

class _TaskChatPageState extends State<TaskChatPage> {
  final _events = <Map<String, dynamic>>[];
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
  final List<Map<String, dynamic>> _artifacts = <Map<String, dynamic>>[];
  DateTime _nextArtifactRefresh = DateTime.fromMillisecondsSinceEpoch(0);
  bool _artifactRefreshing = false;
  bool _artifactBusy = false;
  String? _selectedArtifactPath;
  Uint8List? _previewImage;
  String? _previewSource;
  String? _previewMessage;
  bool _showSource = false;
  final GlobalKey _workspacePreviewKey = GlobalKey();
  final List<Map<String, dynamic>> _recentTasks = <Map<String, dynamic>>[];
  DateTime _nextTaskListRefresh = DateTime.fromMillisecondsSinceEpoch(0);
  bool _taskListRefreshing = false;

  @override
  void initState() {
    super.initState();
    _refresh();
    _refreshArtifacts();
    _refreshTaskList();
    _timer = Timer.periodic(const Duration(seconds: 2), (_) => _refresh());
  }

  @override
  void dispose() {
    _timer?.cancel();
    _followupCtl.dispose();
    _scrollCtl.dispose();
    super.dispose();
  }

  Future<void> _refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      final status = await widget.client.rpc({'op': 'status', 'task_id': widget.taskId}, timeout: const Duration(seconds: 8));
      final eventReply = await widget.client.rpc(
          {'op': 'events', 'task_id': widget.taskId, 'after_id': _lastEvent, 'limit': 100},
          timeout: const Duration(seconds: 8));
      final newEvents = asList(eventReply['events']).map(asMap).toList();
      if (!mounted) return;
      setState(() {
        _statusReply = status;
        _status = asString(status['status'], 'unknown');
        _loading = false;
        _error = null;
        for (final event in newEvents) {
          final eventId = asInt(event['event_id']);
          if (eventId != 0 && eventId <= _lastEvent) continue;
          _events.add(event);
          if (eventId > _lastEvent) _lastEvent = eventId;
        }
      });
      for (final event in newEvents) {
        if (event['type'] == 'question' || event['type'] == 'agent_question') {
          await _maybeAsk(asInt(event['event_id']), _questionFromEvent(event));
        }
      }
      if (_status == 'waiting_input') {
        final question = asMap(asMap(status['result'])['question']);
        if (question.isNotEmpty) await _maybeAsk(_lastEvent, question);
      }
      if (DateTime.now().isAfter(_nextArtifactRefresh)) unawaited(_refreshArtifacts());
      if (DateTime.now().isAfter(_nextTaskListRefresh)) unawaited(_refreshTaskList());
      if (newEvents.isNotEmpty && _scrollCtl.hasClients) {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (_scrollCtl.hasClients) {
            _scrollCtl.animateTo(_scrollCtl.position.maxScrollExtent,
                duration: const Duration(milliseconds: 180), curve: Curves.easeOut);
          }
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = _err(error, widget.client);
        });
      }
    } finally {
      _refreshing = false;
    }
  }

  Future<void> _refreshTaskList() async {
    if (_taskListRefreshing) return;
    _taskListRefreshing = true;
    _nextTaskListRefresh = DateTime.now().add(const Duration(seconds: 10));
    try {
      final response = await widget.client.rpc(
        {'op': 'list_tasks', 'limit': 100},
        timeout: const Duration(seconds: 10),
      );
      if (!mounted) return;
      setState(() {
        _recentTasks
          ..clear()
          ..addAll(asList(response['tasks']).map(asMap));
      });
    } catch (_) {
      // The task list is navigational context; keep the current task usable.
    } finally {
      _taskListRefreshing = false;
    }
  }

  void _openTaskFromSidebar(Map<String, dynamic> task) {
    final taskId = asString(task['task_id']);
    if (taskId.isEmpty || taskId == widget.taskId) return;
    Navigator.of(context).pushReplacement(MaterialPageRoute(
      builder: (_) => TaskChatPage(client: widget.client, taskId: taskId),
    ));
  }

  Widget _taskSidebar() {
    return Container(
      color: Theme.of(context).colorScheme.surfaceContainerLow,
      child: Column(children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 12, 6, 8),
          child: Row(children: [
            Expanded(child: Text('Tasks', style: Theme.of(context).textTheme.titleLarge)),
            IconButton(tooltip: 'Refresh tasks', onPressed: _refreshTaskList, icon: const Icon(Icons.refresh)),
            IconButton(tooltip: 'Back to task dashboard', onPressed: () => Navigator.of(context).maybePop(), icon: const Icon(Icons.dashboard_outlined)),
          ]),
        ),
        const Divider(height: 1),
        Expanded(
          child: _recentTasks.isEmpty
              ? Center(child: Text(_taskListRefreshing ? 'Loading tasks…' : 'No recent tasks.'))
              : ListView.builder(
                  itemCount: _recentTasks.length,
                  itemBuilder: (context, index) {
                    final task = _recentTasks[index];
                    final id = asString(task['task_id']);
                    final title = asString(task['title'], id);
                    final state = asString(task['status'], asString(task['state'], 'unknown'));
                    return ListTile(
                      dense: true,
                      selected: id == widget.taskId,
                      leading: Icon(id == widget.taskId ? Icons.radio_button_checked : Icons.task_alt_outlined, size: 18),
                      title: Text(title.isEmpty ? id : title, maxLines: 2, overflow: TextOverflow.ellipsis),
                      subtitle: Text('${coreStatusLabel(state)} · ${id.length > 12 ? id.substring(0, 12) : id}', maxLines: 1, overflow: TextOverflow.ellipsis),
                      onTap: id.isEmpty ? null : () => _openTaskFromSidebar(task),
                    );
                  },
                ),
        ),
      ]),
    );
  }

  Future<void> _refreshArtifacts() async {
    if (_artifactRefreshing) return;
    _artifactRefreshing = true;
    _nextArtifactRefresh = DateTime.now().add(const Duration(seconds: 10));
    try {
      final response = await widget.client.rpc(
        {'op': 'artifact_list', 'task_id': widget.taskId},
        timeout: const Duration(seconds: 12),
      );
      if (!mounted) return;
      setState(() {
        _artifacts
          ..clear()
          ..addAll(asList(response['artifacts']).map(asMap));
      });
    } catch (_) {
      // Workspace preview is supplementary; preserve the live task chat if
      // listing artifacts is temporarily unavailable.
    } finally {
      _artifactRefreshing = false;
    }
  }

  Future<Uint8List> _readWorkspaceBytes(String path, {required int maxBytes}) async {
    final output = BytesBuilder(copy: false);
    var offset = 0;
    var expectedSize = -1;
    while (true) {
      final response = await widget.client.rpc({
        'op': 'workspace_read_bytes',
        'task_id': widget.taskId,
        'path': path,
        'offset': offset,
        'limit': 65536,
      }, timeout: const Duration(seconds: 12));
      if (response['ok'] != true) {
        throw StateError(asString(response['error'], 'Could not read workspace file.'));
      }
      final encoded = asString(response['data_b64']);
      final chunk = base64.decode(encoded);
      output.add(chunk);
      offset = asInt(response['next_offset'], offset + chunk.length);
      expectedSize = asInt(response['size'], expectedSize);
      if (offset > maxBytes || output.length > maxBytes) {
        throw StateError('File is too large to preview in the client.');
      }
      if (response['eof'] == true || chunk.isEmpty) break;
      if (expectedSize >= 0 && offset >= expectedSize) break;
    }
    return output.takeBytes();
  }

  String _baseName(String path) => path.split('/').last;
  String _extension(String path) {
    final name = _baseName(path);
    final dot = name.lastIndexOf('.');
    return dot < 0 ? '' : name.substring(dot).toLowerCase();
  }

  Future<void> _openWorkspaceArtifact(Map<String, dynamic> artifact) async {
    if (_artifactBusy) return;
    final path = asString(artifact['path']);
    if (path.isEmpty) return;
    setState(() {
      _artifactBusy = true;
      _selectedArtifactPath = path;
      _previewImage = null;
      _previewSource = null;
      _previewMessage = 'Loading verified workspace preview…';
      _showSource = false;
    });
    try {
      final mime = asString(artifact['mime'], 'application/octet-stream');
      final ext = _extension(path);
      final imageFile = mime.startsWith('image/');
      final textFile = const {
        '.c', '.h', '.cc', '.cpp', '.hpp', '.py', '.rs', '.go', '.java', '.kt',
        '.dart', '.js', '.ts', '.tsx', '.jsx', '.css', '.scss', '.json', '.yaml',
        '.yml', '.toml', '.xml', '.sh', '.sql', '.md', '.txt', '.html', '.htm', '.svg',
      }.contains(ext);
      Uint8List? image;
      String? source;
      if (imageFile) {
        image = await _readWorkspaceBytes(path, maxBytes: 24 * 1024 * 1024);
      } else {
        if (textFile && asInt(artifact['size']) <= 1500000) {
          final bytes = await _readWorkspaceBytes(path, maxBytes: 1500000);
          source = utf8.decode(bytes, allowMalformed: true);
        }
        final fileName = _baseName(path);
        final dot = fileName.lastIndexOf('.');
        final stem = dot > 0 ? fileName.substring(0, dot) : fileName;
        final safeStem = stem.substring(0, stem.length > 48 ? 48 : stem.length);
        final sourceHash = asString(artifact['sha256']);
        final expectedPreview = sourceHash.length >= 12
            ? 'preview_${safeStem}_${sourceHash.substring(0, 12)}_p1.png'
            : null;
        Map<String, dynamic>? preview;
        for (final item in _artifacts) {
          final candidate = asString(item['path']).split('/').last;
          if (asString(item['mime']) == 'image/png' &&
              (expectedPreview != null
                  ? candidate == expectedPreview
                  : candidate.startsWith('preview_${safeStem}_'))) {
            preview = item;
            break;
          }
        }
        if (preview != null) {
          image = await _readWorkspaceBytes(asString(preview['path']), maxBytes: 8 * 1024 * 1024);
        }
      }
      if (!mounted) return;
      setState(() {
        _previewImage = image;
        _previewSource = source;
        _previewMessage = image == null && source == null
            ? 'No screenshot is available yet. Ask the agent to call render_and_inspect after creating or changing this file.'
            : null;
      });
      if (MediaQuery.sizeOf(context).width < 1200) {
        await showModalBottomSheet<void>(
          context: context,
          isScrollControlled: true,
          useSafeArea: true,
          builder: (sheetContext) => DraggableScrollableSheet(
            initialChildSize: 0.90,
            minChildSize: 0.55,
            maxChildSize: 0.96,
            expand: false,
            builder: (context, controller) {
              var sheetShowSource = _showSource;
              return StatefulBuilder(builder: (context, setSheetState) => _workspacePreviewContent(
                controller: controller,
                showSource: sheetShowSource,
                onShowSource: (value) => setSheetState(() => sheetShowSource = value),
              ));
            },
          ),
        );
      }
    } catch (error) {
      if (mounted) setState(() => _previewMessage = safeError(error, [widget.client.token]));
    } finally {
      if (mounted) setState(() => _artifactBusy = false);
    }
  }

  IconData _workspaceIcon(Map<String, dynamic> artifact) {
    final ext = _extension(asString(artifact['path']));
    if (ext == '.docx' || ext == '.odt' || ext == '.rtf') return Icons.description_outlined;
    if (ext == '.xlsx' || ext == '.ods' || ext == '.csv') return Icons.table_chart_outlined;
    if (ext == '.pptx' || ext == '.odp') return Icons.slideshow_outlined;
    if (ext == '.pdf') return Icons.picture_as_pdf_outlined;
    if (ext == '.html' || ext == '.htm' || ext == '.svg') return Icons.web_outlined;
    if (asString(artifact['mime']).startsWith('image/')) return Icons.image_outlined;
    return Icons.code_outlined;
  }

  Widget _workspaceFiles({bool compact = false}) {
    if (_artifacts.isEmpty) {
      return Padding(
        padding: const EdgeInsets.all(14),
        child: Text(_artifactRefreshing ? 'Scanning task workspace…' : 'Files created by the agent will appear here.'),
      );
    }
    return ListView.builder(
      shrinkWrap: compact,
      physics: compact ? const NeverScrollableScrollPhysics() : null,
      itemCount: _artifacts.length,
      itemBuilder: (context, index) {
        final item = _artifacts[index];
        final path = asString(item['path']);
        final selected = path == _selectedArtifactPath;
        return ListTile(
          dense: compact,
          selected: selected,
          leading: Icon(_workspaceIcon(item)),
          title: Text(_baseName(path), maxLines: 1, overflow: TextOverflow.ellipsis),
          subtitle: Text('${asString(item['mime'], 'file')} · ${_localSize(asInt(item['size']))}', maxLines: 1, overflow: TextOverflow.ellipsis),
          onTap: _artifactBusy ? null : () => _openWorkspaceArtifact(item),
        );
      },
    );
  }

  String _localSize(int bytes) => bytes < 1024
      ? '$bytes B'
      : bytes < 1024 * 1024
          ? '${(bytes / 1024).toStringAsFixed(1)} KB'
          : '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';

  Future<void> _captureNativeWorkspacePreview() async {
    File? snapshotFile;
    try {
      final png = await FlutterScreenshot.capturePng(
        _workspacePreviewKey,
        pixelRatio: 1.35,
        maxBytes: 12 * 1024 * 1024,
        maxPixels: 16 * 1000 * 1000,
      );
      final directory = await getTemporaryDirectory();
      snapshotFile = File('${directory.path}${Platform.pathSeparator}asgend-workspace-preview-${DateTime.now().microsecondsSinceEpoch}.png');
      await snapshotFile.writeAsBytes(png, flush: true);
      await SharePlus.instance.share(ShareParams(
        files: [XFile(snapshotFile.path, mimeType: 'image/png', name: 'asgend-workspace-preview.png')],
        title: 'ASGEND workspace preview · ${_baseName(_selectedArtifactPath ?? 'workspace')}',
      ));
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Native workspace preview captured as PNG.')),
        );
      }
    } catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Could not capture workspace preview: ${_err(error, widget.client)}')),
        );
      }
    } finally {
      // SharePlus completes after the native share flow returns. Avoid leaving
      // a potentially sensitive workspace screenshot in the app cache.
      if (snapshotFile != null) {
        try { await snapshotFile.delete(); } catch (_) { /* best-effort cleanup */ }
      }
    }
  }

  Widget _workspacePreviewContent({ScrollController? controller, bool? showSource, ValueChanged<bool>? onShowSource}) {
    final selectedName = _selectedArtifactPath == null ? 'No file selected' : _baseName(_selectedArtifactPath!);
    final showSourceNow = showSource ?? _showSource;
    return RepaintBoundary(
      key: _workspacePreviewKey,
      child: Material(
        color: Theme.of(context).colorScheme.surface,
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 14, 8, 10),
          child: Row(children: [
            const Icon(Icons.workspaces_outline),
            const SizedBox(width: 8),
            Expanded(child: Text(selectedName, maxLines: 1, overflow: TextOverflow.ellipsis, style: Theme.of(context).textTheme.titleMedium)),
            if (_previewSource != null)
              SegmentedButton<bool>(
                segments: const [
                  ButtonSegment(value: false, label: Text('Preview')),
                  ButtonSegment(value: true, label: Text('Source')),
                ],
                selected: {showSourceNow},
                onSelectionChanged: (value) {
                  if (onShowSource != null) onShowSource(value.first);
                  else setState(() => _showSource = value.first);
                },
              ),
            IconButton(tooltip: 'Capture native PNG snapshot', onPressed: _captureNativeWorkspacePreview, icon: const Icon(Icons.photo_camera_outlined)),
            IconButton(tooltip: 'Refresh files', onPressed: _refreshArtifacts, icon: const Icon(Icons.refresh)),
          ]),
        ),
        const Divider(height: 1),
        Expanded(
          child: showSourceNow && _previewSource != null
              ? SingleChildScrollView(
                  controller: controller,
                  padding: const EdgeInsets.all(14),
                  child: SelectableText(_previewSource!, style: const TextStyle(fontFamily: 'monospace', fontSize: 12, height: 1.45)),
                )
              : _previewImage != null
                  ? InteractiveViewer(
                      minScale: 0.5,
                      maxScale: 5,
                      child: Center(child: Image.memory(_previewImage!, fit: BoxFit.contain, errorBuilder: (_, __, ___) => const Text('Image preview could not be decoded.'))),
                    )
                  : Center(child: Padding(padding: const EdgeInsets.all(20), child: Text(_previewMessage ?? 'Select a file to inspect it.', textAlign: TextAlign.center))),
        ),
        if (_previewSource != null && _previewImage != null && !showSourceNow)
          Padding(padding: const EdgeInsets.all(8), child: Text('Visual snapshot generated by the agent. Use Source to inspect the underlying file.', style: Theme.of(context).textTheme.bodySmall)),
        ]),
      ),
    );
  }

  Widget _workspacePane() {
    return Container(
      decoration: BoxDecoration(color: Theme.of(context).colorScheme.surfaceContainerLow, borderRadius: BorderRadius.circular(12)),
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(14, 12, 8, 10),
          child: Row(children: [
            Expanded(child: Text('Agent workspace', style: Theme.of(context).textTheme.titleLarge)),
            IconButton(tooltip: 'Refresh workspace files', onPressed: _refreshArtifacts, icon: const Icon(Icons.refresh)),
          ]),
        ),
        SizedBox(height: 190, child: _workspaceFiles()),
        const Divider(height: 1),
        Expanded(child: _workspacePreviewContent()),
      ]),
    );
  }

  Map<String, dynamic> _questionFromEvent(Map<String, dynamic> event) {
    final payload = asMap(event['payload']);
    if (payload['question'] is Map) return asMap(payload['question']);
    return payload;
  }

  Future<void> _maybeAsk(int eventId, Map<String, dynamic> question) async {
    if (!mounted || question.isEmpty || _activeQuestionEvent == eventId) return;
    final prompt = QuestionPrompt.fromJson(question);
    if (prompt == null) return;
    _activeQuestionEvent = eventId;
    final answer = await QuestionDialog.show(context, prompt);
    if (answer == null || !mounted) return;
    try {
      await widget.client.rpc({'op': 'answer', 'task_id': widget.taskId, 'answer': answer.answer, 'option_id': answer.optionId});
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Answer sent. Task resumed.')));
      await _refresh();
    } catch (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Could not send answer: ${_err(error, widget.client)}')));
      }
      _activeQuestionEvent = null;
    }
  }

  Future<void> _cancel() async {
    try {
      await widget.client.rpc({'op': 'cancel', 'task_id': widget.taskId});
      await _refresh();
    } catch (error) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(_err(error, widget.client))));
    }
  }

  Future<void> _followup() async {
    final text = _followupCtl.text.trim();
    if (text.isEmpty || _sendingFollowup) return;
    setState(() => _sendingFollowup = true);
    try {
      final response = await widget.client.rpc({'op': 'submit', 'task': text, 'session_id': 'task-${widget.taskId}'});
      _followupCtl.clear();
      if (!mounted) return;
      await Navigator.of(context).push(
          MaterialPageRoute(builder: (_) => TaskChatPage(client: widget.client, taskId: asString(response['task_id']))));
    } catch (error) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(_err(error, widget.client))));
    } finally {
      if (mounted) setState(() => _sendingFollowup = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final result = asMap(_statusReply['result']);
    final statusError = asString(_statusReply['error']);
    final isTerminal = const {'completed', 'completed_without_model', 'failed', 'cancelled'}.contains(_status);
    return Scaffold(
      appBar: AppBar(title: const Text('Task workspace'), actions: [
        Padding(padding: const EdgeInsets.symmetric(horizontal: 10), child: Center(child: Chip(label: Text(coreStatusLabel(_status))))),
        IconButton(tooltip: 'Refresh files', onPressed: _refreshArtifacts, icon: const Icon(Icons.folder_open_outlined)),
        if (!isTerminal) IconButton(tooltip: 'Cancel task', onPressed: _cancel, icon: const Icon(Icons.stop_circle_outlined)),
        IconButton(tooltip: 'Refresh', onPressed: _refresh, icon: const Icon(Icons.refresh)),
      ]),
      body: LayoutBuilder(builder: (context, constraints) {
        final wide = constraints.maxWidth >= 1200;
        final chat = Column(children: [
          Material(
            color: Theme.of(context).colorScheme.surfaceContainerLow,
            child: Padding(padding: const EdgeInsets.all(12), child: Row(children: [
              const Icon(Icons.tag), const SizedBox(width: 8),
              Expanded(child: SelectableText(widget.taskId)),
              Text(coreStatusLabel(_status), style: Theme.of(context).textTheme.labelLarge),
            ])),
          ),
          if (_error != null) Material(color: Theme.of(context).colorScheme.errorContainer, child: Padding(padding: const EdgeInsets.all(8), child: Text(_error!))),
          if (statusError.isNotEmpty) Padding(padding: const EdgeInsets.all(8), child: Text(statusError)),
          Expanded(
            child: _loading && _events.isEmpty
                ? const Center(child: CircularProgressIndicator())
                : _events.isEmpty
                    ? const Center(child: Text('Waiting for core events…'))
                    : ListView.builder(
                        controller: _scrollCtl,
                        padding: const EdgeInsets.all(12),
                        itemCount: _events.length,
                        itemBuilder: (context, index) => CoreEventCard(event: _events[index]),
                      ),
          ),
          if (result.isNotEmpty && isTerminal)
            ExpansionTile(title: const Text('Final result'), children: [Padding(padding: const EdgeInsets.all(12), child: SelectableText(const JsonEncoder.withIndent('  ').convert(result)))]),
          if (isTerminal)
            SafeArea(top: false, child: Padding(padding: const EdgeInsets.fromLTRB(12, 4, 12, 12), child: Row(children: [
              Expanded(child: TextField(controller: _followupCtl, minLines: 1, maxLines: 3, decoration: const InputDecoration(labelText: 'Continue this session', border: OutlineInputBorder()))),
              const SizedBox(width: 8),
              IconButton.filled(onPressed: _sendingFollowup ? null : _followup, icon: const Icon(Icons.send)),
            ]))),
        ]);
        if (wide) {
          return Row(children: [
            SizedBox(width: 238, child: _taskSidebar()),
            const VerticalDivider(width: 1),
            Expanded(flex: 3, child: chat),
            const VerticalDivider(width: 1),
            Expanded(flex: 2, child: Padding(padding: const EdgeInsets.all(12), child: _workspacePane())),
          ]);
        }
        return Column(children: [
          if (_artifacts.isNotEmpty)
            ExpansionTile(
              leading: const Icon(Icons.folder_open_outlined),
              title: Text('Agent workspace · ${_artifacts.length} files'),
              children: [_workspaceFiles(compact: true)],
            ),
          Expanded(child: chat),
        ]);
      }),
    );
  }
}

class CoreEventCard extends StatelessWidget {
  const CoreEventCard({super.key, required this.event});
  final Map<String, dynamic> event;
  @override
  Widget build(BuildContext context) {
    final actor = asString(event['actor'], 'system');
    final type = prettyKey(asString(event['type'], 'event'));
    final message = asString(event['message']);
    final stamp = asString(event['created_at']);
    final payload = asMap(event['payload']);
    final text = asString(payload['content']);
    final tool = asString(payload['tool']);
    return Align(
        alignment: Alignment.centerLeft,
        child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 940),
            child: Card(
                margin: const EdgeInsets.only(bottom: 10),
                child: Padding(
                    padding: const EdgeInsets.all(12),
                    child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                      Row(children: [
                        Expanded(child: Text(actor, style: Theme.of(context).textTheme.titleSmall)),
                        Chip(label: Text(type, style: const TextStyle(fontSize: 11))),
                      ]),
                      if (message.isNotEmpty) Text(message),
                      if (tool.isNotEmpty)
                        Padding(
                            padding: const EdgeInsets.only(top: 4),
                            child: Text('Tool: $tool', style: Theme.of(context).textTheme.labelMedium)),
                      if (text.isNotEmpty) Padding(padding: const EdgeInsets.only(top: 8), child: SelectableText(text)),
                      if (stamp.isNotEmpty)
                        Padding(padding: const EdgeInsets.only(top: 6), child: Text(stamp, style: Theme.of(context).textTheme.bodySmall)),
                    ])))));
  }
}
