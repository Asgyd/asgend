import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:audioplayers/audioplayers.dart';
import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';
import 'package:record/record.dart';

import '../../api/core_client.dart';
import '../../api/voice_gateway_client.dart';
import '../../storage/settings.dart';
import '../../util/redact.dart';
import '../common/flutter_screenshot.dart';
import 'local_tasks.dart';

/// Flutter Cowork surface for local-core mode.
///
/// Voice is sent to the separate LFM3 gateway. The gateway returns a report;
/// this widget never sends user speech directly to the main task queue. The
/// only queue transition is the explicit confirmation button below the report.
class CoworkPage extends StatefulWidget {
  const CoworkPage({
    super.key,
    required this.coreClient,
    required this.settings,
    required this.isAndroid,
    this.onConfigure,
  });

  final CoreClient coreClient;
  final LocalCoreSettings settings;
  final bool isAndroid;
  final VoidCallback? onConfigure;

  @override
  State<CoworkPage> createState() => _CoworkPageState();
}

class _CoworkPageState extends State<CoworkPage> {
  final _messageController = TextEditingController();
  final _selectionController = TextEditingController();
  final _snapshotKey = GlobalKey();
  final _recorder = AudioRecorder();
  final _player = AudioPlayer();
  StreamSubscription<PlayerState>? _playerStateSubscription;
  bool _isPlayingSpeech = false;
  String? _speechAudioPath;
  VoiceGatewayClient? _gateway;
  String? _sessionId;
  String? _report;
  String? _error;
  String _projectState = 'ASGEND local workspace';
  int _revision = 0;
  int _attachmentCount = 0;
  bool _busy = false;
  bool _recording = false;
  bool _loadingProject = true;
  DateTime? _recordingStarted;
  String? _recordingPath;
  Duration _recordingElapsed = Duration.zero;
  Timer? _recordingTimer;
  String? _lastSpeechMessage;
  String? _pendingAudioPath;

  @override
  void initState() {
    super.initState();
    _configureGateway();
    _loadProjectState();
    _playerStateSubscription = _player.onPlayerStateChanged.listen((state) {
      if (mounted) setState(() => _isPlayingSpeech = state == PlayerState.playing);
    });
  }

  @override
  void didUpdateWidget(covariant CoworkPage oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.settings.voiceGatewayUrl != widget.settings.voiceGatewayUrl ||
        oldWidget.settings.voiceGatewayToken != widget.settings.voiceGatewayToken ||
        oldWidget.isAndroid != widget.isAndroid) {
      _gateway?.close();
      _gateway = null;
      _error = null;
      _configureGateway();
    }
  }

  void _configureGateway() {
    final url = widget.settings.voiceGatewayUrl.trim();
    final token = widget.settings.voiceGatewayToken;
    if (url.isEmpty || token.isEmpty) return;
    final issue = VoiceGatewayClient.validateEndpoint(url, isAndroid: widget.isAndroid);
    if (issue != null) {
      _error = issue;
      return;
    }
    _gateway = VoiceGatewayClient(baseUrl: url, token: token);
  }

  Future<void> _loadProjectState() async {
    try {
      final result = await widget.coreClient.rpc({'op': 'list_tasks', 'limit': 25}, timeout: const Duration(seconds: 10));
      final tasks = (result['tasks'] as List? ?? const []).whereType<Map>().map((row) {
        final item = Map<String, dynamic>.from(row);
        final title = (item['task'] ?? item['title'] ?? item['task_id'] ?? 'Task').toString();
        final state = (item['status'] ?? 'unknown').toString();
        final compactTitle = title.replaceAll(RegExp(r'\s+'), ' ').trim();
        return '- $compactTitle [$state]';
      }).toList();
      final taskSummary = tasks.isEmpty ? '(no recent tasks)' : tasks.join('\n');
      if (mounted) {
        setState(() {
          _projectState = 'ASGEND local core ${widget.coreClient.host}:${widget.coreClient.port}.\n'
              'Recent project tasks:\n$taskSummary';
          _loadingProject = false;
        });
      }
    } catch (_) {
      if (mounted) setState(() => _loadingProject = false);
    }
  }

  @override
  void dispose() {
    _recordingTimer?.cancel();
    _messageController.dispose();
    _selectionController.dispose();
    _gateway?.close();
    _playerStateSubscription?.cancel();
    unawaited(_disposeAudioResources());
    super.dispose();
  }

  Future<void> _disposeAudioResources() async {
    final recordingPath = _recordingPath;
    final pendingAudioPath = _pendingAudioPath;
    final speechAudioPath = _speechAudioPath;
    try {
      if (await _recorder.isRecording()) await _recorder.cancel();
    } catch (_) {
      // Best effort: disposal below still releases the platform recorder.
    }
    try {
      await _recorder.dispose();
    } catch (_) {
      // The route is already closing.
    }
    try {
      await _player.dispose();
    } catch (_) {
      // The route is already closing.
    }
    for (final path in {recordingPath, pendingAudioPath, speechAudioPath}.whereType<String>()) {
      try { await File(path).delete(); } catch (_) { /* best-effort cleanup */ }
    }
  }

  Future<VoiceGatewayClient> _ensureGateway() async {
    final gateway = _gateway;
    if (gateway == null) {
      throw const VoiceGatewayException('Configure the HTTPS voice gateway URL and ASGEND_UI_TOKEN in Connection settings.');
    }
    return gateway;
  }

  Future<String> _ensureSession() async {
    final current = _sessionId;
    if (current != null) return current;
    final gateway = await _ensureGateway();
    final id = await gateway.createSession(_projectState);
    if (mounted) setState(() => _sessionId = id);
    return id;
  }

  Future<void> _storeSpeechIfPresent(Map<String, dynamic> response) async {
    if (!mounted) return;
    final encoded = response['speech_wav_base64'];
    if (encoded is! String || encoded.isEmpty) return;
    String? createdPath;
    try {
      final bytes = base64Decode(encoded);
      if (bytes.isEmpty || bytes.length > 20 * 1024 * 1024) return;
      final directory = await getTemporaryDirectory();
      createdPath = '${directory.path}${Platform.pathSeparator}asgend_voice_reply_${DateTime.now().microsecondsSinceEpoch}.wav';
      await File(createdPath).writeAsBytes(bytes, flush: true);
      if (!mounted) {
        try { await File(createdPath).delete(); } catch (_) {}
        return;
      }
      final previous = _speechAudioPath;
      if (_isPlayingSpeech) {
        try { await _player.stop(); } catch (_) { /* best-effort stop before replacing audio */ }
      }
      _speechAudioPath = createdPath;
      if (previous != null) {
        try { await File(previous).delete(); } catch (_) { /* best-effort cleanup */ }
      }
    } catch (_) {
      // TTS is optional. A malformed/unsupported reply must not invalidate a
      // successfully updated text report.
      if (createdPath != null && createdPath != _speechAudioPath) {
        try { await File(createdPath).delete(); } catch (_) { /* best-effort cleanup */ }
      }
    }
  }

  Future<void> _playSpeechResponse() async {
    final path = _speechAudioPath;
    if (path == null) return;
    try {
      if (_isPlayingSpeech) {
        await _player.stop();
      } else {
        await _player.play(DeviceFileSource(path, mimeType: 'audio/wav'));
      }
    } catch (error) {
      if (mounted) setState(() => _error = _safeMessage(error));
    }
  }

  Future<bool> _sendTurn({
    String text = '',
    List<Map<String, String>> selections = const [],
    List<VoiceAttachment> attachments = const [],
  }) async {
    if (_busy) return false;
    if (text.trim().isEmpty && selections.isEmpty && attachments.isEmpty) return false;
    setState(() {
      _busy = true;
      _error = null;
      _lastSpeechMessage = null;
    });
    try {
      final gateway = await _ensureGateway();
      final session = await _ensureSession();
      final response = await gateway.sendTurn(
        sessionId: session,
        text: text.trim(),
        selections: selections,
        attachments: attachments,
        projectState: _projectState,
      );
      await _storeSpeechIfPresent(response);
      if (!mounted) return false;
      setState(() {
        _revision = response['revision'] is int ? response['revision'] as int : _revision;
        _report = response['report'] is String ? response['report'] as String : _report;
        _attachmentCount = response['attachment_count'] is int ? response['attachment_count'] as int : _attachmentCount;
        if (response['speech_wav_base64'] is String) {
          _lastSpeechMessage = 'Голосовой ответ подготовлен шлюзом. Текст отчёта показан ниже.';
        }
      });
      return true;
    } catch (error) {
      if (mounted) setState(() => _error = _safeMessage(error));
      return false;
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _sendText() async {
    final text = _messageController.text.trim();
    final selected = _selectionController.text.trim();
    if (text.isEmpty && selected.isEmpty) return;
    final selections = selected.isEmpty
        ? const <Map<String, String>>[]
        : <Map<String, String>>[
            {'kind': 'text', 'label': 'User-selected text / object', 'text': selected},
          ];
    final sent = await _sendTurn(text: text, selections: selections);
    if (sent) {
      _messageController.clear();
      _selectionController.clear();
    }
  }

  Future<void> _startRecording() async {
    if (_busy || _recording) return;
    String? startingPath;
    try {
      await _ensureGateway();
      final allowed = await _recorder.hasPermission();
      if (!allowed) throw const VoiceGatewayException('Microphone permission was not granted.');
      final directory = await getTemporaryDirectory();
      final path = '${directory.path}${Platform.pathSeparator}asgend_voice_${DateTime.now().microsecondsSinceEpoch}.wav';
      startingPath = path;
      await _recorder.start(
        const RecordConfig(
          encoder: AudioEncoder.wav,
          sampleRate: 16000,
          numChannels: 1,
          autoGain: true,
          echoCancel: true,
          noiseSuppress: true,
        ),
        path: path,
      );
      _recordingPath = path;
      if (!mounted) {
        try { await _recorder.cancel(); } catch (_) {}
        try { await File(path).delete(); } catch (_) {}
        _recordingPath = null;
        return;
      }
      setState(() {
        _recording = true;
        _recordingStarted = DateTime.now();
        _recordingElapsed = Duration.zero;
        _error = null;
      });
      _recordingTimer = Timer.periodic(const Duration(seconds: 1), (_) {
        final started = _recordingStarted;
        if (started == null || !mounted) return;
        final elapsed = DateTime.now().difference(started);
        if (elapsed >= const Duration(minutes: 3)) {
          unawaited(_stopRecordingAndSend());
        } else {
          setState(() => _recordingElapsed = elapsed);
        }
      });
    } catch (error) {
      final failedPath = _recordingPath ?? startingPath;
      _recordingPath = null;
      if (failedPath != null) {
        try { await File(failedPath).delete(); } catch (_) {}
      }
      if (mounted) setState(() => _error = _safeMessage(error));
    }
  }

  Future<void> _stopRecordingAndSend() async {
    if (!_recording || _busy) return;
    _recordingTimer?.cancel();
    _recordingTimer = null;
    setState(() {
      _recording = false;
      _busy = true;
    });
    String? path;
    try {
      final stoppedPath = await _recorder.stop();
      path = stoppedPath != null && stoppedPath.isNotEmpty ? stoppedPath : _recordingPath;
      _recordingPath = null;
      if (path == null || path.isEmpty) throw const VoiceGatewayException('The recorder did not return an audio file.');
      final file = File(path);
      final bytes = await file.readAsBytes();
      if (bytes.isEmpty || bytes.length > 20 * 1024 * 1024) {
        throw const VoiceGatewayException('Voice clip must be between 1 byte and 20 MiB.');
      }
      final gateway = await _ensureGateway();
      final session = await _ensureSession();
      final response = await gateway.sendTurn(
        sessionId: session,
        text: 'Пользователь передал голосовое сообщение. Обнови полный отчёт с учётом аудио.',
        attachments: [
          VoiceAttachment(
            name: 'voice-${DateTime.now().millisecondsSinceEpoch}.wav',
            mimeType: 'audio/wav',
            dataBase64: base64Encode(bytes),
          ),
        ],
        projectState: _projectState,
      );
      await _storeSpeechIfPresent(response);
      if (!mounted) return;
      setState(() {
        _revision = response['revision'] is int ? response['revision'] as int : _revision;
        _report = response['report'] is String ? response['report'] as String : _report;
        _attachmentCount = response['attachment_count'] is int ? response['attachment_count'] as int : _attachmentCount;
        _lastSpeechMessage = 'Голосовое сообщение передано посреднику и включено в отчёт.';
        _pendingAudioPath = null;
      });
    } catch (error) {
      final failedPath = path ?? _recordingPath;
      _recordingPath = null;
      if (failedPath != null && await File(failedPath).exists()) {
        if (mounted) {
          _pendingAudioPath = failedPath;
        } else {
          try { await File(failedPath).delete(); } catch (_) {}
        }
      }
      if (mounted) setState(() => _error = _safeMessage(error));
    } finally {
      if (path != null && _pendingAudioPath != path) {
        try { await File(path).delete(); } catch (_) { /* best-effort cleanup */ }
      }
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _discardPendingAudio() async {
    final path = _pendingAudioPath;
    setState(() => _pendingAudioPath = null);
    if (path != null) {
      try { await File(path).delete(); } catch (_) { /* best-effort cleanup */ }
    }
  }

  Future<void> _retryPendingAudio() async {
    final path = _pendingAudioPath;
    if (path == null || _busy) return;
    setState(() { _busy = true; _error = null; });
    try {
      final bytes = await File(path).readAsBytes();
      if (bytes.isEmpty || bytes.length > 20 * 1024 * 1024) {
        throw const VoiceGatewayException('Saved voice clip is empty or exceeds 20 MiB.');
      }
      final gateway = await _ensureGateway();
      final session = await _ensureSession();
      final response = await gateway.sendTurn(
        sessionId: session,
        text: 'Повторная отправка ранее записанного голосового сообщения.',
        attachments: [VoiceAttachment(
          name: 'voice-retry-${DateTime.now().millisecondsSinceEpoch}.wav',
          mimeType: 'audio/wav',
          dataBase64: base64Encode(bytes),
        )],
        projectState: _projectState,
      );
      await _storeSpeechIfPresent(response);
      if (!mounted) return;
      setState(() {
        _revision = response['revision'] is int ? response['revision'] as int : _revision;
        _report = response['report'] is String ? response['report'] as String : _report;
        _attachmentCount = response['attachment_count'] is int ? response['attachment_count'] as int : _attachmentCount;
        _pendingAudioPath = null;
        _lastSpeechMessage = 'Сохранённое голосовое сообщение успешно обработано.';
      });
      await File(path).delete();
    } catch (error) {
      if (mounted) setState(() => _error = _safeMessage(error));
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _captureWorkspaceAndSend() async {
    if (_busy) return;
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final png = await FlutterScreenshot.capturePng(_snapshotKey, pixelRatio: 1.5);
      final gateway = await _ensureGateway();
      final session = await _ensureSession();
      final response = await gateway.sendTurn(
        sessionId: session,
        text: 'Это снимок панели отчёта Flutter Cowork ASGEND. Учти видимый текст и состояние отчёта при его обновлении.',
        attachments: [
          VoiceAttachment(
            name: 'asgend-cowork-workspace.png',
            mimeType: 'image/png',
            dataBase64: base64Encode(png),
          ),
        ],
        projectState: _projectState,
      );
      await _storeSpeechIfPresent(response);
      if (!mounted) return;
      setState(() {
        _revision = response['revision'] is int ? response['revision'] as int : _revision;
        _report = response['report'] is String ? response['report'] as String : _report;
        _attachmentCount = response['attachment_count'] is int ? response['attachment_count'] as int : _attachmentCount;
      });
    } catch (error) {
      if (mounted) setState(() => _error = _safeMessage(error));
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _confirmReport() async {
    final gateway = _gateway;
    final session = _sessionId;
    if (gateway == null || session == null || _revision < 1 || _report == null || _busy) return;
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final result = await gateway.confirm(sessionId: session, revision: _revision);
      final taskId = result['task_id'];
      if (taskId is! String || taskId.isEmpty) throw const VoiceGatewayException('Gateway did not return a task ID.');
      if (!mounted) return;
      setState(() {
        _sessionId = null;
        _revision = 0;
        _report = null;
        _attachmentCount = 0;
      });
      await Navigator.of(context).push(MaterialPageRoute(builder: (_) => TaskChatPage(client: widget.coreClient, taskId: taskId)));
    } catch (error) {
      if (mounted) setState(() => _error = _safeMessage(error));
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  String get _formattedElapsed {
    final seconds = _recordingElapsed.inSeconds;
    return '${(seconds ~/ 60).toString().padLeft(2, '0')}:${(seconds % 60).toString().padLeft(2, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    final configured = _gateway != null;
    return LayoutBuilder(builder: (context, constraints) {
      final wide = constraints.maxWidth >= 1100;
      final report = _workspaceReportPanel();
      final composer = _composerPanel(configured);
      if (wide) {
        return Padding(
          padding: const EdgeInsets.all(12),
          child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            SizedBox(width: 340, child: composer),
            const SizedBox(width: 12),
            Expanded(child: report),
          ]),
        );
      }
      return ListView(padding: const EdgeInsets.all(12), children: [
        SizedBox(height: 420, child: composer),
        const SizedBox(height: 12),
        SizedBox(height: 520, child: report),
      ]);
    });
  }

  Widget _composerPanel(bool configured) {
    return Card(
      clipBehavior: Clip.antiAlias,
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Row(children: [
            const Icon(Icons.forum_outlined),
            const SizedBox(width: 8),
            Expanded(child: Text('Cowork · LFM3 voice bridge', style: Theme.of(context).textTheme.titleMedium)),
            if (_sessionId != null) const Chip(label: Text('Session active')),
          ]),
          if (!configured) ...[
            const SizedBox(height: 10),
            const Text('Voice gateway is not configured. Set the gateway URL and ASGEND_UI_TOKEN in Connection settings.'),
            Align(alignment: Alignment.centerLeft, child: TextButton.icon(onPressed: widget.onConfigure, icon: const Icon(Icons.settings), label: const Text('Connection settings'))),
          ],
          const SizedBox(height: 10),
          Text('Current project', style: Theme.of(context).textTheme.labelLarge),
          const SizedBox(height: 4),
          Text(_loadingProject ? 'Loading project state…' : _projectState, maxLines: 5, overflow: TextOverflow.ellipsis, style: Theme.of(context).textTheme.bodySmall),
          const Divider(height: 22),
          TextField(
            controller: _selectionController,
            minLines: 2,
            maxLines: 4,
            enabled: configured && !_busy && !_recording,
            decoration: const InputDecoration(
              labelText: 'Selected text / object context',
              hintText: 'Paste the text or describe the selected UI object…',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 8),
          Expanded(
            child: TextField(
              controller: _messageController,
              maxLines: null,
              expands: true,
              enabled: configured && !_busy && !_recording,
              textAlignVertical: TextAlignVertical.top,
              decoration: const InputDecoration(
                labelText: 'Message to the voice interpreter',
                hintText: 'Describe what needs to change. The main agent will not be called until you confirm the report.',
                border: OutlineInputBorder(),
                alignLabelWithHint: true,
              ),
              onSubmitted: (_) => _sendText(),
            ),
          ),
          const SizedBox(height: 10),
          Wrap(spacing: 8, runSpacing: 8, alignment: WrapAlignment.spaceBetween, children: [
            FilledButton.tonalIcon(
              onPressed: !configured || _busy ? null : _recording ? _stopRecordingAndSend : _startRecording,
              icon: Icon(_recording ? Icons.stop_circle_outlined : Icons.mic_none),
              label: Text(_recording ? 'Stop · $_formattedElapsed' : 'Record voice'),
            ),
            OutlinedButton.icon(
              onPressed: !configured || _busy || _recording ? null : _captureWorkspaceAndSend,
              icon: const Icon(Icons.screenshot_monitor_outlined),
              label: const Text('Send report snapshot'),
            ),
            FilledButton.icon(
              onPressed: !configured || _busy || _recording ? null : _sendText,
              icon: _busy ? const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2)) : const Icon(Icons.send),
              label: const Text('Update report'),
            ),
          ]),
          if (_pendingAudioPath != null) Padding(
            padding: const EdgeInsets.only(top: 8),
            child: Align(alignment: Alignment.centerLeft, child: TextButton.icon(
              onPressed: _busy ? null : _retryPendingAudio,
              icon: const Icon(Icons.replay),
              label: const Text('Retry saved voice clip'),
            )),
          ),
          if (_pendingAudioPath != null) Align(
            alignment: Alignment.centerLeft,
            child: TextButton(onPressed: _busy ? null : _discardPendingAudio, child: const Text('Discard saved clip')),
          ),
          if (_lastSpeechMessage != null) Padding(padding: const EdgeInsets.only(top: 8), child: Text(_lastSpeechMessage!, style: Theme.of(context).textTheme.bodySmall)),
          if (_speechAudioPath != null) Align(
            alignment: Alignment.centerLeft,
            child: TextButton.icon(
              onPressed: _playSpeechResponse,
              icon: Icon(_isPlayingSpeech ? Icons.stop_circle_outlined : Icons.volume_up_outlined),
              label: Text(_isPlayingSpeech ? 'Stop voice reply' : 'Play voice reply'),
            ),
          ),
          if (_error != null) Padding(padding: const EdgeInsets.only(top: 8), child: Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error))),
        ]),
      ),
    );
  }

  Widget _workspaceReportPanel() {
    return RepaintBoundary(
      key: _snapshotKey,
      child: Card(
        clipBehavior: Clip.antiAlias,
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 14, 12, 12),
            child: Row(children: [
              const Icon(Icons.fact_check_outlined),
              const SizedBox(width: 8),
              Expanded(child: Text('Review report', style: Theme.of(context).textTheme.titleLarge)),
              Chip(label: Text('Revision $_revision')),
              const SizedBox(width: 6),
              Chip(label: Text('$_attachmentCount attachments')),
            ]),
          ),
          const Divider(height: 1),
          Expanded(
            child: _report == null
                ? const Center(child: Padding(padding: EdgeInsets.all(24), child: Text('Send text, voice or a workspace snapshot.\n\nLFM3 will update the structured report here. You can add more context before confirming.', textAlign: TextAlign.center)))
                : SingleChildScrollView(padding: const EdgeInsets.all(18), child: SelectableText(_report!, style: Theme.of(context).textTheme.bodyMedium?.copyWith(height: 1.45))),
          ),
          if (_report != null) Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(color: Theme.of(context).colorScheme.surfaceContainerLow, border: Border(top: BorderSide(color: Theme.of(context).dividerColor))),
            child: Row(children: [
              const Expanded(child: Text('The main project agent receives this report only after you confirm the latest revision.', style: TextStyle(fontSize: 12))),
              FilledButton.icon(onPressed: _busy ? null : _confirmReport, icon: const Icon(Icons.check), label: const Text('Confirm report')),
            ]),
          ),
        ]),
      ),
    );
  }

  String _safeMessage(Object error) {
    if (error is VoiceGatewayException) {
      return safeError(error.message, [widget.settings.voiceGatewayToken, widget.settings.token]);
    }
    final value = safeError(error, [widget.settings.voiceGatewayToken, widget.settings.token]).replaceAll(RegExp(r'\s+'), ' ').trim();
    return value.length > 400 ? value.substring(0, 400) : value;
  }
}
