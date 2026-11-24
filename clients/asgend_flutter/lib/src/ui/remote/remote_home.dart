import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../api/api_error.dart';
import '../app_environment.dart';
import '../../api/artifact_download.dart';
import '../../api/endpoint_policy.dart';
import '../../models/remote_models.dart';
import '../../state/event_follower.dart';
import '../../state/remote_session.dart';
import '../../storage/artifact_export.dart';
import '../../storage/settings.dart';
import '../../storage/task_cache.dart';
import '../../util/redact.dart';

/// User-facing Control Plane mode. It shares the same platform secure store and
/// preferences as local mode, but keeps remote bearer tokens inside the API /
/// settings layers and never places them in widget state or route arguments.
class RemoteHomePage extends StatefulWidget {
  const RemoteHomePage({super.key, this.environment});

  /// Optional composition root for deterministic widget tests. Production
  /// callers use platform stores, HTTP and artifact export by default.
  final AppEnvironment? environment;

  @override
  State<RemoteHomePage> createState() => _RemoteHomePageState();
}

class _RemoteHomePageState extends State<RemoteHomePage> with WidgetsBindingObserver {
  late final AppEnvironment _environment;
  late final SettingsRepository _settings;
  late final TaskCache _cache;
  late final RemoteSession _session;
  late final Future<void> _ready;
  String _endpoint = '';
  bool _initialized = false;
  String? _initError;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _environment = widget.environment ?? AppEnvironment.platform();
    _settings = _environment.settings;
    _cache = TaskCache(_settings.prefs);
    _session = RemoteSession(
      settings: _settings,
      cache: _cache,
      httpClientFactory: _environment.httpClientFactory,
    );
    _ready = _initialize();
  }

  Future<void> _initialize() async {
    try {
      _endpoint = await _settings.lastEndpoint();
      await _session.init();
    } catch (e) {
      _initError = _safeError(e);
    } finally {
      _initialized = true;
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _session.onForegroundChanged(state == AppLifecycleState.resumed);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _session.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return FutureBuilder<void>(
      future: _ready,
      builder: (context, initial) {
        if (!_initialized) {
          return const Scaffold(body: Center(child: CircularProgressIndicator()));
        }
        return AnimatedBuilder(
          animation: _session,
          builder: (context, _) {
            if (_session.credentials == null || _session.status == RemoteStatus.unpaired) {
              return _PairingPage(
                initialEndpoint: _endpoint,
                repairReason: _session.repairReason,
                initialError: _initError,
                settings: _settings,
                onPair: (endpoint, code, deviceName) async {
                  await _session.pair(endpoint: endpoint, code: code, deviceName: deviceName);
                  _endpoint = endpoint.trim();
                  _initError = null;
                },
              );
            }
            return _RemoteDashboard(
              session: _session,
              cache: _cache,
              artifactExporter: _environment.artifactExporter,
              onExit: () => Navigator.of(context).maybePop(),
            );
          },
        );
      },
    );
  }
}

class _PairingPage extends StatefulWidget {
  const _PairingPage({
    required this.initialEndpoint,
    required this.repairReason,
    required this.initialError,
    required this.settings,
    required this.onPair,
  });

  final String initialEndpoint;
  final String? repairReason;
  final String? initialError;
  final SettingsRepository settings;
  final Future<void> Function(String endpoint, String code, String deviceName) onPair;

  @override
  State<_PairingPage> createState() => _PairingPageState();
}

class _PairingPageState extends State<_PairingPage> {
  late final TextEditingController _endpoint = TextEditingController(text: widget.initialEndpoint);
  final _code = TextEditingController();
  final _device = TextEditingController(text: 'ASGEND mobile/desktop client');
  bool _busy = false;
  String? _error;
  bool _showInsecureDev = false;
  bool _allowInsecure = false;

  @override
  void initState() {
    super.initState();
    _loadDevPolicy();
  }

  Future<void> _loadDevPolicy() async {
    try {
      final enabled = await widget.settings.devAllowInsecureHttp();
      if (mounted) setState(() => _allowInsecure = enabled);
    } catch (_) {
      // The secure default is false when settings cannot be read.
    }
  }

  @override
  void dispose() {
    _endpoint.dispose();
    _code.dispose();
    _device.dispose();
    super.dispose();
  }

  Future<void> _usePairingLinkFromClipboard() async {
    try {
      final data = await Clipboard.getData(Clipboard.kTextPlain);
      final link = PairingLink.tryParse(data?.text ?? '');
      if (link == null) {
        setState(() => _error = 'Clipboard does not contain a valid asgend-pair:// pairing link.');
        return;
      }
      setState(() {
        _endpoint.text = link.endpoint;
        _code.text = link.code;
        _error = null;
      });
    } catch (_) {
      if (mounted) setState(() => _error = 'Could not read the pairing link from the clipboard.');
    }
  }

  Future<void> _pair() async {
    if (_busy) return;
    final endpoint = _endpoint.text.trim();
    final code = _code.text.trim();
    if (endpoint.isEmpty || code.isEmpty) {
      setState(() => _error = 'Enter the Control Plane HTTPS endpoint and the one-time pairing code.');
      return;
    }
    setState(() { _busy = true; _error = null; });
    try {
      await widget.onPair(endpoint, code, _device.text.trim());
    } catch (e) {
      if (mounted) setState(() => _error = _safeError(e));
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _saveDevPolicy(bool value) async {
    await widget.settings.setDevAllowInsecureHttp(value);
    if (!mounted) return;
    setState(() => _allowInsecure = value);
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(value
          ? 'Development HTTP policy enabled. Only use with a trusted loopback/test endpoint.'
          : 'Development HTTP policy disabled.'),
    ));
  }

  @override
  Widget build(BuildContext context) {
    final error = _error ?? widget.repairReason ?? widget.initialError;
    return Scaffold(
      appBar: AppBar(
        leading: IconButton(tooltip: 'Back to local core', icon: const Icon(Icons.arrow_back), onPressed: () => Navigator.of(context).maybePop()),
        title: const Text('ASGEND · Control Plane'),
      ),
      body: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 560),
          child: ListView(padding: const EdgeInsets.all(20), shrinkWrap: true, children: [
            const Icon(Icons.phonelink_lock_outlined, size: 52),
            const SizedBox(height: 12),
            Text('Connect a device', style: Theme.of(context).textTheme.headlineSmall, textAlign: TextAlign.center),
            const SizedBox(height: 8),
            const Text(
              'Use a one-time pairing code created in the ASGEND Control Plane. The resulting bearer token is stored in platform secure storage.',
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 20),
            TextField(
              controller: _endpoint,
              keyboardType: TextInputType.url,
              autocorrect: false,
              decoration: const InputDecoration(labelText: 'Control Plane endpoint', hintText: 'https://control.example.com', border: OutlineInputBorder()),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _code,
              obscureText: true,
              autocorrect: false,
              enableSuggestions: false,
              decoration: const InputDecoration(labelText: 'One-time pairing code', border: OutlineInputBorder()),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _device,
              decoration: const InputDecoration(labelText: 'Device name (optional)', border: OutlineInputBorder()),
            ),
            Align(
              alignment: Alignment.centerLeft,
              child: TextButton.icon(
                onPressed: _usePairingLinkFromClipboard,
                icon: const Icon(Icons.qr_code_2),
                label: const Text('Use copied pairing link / QR payload'),
              ),
            ),
            if (error != null && error.isNotEmpty) ...[
              const SizedBox(height: 12),
              _InlineNotice(text: error, isError: true),
            ],
            const SizedBox(height: 16),
            FilledButton.icon(
              onPressed: _busy ? null : _pair,
              icon: _busy ? const SizedBox.square(dimension: 18, child: CircularProgressIndicator(strokeWidth: 2)) : const Icon(Icons.link),
              label: const Text('Pair device'),
            ),
            const SizedBox(height: 8),
            TextButton(
              onPressed: () => setState(() => _showInsecureDev = !_showInsecureDev),
              child: Text(_showInsecureDev ? 'Hide development network options' : 'Development network options'),
            ),
            if (_showInsecureDev)
              Card(child: Padding(padding: const EdgeInsets.all(12), child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                const Text('HTTP is refused for remote hosts. This override is intended only for isolated local tests; never send real tokens over untrusted HTTP.'),
                SwitchListTile(
                  contentPadding: EdgeInsets.zero,
                  title: const Text('Allow development HTTP endpoint'),
                  value: _allowInsecure,
                  onChanged: _saveDevPolicy,
                ),
              ]))),
          ]),
        ),
      ),
    );
  }
}

class _RemoteDashboard extends StatefulWidget {
  const _RemoteDashboard({required this.session, required this.cache, required this.artifactExporter, required this.onExit});
  final RemoteSession session;
  final TaskCache cache;
  final ArtifactExporter artifactExporter;
  final VoidCallback onExit;

  @override
  State<_RemoteDashboard> createState() => _RemoteDashboardState();
}

class _RemoteDashboardState extends State<_RemoteDashboard> {
  Future<void> _logout() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Disconnect this device?'),
        content: const Text('ASGEND will revoke the Control Plane token when online and clear local credentials and cached remote data.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Disconnect')),
        ],
      ),
    );
    if (ok != true) return;
    await widget.session.logout();
  }

  @override
  Widget build(BuildContext context) {
    final session = widget.session;
    final creds = session.credentials!;
    final memberships = session.me?.workspaces ?? const <WorkspaceMembership>[];
    final offline = session.status == RemoteStatus.offline || session.status == RemoteStatus.permissionDenied;
    return DefaultTabController(
      length: 2,
      child: Scaffold(
        appBar: AppBar(
          leading: IconButton(tooltip: 'Back to local core', icon: const Icon(Icons.arrow_back), onPressed: widget.onExit),
          title: const Text('ASGEND · Control Plane'),
          actions: [
            if (memberships.length > 1)
              PopupMenuButton<String>(
                tooltip: 'Switch workspace',
                icon: const Icon(Icons.workspaces_outline),
                onSelected: (id) async {
                  WorkspaceMembership? workspace;
                  for (final candidate in memberships) {
                    if (candidate.workspaceId == id) { workspace = candidate; break; }
                  }
                  if (workspace != null) await session.selectWorkspace(workspace);
                },
                itemBuilder: (_) => memberships.map((w) => PopupMenuItem(value: w.workspaceId, child: Text('${w.name} · ${w.role.name}'))).toList(),
              ),
            IconButton(tooltip: 'Refresh', onPressed: session.refresh, icon: const Icon(Icons.refresh)),
            IconButton(tooltip: 'Logout and revoke token', onPressed: _logout, icon: const Icon(Icons.logout)),
          ],
          bottom: const TabBar(tabs: [Tab(icon: Icon(Icons.assignment_outlined), text: 'Tasks'), Tab(icon: Icon(Icons.dns_outlined), text: 'Nodes')]),
        ),
        body: Column(children: [
          if (offline)
            Material(
              color: Theme.of(context).colorScheme.errorContainer,
              child: Padding(padding: const EdgeInsets.all(10), child: Row(children: [
                Icon(session.showsStaleData ? Icons.cloud_off : Icons.warning_amber, color: Theme.of(context).colorScheme.onErrorContainer),
                const SizedBox(width: 8),
                Expanded(child: Text(session.status == RemoteStatus.permissionDenied
                    ? 'Access denied for workspace ${creds.workspaceId}.'
                    : session.showsStaleData
                        ? 'Offline — showing cached tasks from ${session.tasksFetchedAt?.toLocal() ?? 'an earlier session'}. Mutations are not queued.'
                        : (session.lastError ?? 'Control Plane is unavailable.'))),
                TextButton(onPressed: session.refresh, child: const Text('Retry')),
              ])),
            ),
          if (session.loading && session.tasks.isEmpty) const LinearProgressIndicator(),
          Expanded(child: TabBarView(children: [
            _RemoteTasks(session: session, cache: widget.cache, artifactExporter: widget.artifactExporter),
            _RemoteNodes(session: session),
          ])),
          SafeArea(top: false, child: Padding(
            padding: const EdgeInsets.fromLTRB(12, 2, 12, 6),
            child: Row(children: [
              const Icon(Icons.verified_user_outlined, size: 16),
              const SizedBox(width: 6),
              Expanded(child: Text('${session.role.name} · workspace ${creds.workspaceId}', maxLines: 1, overflow: TextOverflow.ellipsis, style: Theme.of(context).textTheme.bodySmall)),
              Text(session.status == RemoteStatus.online ? 'Online' : session.status.name, style: Theme.of(context).textTheme.bodySmall),
            ]),
          )),
        ]),
      ),
    );
  }
}

class _RemoteTasks extends StatefulWidget {
  const _RemoteTasks({required this.session, required this.cache, required this.artifactExporter});
  final RemoteSession session;
  final TaskCache cache;
  final ArtifactExporter artifactExporter;
  @override
  State<_RemoteTasks> createState() => _RemoteTasksState();
}

class _RemoteTasksState extends State<_RemoteTasks> {
  final _task = TextEditingController();
  String? _nodeId;
  int _priority = 5;
  bool _submitting = false;
  String? _submitError;

  @override
  void dispose() { _task.dispose(); super.dispose(); }

  Future<void> _submit() async {
    final text = _task.text.trim();
    final session = widget.session;
    final nodes = session.nodes;
    final nodeId = nodes.any((n) => n.nodeId == _nodeId) ? _nodeId : (nodes.isEmpty ? null : nodes.first.nodeId);
    if (text.isEmpty || nodeId == null || !session.canOperate || _submitting) return;
    setState(() { _submitting = true; _submitError = null; });
    try {
      final task = await session.submitTask(nodeId: nodeId, text: text, priority: _priority);
      _task.clear();
      if (!mounted) return;
      await Navigator.of(context).push(MaterialPageRoute(builder: (_) => _RemoteTaskDetail(session: session, cache: widget.cache, artifactExporter: widget.artifactExporter, task: task)));
      await session.refresh();
    } catch (e) {
      if (mounted) setState(() => _submitError = _safeError(e));
    } finally { if (mounted) setState(() => _submitting = false); }
  }

  @override
  Widget build(BuildContext context) {
    final session = widget.session;
    return AnimatedBuilder(
      animation: session,
      builder: (context, _) {
        final nodes = session.nodes;
        final canCreate = session.canOperate && session.status == RemoteStatus.online && nodes.isNotEmpty;
        return ListView(key: const Key('remote-task-list'), padding: const EdgeInsets.all(14), children: [
        if (session.showsStaleData) const _InlineNotice(text: 'Cached task list — information may be stale.', isError: false),
        Card(child: Padding(padding: const EdgeInsets.all(14), child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('Create remote task', style: Theme.of(context).textTheme.titleLarge),
          const SizedBox(height: 10),
          TextField(controller: _task, enabled: canCreate && !_submitting, minLines: 2, maxLines: 5,
            decoration: const InputDecoration(border: OutlineInputBorder(), hintText: 'Describe the result you need…')),
          const SizedBox(height: 8),
          DropdownButtonFormField<String>(
            value: nodes.any((n) => n.nodeId == _nodeId) ? _nodeId : (nodes.isEmpty ? null : nodes.first.nodeId),
            decoration: const InputDecoration(labelText: 'Execution node'),
            items: nodes.map((n) => DropdownMenuItem(value: n.nodeId, child: Text('${n.name} · ${n.status}'))).toList(),
            onChanged: canCreate && !_submitting ? (v) => setState(() => _nodeId = v) : null,
          ),
          Row(children: [const Text('Priority'), Expanded(child: Slider(value: _priority.toDouble(), min: 0, max: 9, divisions: 9, label: '$_priority', onChanged: canCreate && !_submitting ? (v) => setState(() => _priority = v.round()) : null)), Text('$_priority')]),
          if (!session.canOperate) const Text('Your workspace role is read-only. You can view tasks but cannot create or change them.'),
          if (nodes.isEmpty) const Text('No execution nodes are available in this workspace.'),
          if (_submitError != null) _InlineNotice(text: _submitError!, isError: true),
          Align(alignment: Alignment.centerRight, child: FilledButton.icon(
            onPressed: canCreate && !_submitting ? _submit : null,
            icon: _submitting ? const SizedBox.square(dimension: 16, child: CircularProgressIndicator(strokeWidth: 2)) : const Icon(Icons.play_arrow),
            label: const Text('Create task'),
          )),
        ]))),
        const SizedBox(height: 12),
        Row(children: [Expanded(child: Text('Workspace tasks', style: Theme.of(context).textTheme.titleLarge)), IconButton(tooltip: 'Refresh', onPressed: session.refresh, icon: const Icon(Icons.refresh))]),
        if (session.tasks.isEmpty) const Padding(padding: EdgeInsets.symmetric(vertical: 28), child: Center(child: Text('No tasks yet.'))),
        for (final task in session.tasks) Card(child: ListTile(
          leading: CircleAvatar(child: Icon(_taskIcon(task.state))),
          title: Text(task.title, maxLines: 2, overflow: TextOverflow.ellipsis),
          subtitle: Text('${TaskStates.label(task.state)} · ${task.nodeId} · priority ${task.priority}'),
          trailing: const Icon(Icons.chevron_right),
          onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => _RemoteTaskDetail(session: session, cache: widget.cache, artifactExporter: widget.artifactExporter, task: task))),
        )),
      ]);
      },
    );
  }
}

class _RemoteNodes extends StatelessWidget {
  const _RemoteNodes({required this.session});
  final RemoteSession session;
  @override
  Widget build(BuildContext context) => AnimatedBuilder(
    animation: session,
    builder: (context, _) => RefreshIndicator(
      onRefresh: session.refresh,
      child: ListView(padding: const EdgeInsets.all(14), children: [
        Text('Execution nodes', style: Theme.of(context).textTheme.titleLarge),
        const SizedBox(height: 8),
        if (session.nodes.isEmpty) const Padding(padding: EdgeInsets.symmetric(vertical: 30), child: Center(child: Text('No nodes are visible in this workspace.'))),
        for (final node in session.nodes) Card(child: ListTile(
          leading: CircleAvatar(child: Icon(node.online ? Icons.dns : Icons.dns_outlined, color: node.online ? Colors.green : null)),
          title: Text(node.name),
          subtitle: Text('${node.nodeId} · ${node.status}${node.coreVersion == null ? '' : ' · core ${node.coreVersion}'}\nLast seen: ${node.lastSeenAt?.toLocal() ?? 'unknown'}'),
          isThreeLine: true,
          trailing: node.online ? const Icon(Icons.check_circle_outline, color: Colors.green) : const Icon(Icons.cloud_off_outlined),
        )),
      ]),
    ),
  );
}

class _RemoteTaskDetail extends StatefulWidget {
  const _RemoteTaskDetail({required this.session, required this.cache, required this.artifactExporter, required this.task});
  final RemoteSession session;
  final TaskCache cache;
  final ArtifactExporter artifactExporter;
  final RemoteTask task;
  @override
  State<_RemoteTaskDetail> createState() => _RemoteTaskDetailState();
}

class _RemoteTaskDetailState extends State<_RemoteTaskDetail> {
  late final EventFollower _follower;
  final _answer = TextEditingController();
  bool _commandBusy = false;
  bool _artifactBusy = false;
  String? _actionError;
  RemoteArtifact? _selectedArtifact;
  File? _selectedPreviewFile;
  String? _selectedSourceText;
  String? _previewMessage;
  bool _showSource = false;
  late final ArtifactExporter _exporter;

  @override
  void initState() {
    super.initState();
    _exporter = widget.artifactExporter;
    // Details routes are only created from a paired session. Capturing this
    // client also lets an in-flight poll finish safely while a revoke clears
    // the session's reference; the client itself is closed by the session.
    final client = widget.session.client!;
    _follower = EventFollower(
      client: client,
      cache: widget.cache,
      taskId: widget.task.taskId,
      onUnauthorized: () async {
        await widget.session.handleUnauthorized();
        if (mounted) Navigator.of(context).maybePop();
      },
    );
    unawaited(_startFollower());
  }

  Future<void> _startFollower() async {
    await _follower.start();
    if (mounted) await _follower.refreshArtifacts();
  }

  @override
  void dispose() {
    _follower.pause();
    _follower.dispose();
    _answer.dispose();
    super.dispose();
  }

  Future<void> _sendAnswer([String? selected]) async {
    final answer = (selected ?? _answer.text).trim();
    if (answer.isEmpty || _commandBusy) return;
    setState(() { _commandBusy = true; _actionError = null; });
    try {
      await widget.session.answer(widget.task.taskId, answer);
      _answer.clear();
      await _follower.refreshTask();
      await widget.session.refresh();
    } catch (e) {
      if (mounted) setState(() => _actionError = _safeError(e));
    } finally { if (mounted) setState(() => _commandBusy = false); }
  }

  Future<void> _answerDialog(RemoteTask task) async {
    final q = task.question;
    if (q == null || q.options.isEmpty) {
      await showDialog<void>(context: context, builder: (context) => AlertDialog(
        title: const Text('Answer question'),
        content: TextField(controller: _answer, autofocus: true, minLines: 2, maxLines: 5, decoration: InputDecoration(hintText: q?.text ?? 'Enter your answer')),
        actions: [TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')), FilledButton(onPressed: () { Navigator.pop(context); unawaited(_sendAnswer()); }, child: const Text('Send answer'))],
      ));
      return;
    }
    await showDialog<void>(context: context, builder: (context) => AlertDialog(
      title: const Text('Choose an answer'),
      content: SizedBox(width: 460, child: SingleChildScrollView(child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Text(q.text), const SizedBox(height: 8),
        for (final option in q.options) Padding(padding: const EdgeInsets.only(bottom: 6), child: OutlinedButton(onPressed: () { Navigator.pop(context); unawaited(_sendAnswer(option.id)); }, child: Text(option.label))),
        TextButton(onPressed: () async { Navigator.pop(context); await _answerDialog( const RemoteTask(taskId: '', workspaceId: '', nodeId: '', title: '', state: 'waiting_input')); }, child: const Text('Enter a different answer')),
      ]))),
      actions: [TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel'))],
    ));
  }

  Future<void> _command(String verb) async {
    if (_commandBusy) return;
    setState(() { _commandBusy = true; _actionError = null; });
    try {
      if (verb == 'cancel') await widget.session.cancel(widget.task.taskId);
      if (verb == 'retry') await widget.session.retry(widget.task.taskId);
      await _follower.refreshTask();
      await widget.session.refresh();
    } catch (e) {
      if (mounted) setState(() => _actionError = _safeError(e));
    } finally { if (mounted) setState(() => _commandBusy = false); }
  }

  Future<bool> _confirmExecutable(RemoteArtifact artifact) async {
    if (!isPotentiallyExecutable(artifact.name, artifact.mime)) return true;
    return await showDialog<bool>(context: context, builder: (context) => AlertDialog(
      title: const Text('Potentially executable file'),
      content: Text('${artifact.name} may run code or contain active content. Only export it if you trust the source.'),
      actions: [TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')), FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('I trust this file'))],
    )) ?? false;
  }

  String _artifactStem(RemoteArtifact artifact) {
    final name = artifact.name.split('/').last;
    final dot = name.lastIndexOf('.');
    return dot > 0 ? name.substring(0, dot) : name;
  }

  String _artifactExtension(RemoteArtifact artifact) {
    final name = artifact.name.split('/').last;
    final dot = name.lastIndexOf('.');
    return dot < 0 ? '' : name.substring(dot).toLowerCase();
  }

  bool _isTextArtifact(RemoteArtifact artifact) => const {
    '.c', '.h', '.cc', '.cpp', '.hpp', '.py', '.rs', '.go', '.java', '.kt',
    '.dart', '.js', '.ts', '.tsx', '.jsx', '.css', '.scss', '.json', '.yaml',
    '.yml', '.toml', '.xml', '.sh', '.sql', '.md', '.txt', '.html', '.htm', '.svg',
  }.contains(_artifactExtension(artifact));

  Future<void> _openArtifact(RemoteArtifact artifact) async {
    final client = widget.session.client;
    if (client == null || !artifact.available || _artifactBusy) return;
    setState(() {
      _artifactBusy = true;
      _selectedArtifact = artifact;
      _selectedPreviewFile = null;
      _selectedSourceText = null;
      _previewMessage = 'Downloading and verifying artifact…';
      _showSource = false;
      _actionError = null;
    });
    try {
      final directory = await _exporter.downloadDirectory();
      final downloaded = await ArtifactDownloader(client).download(artifact, directory);
      File? preview;
      String? sourceText;
      if (artifact.mime.startsWith('image/')) {
        preview = downloaded.file;
      } else {
        if (_isTextArtifact(artifact) && artifact.size <= 1500000) {
          sourceText = await downloaded.file.readAsString();
        }
        final stem = _artifactStem(artifact);
        final safeStem = stem.substring(0, stem.length > 48 ? 48 : stem.length);
        final expectedPreview = artifact.sha256.length >= 12
            ? 'preview_${safeStem}_${artifact.sha256.substring(0, 12)}_p1.png'
            : null;
        RemoteArtifact? companion;
        for (final candidate in _follower.artifacts) {
          final leaf = candidate.name.split('/').last;
          if (candidate.available && candidate.mime == 'image/png' &&
              (expectedPreview != null
                  ? leaf == expectedPreview
                  : leaf.startsWith('preview_${safeStem}_'))) {
            companion = candidate;
            break;
          }
        }
        if (companion != null) {
          final previewDownload = await ArtifactDownloader(client).download(companion, directory);
          preview = previewDownload.file;
        }
      }
      if (!mounted) return;
      setState(() {
        _selectedPreviewFile = preview;
        _selectedSourceText = sourceText;
        _previewMessage = preview == null && sourceText == null
            ? 'No visual snapshot is available. Ask the agent to run render_and_inspect after creating or changing this file.'
            : null;
      });
      if (MediaQuery.sizeOf(context).width < 1200) {
        await showModalBottomSheet<void>(
          context: context,
          isScrollControlled: true,
          useSafeArea: true,
          builder: (_) => DraggableScrollableSheet(
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
    } catch (e) {
      if (mounted) setState(() => _previewMessage = _safeError(e));
    } finally {
      if (mounted) setState(() => _artifactBusy = false);
    }
  }

  Widget _workspacePreviewContent({ScrollController? controller, bool? showSource, ValueChanged<bool>? onShowSource}) {
    final selectedName = _selectedArtifact?.name ?? 'Select a project file';
    return Material(
      color: Theme.of(context).colorScheme.surface,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 14, 8, 10),
          child: Row(children: [
            const Icon(Icons.workspaces_outline),
            const SizedBox(width: 8),
            Expanded(child: Text(selectedName, maxLines: 1, overflow: TextOverflow.ellipsis, style: Theme.of(context).textTheme.titleMedium)),
            if (_selectedSourceText != null)
              SegmentedButton<bool>(
                segments: const [ButtonSegment(value: false, label: Text('Visual')), ButtonSegment(value: true, label: Text('Source'))],
                selected: {showSourceNow},
                onSelectionChanged: (value) {
                  if (onShowSource != null) onShowSource(value.first);
                  else setState(() => _showSource = value.first);
                },
              ),
          ]),
        ),
        const Divider(height: 1),
        Expanded(
          child: showSourceNow && _selectedSourceText != null
              ? SingleChildScrollView(controller: controller, padding: const EdgeInsets.all(14), child: SelectableText(_selectedSourceText!, style: const TextStyle(fontFamily: 'monospace', fontSize: 12, height: 1.45)))
              : _selectedPreviewFile != null
                  ? InteractiveViewer(minScale: 0.5, maxScale: 5, child: Center(child: Image.file(_selectedPreviewFile!, fit: BoxFit.contain, errorBuilder: (_, __, ___) => const Text('Preview could not be decoded.'))))
                  : Center(child: Padding(padding: const EdgeInsets.all(20), child: Text(_previewMessage ?? 'Choose a file to inspect its visual result.', textAlign: TextAlign.center))),
        ),
        if (_selectedSourceText != null && _selectedPreviewFile != null && !showSourceNow)
          Padding(padding: const EdgeInsets.all(8), child: Text('This is the agent-generated visual snapshot. The original source remains available in the task artifacts.', style: Theme.of(context).textTheme.bodySmall)),
      ]),
    );
  }

  Widget _taskSidebar() {
    final tasks = widget.session.tasks;
    return Container(
      color: Theme.of(context).colorScheme.surfaceContainerLow,
      child: Column(children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 12, 6, 8),
          child: Row(children: [
            Expanded(child: Text('Tasks', style: Theme.of(context).textTheme.titleLarge)),
            IconButton(tooltip: 'Refresh tasks', onPressed: widget.session.refresh, icon: const Icon(Icons.refresh)),
            IconButton(tooltip: 'Back to workspace', onPressed: () => Navigator.of(context).maybePop(), icon: const Icon(Icons.dashboard_outlined)),
          ]),
        ),
        const Divider(height: 1),
        Expanded(
          child: tasks.isEmpty
              ? const Center(child: Text('No tasks in this workspace.'))
              : ListView.builder(
                  itemCount: tasks.length,
                  itemBuilder: (context, index) {
                    final task = tasks[index];
                    return ListTile(
                      dense: true,
                      selected: task.taskId == widget.task.taskId,
                      leading: Icon(task.taskId == widget.task.taskId ? Icons.radio_button_checked : Icons.task_alt_outlined, size: 18),
                      title: Text(task.title, maxLines: 2, overflow: TextOverflow.ellipsis),
                      subtitle: Text('${TaskStates.label(task.state)} · ${task.taskId.length > 12 ? task.taskId.substring(0, 12) : task.taskId}', maxLines: 1, overflow: TextOverflow.ellipsis),
                      onTap: task.taskId == widget.task.taskId ? null : () {
                        Navigator.of(context).pushReplacement(MaterialPageRoute(
                          builder: (_) => _RemoteTaskDetail(
                            session: widget.session,
                            cache: widget.cache,
                            artifactExporter: widget.artifactExporter,
                            task: task,
                          ),
                        ));
                      },
                    );
                  },
                ),
        ),
      ]),
    );
  }

  Widget _workspacePane() {
    return Container(
      decoration: BoxDecoration(color: Theme.of(context).colorScheme.surfaceContainerLow, borderRadius: BorderRadius.circular(12)),
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        Padding(padding: const EdgeInsets.fromLTRB(14, 12, 8, 10), child: Row(children: [
          Expanded(child: Text('Agent workspace', style: Theme.of(context).textTheme.titleLarge)),
          IconButton(tooltip: 'Refresh artifacts', onPressed: _follower.refreshArtifacts, icon: const Icon(Icons.refresh)),
        ])),
        SizedBox(height: 190, child: _follower.artifacts.isEmpty
            ? const Center(child: Text('No project files yet.'))
            : ListView.builder(
                itemCount: _follower.artifacts.length,
                itemBuilder: (context, index) {
                  final item = _follower.artifacts[index];
                  return ListTile(
                    dense: true,
                    selected: item.artifactId == _selectedArtifact?.artifactId,
                    leading: Icon(item.mime.startsWith('image/') ? Icons.image_outlined : Icons.insert_drive_file_outlined),
                    title: Text(item.name, maxLines: 1, overflow: TextOverflow.ellipsis),
                    subtitle: Text('${item.mime} · ${_humanSize(item.size)}'),
                    onTap: item.available && !_artifactBusy ? () => _openArtifact(item) : null,
                  );
                },
              )),
        const Divider(height: 1),
        Expanded(child: _workspacePreviewContent()),
      ]),
    );
  }

  Future<void> _download(RemoteArtifact artifact) async {
    final client = widget.session.client;
    if (client == null || _artifactBusy) return;
    setState(() { _artifactBusy = true; _actionError = null; });
    try {
      final directory = await _exporter.downloadDirectory();
      final downloaded = await ArtifactDownloader(client).download(artifact, directory);
      if (!mounted) return;
      if (!await _confirmExecutable(artifact)) return;
      if (!mounted) return;
      final choice = await showModalBottomSheet<String>(context: context, builder: (context) => SafeArea(child: Wrap(children: [
        ListTile(leading: const Icon(Icons.save_alt), title: const Text('Save / export…'), onTap: () => Navigator.pop(context, 'save')),
        ListTile(leading: const Icon(Icons.share), title: const Text('Share…'), onTap: () => Navigator.pop(context, 'share')),
        ListTile(leading: const Icon(Icons.folder_open), title: const Text('Keep in app cache only'), onTap: () => Navigator.pop(context, 'private')),
      ])));
      if (!mounted || choice == null) return;
      if (choice == 'save') {
        final path = await _exporter.saveAs(downloaded);
        if (mounted && path != null) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Artifact exported.')));
      } else if (choice == 'share') {
        await _exporter.share(downloaded);
      } else if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Verified artifact kept in the private app cache.')));
      }
    } catch (e) {
      if (mounted) setState(() => _actionError = _safeError(e));
    } finally { if (mounted) setState(() => _artifactBusy = false); }
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _follower,
      builder: (context, _) {
        final task = _follower.task ?? widget.task;
        final question = task.question;
        return Scaffold(
          appBar: AppBar(
            title: const Text('Remote task workspace'),
            actions: [
              IconButton(tooltip: 'Refresh task/artifacts', onPressed: () async { await _follower.refreshTask(); await _follower.refreshArtifacts(); }, icon: const Icon(Icons.refresh)),
            ],
          ),
          body: LayoutBuilder(builder: (context, constraints) {
            final wide = constraints.maxWidth >= 1200;
            final details = ListView(padding: const EdgeInsets.all(14), children: [
              Card(child: Padding(padding: const EdgeInsets.all(14), child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(task.title, style: Theme.of(context).textTheme.titleLarge),
                const SizedBox(height: 6),
                Text('${TaskStates.label(task.state)} · node ${task.nodeId} · priority ${task.priority}'),
                if (task.resultSummary != null && task.resultSummary!.isNotEmpty) ...[const Divider(), const Text('Result', style: TextStyle(fontWeight: FontWeight.bold)), SelectableText(_limitText(task.resultSummary!))],
                if (task.error != null && task.error!.isNotEmpty) _InlineNotice(text: _limitText(task.error!), isError: true),
                if (question != null && task.waitingInput) ...[
                  const Divider(), Text(question.text, style: Theme.of(context).textTheme.titleMedium),
                  if (widget.session.canOperate) Align(alignment: Alignment.centerRight, child: FilledButton.icon(onPressed: _commandBusy ? null : () => _answerDialog(task), icon: const Icon(Icons.question_answer), label: const Text('Answer'))),
                ],
                if (widget.session.canOperate) Wrap(spacing: 8, runSpacing: 8, children: [
                  if (!task.isTerminal) OutlinedButton.icon(onPressed: _commandBusy ? null : () => _command('cancel'), icon: const Icon(Icons.cancel_outlined), label: const Text('Cancel')),
                  if (task.canRetry) FilledButton.icon(onPressed: _commandBusy ? null : () => _command('retry'), icon: const Icon(Icons.replay), label: const Text('Retry')),
                ]),
                if (!widget.session.canOperate) const Text('Read-only workspace role.'),
                if (_actionError != null) _InlineNotice(text: _actionError!, isError: true),
              ]))),
              if (!wide) Row(children: [Expanded(child: Text('Project files', style: Theme.of(context).textTheme.titleLarge)), IconButton(onPressed: _follower.refreshArtifacts, icon: const Icon(Icons.refresh))]),
              if (!wide && _follower.artifacts.isEmpty) const Padding(padding: EdgeInsets.symmetric(vertical: 8), child: Text('No artifacts available yet.')),
              if (!wide) for (final artifact in _follower.artifacts) Card(child: ListTile(
                selected: artifact.artifactId == _selectedArtifact?.artifactId,
                leading: Icon(artifact.mime.startsWith('image/') ? Icons.image_outlined : Icons.insert_drive_file_outlined),
                title: Text(artifact.name, maxLines: 2, overflow: TextOverflow.ellipsis),
                subtitle: Text('${artifact.state} · ${_humanSize(artifact.size)} · SHA-256 ${_shortHash(artifact.sha256)}'),
                onTap: artifact.available && !_artifactBusy ? () => _openArtifact(artifact) : null,
                trailing: IconButton(tooltip: 'Verify and export', onPressed: artifact.available && !_artifactBusy ? () => _download(artifact) : null,
                  icon: _artifactBusy ? const SizedBox.square(dimension: 18, child: CircularProgressIndicator(strokeWidth: 2)) : const Icon(Icons.download)),
              )),
              Row(children: [Expanded(child: Text('Event timeline', style: Theme.of(context).textTheme.titleLarge)), Text(_follower.status.name, style: Theme.of(context).textTheme.bodySmall)]),
              if (_follower.error != null) _InlineNotice(text: _follower.error!, isError: _follower.status == FollowStatus.permissionDenied || _follower.status == FollowStatus.unauthorized),
              for (final event in _follower.events.reversed) Card(child: ListTile(
                dense: true,
                leading: const Icon(Icons.fiber_manual_record, size: 12),
                title: Text(event.type.replaceAll('_', ' ')),
                subtitle: Text(event.summary.isEmpty ? 'Event ${event.eventId}' : _limitText(event.summary)),
                trailing: Text('#${event.eventId}', style: Theme.of(context).textTheme.bodySmall),
              )),
              if (_follower.events.isEmpty) const Padding(padding: EdgeInsets.symmetric(vertical: 18), child: Center(child: Text('Waiting for task events…'))),
            ]);
            if (wide) {
              return Row(children: [
                SizedBox(width: 238, child: _taskSidebar()),
                const VerticalDivider(width: 1),
                Expanded(flex: 3, child: details),
                const VerticalDivider(width: 1),
                Expanded(flex: 2, child: Padding(padding: const EdgeInsets.all(12), child: _workspacePane())),
              ]);
            }
            return details;
          }),
        );
      },
    );
  }
}

class _InlineNotice extends StatelessWidget {
  const _InlineNotice({required this.text, required this.isError});
  final String text;
  final bool isError;
  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Container(
      margin: const EdgeInsets.symmetric(vertical: 6),
      padding: const EdgeInsets.all(10),
      decoration: BoxDecoration(
        color: isError ? theme.colorScheme.errorContainer : theme.colorScheme.surfaceContainerHighest,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Text(text, style: TextStyle(color: isError ? theme.colorScheme.onErrorContainer : theme.colorScheme.onSurfaceVariant)),
    );
  }
}

IconData _taskIcon(String state) => switch (state) {
  'waiting_input' => Icons.help_outline,
  'failed' || 'delivery_failed' => Icons.error_outline,
  'completed' || 'completed_without_model' => Icons.check_circle_outline,
  'cancelled' => Icons.cancel_outlined,
  'running' => Icons.play_circle_outline,
  _ => Icons.pending_actions,
};

String _shortHash(String hash) {
  if (hash.isEmpty) return 'missing';
  return '${hash.substring(0, hash.length < 12 ? hash.length : 12)}…';
}

String _humanSize(int bytes) {
  if (bytes < 1024) return '$bytes B';
  if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} KiB';
  return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MiB';
}

String _limitText(String text, {int max = 4000}) => text.length <= max ? text : '${text.substring(0, max)}…';

String _safeError(Object error) {
  if (error is ApiException) return error.userMessage;
  final safe = redact(error.toString()).replaceAll(RegExp(r'\s+'), ' ').trim();
  return safe.length > 500 ? safe.substring(0, 500) : safe;
}
