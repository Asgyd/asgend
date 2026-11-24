// Local/desktop core mode: resources, nodes/models and agent profiles.
import 'dart:convert';

import 'package:flutter/material.dart';

import '../../api/core_client.dart';
import '../../models/json.dart';
import '../../util/redact.dart';

String _err(Object e, CoreClient c) => safeError(e, [c.token]);

Map<String, dynamic> _deepCopy(Map<String, dynamic> m) => asMap(jsonDecode(jsonEncode(m)));

class ResourcesPage extends StatefulWidget {
  const ResourcesPage({super.key, required this.client});
  final CoreClient client;
  @override
  State<ResourcesPage> createState() => _ResourcesPageState();
}

class _ResourcesPageState extends State<ResourcesPage> {
  Map<String, dynamic> _catalog = {};
  bool _loading = true;
  String? _error;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  Future<void> _refresh() async {
    try {
      final result = await widget.client.rpc({'op': 'resources'}, timeout: const Duration(seconds: 10));
      if (mounted) {
        setState(() {
          _catalog = result;
          _loading = false;
          _error = null;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = _err(error, widget.client);
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final files = asMap(_catalog['files']);
    final dirs = asMap(_catalog['directories']);
    const categories = <String, String>{
      'skills': 'Skills',
      'system_prompts': 'System prompts',
      'tools': 'Tools and MCP descriptions',
      'knowledge': 'Knowledge',
      'instructions': 'Decision instructions',
    };
    if (_loading) return const Center(child: CircularProgressIndicator());
    return RefreshIndicator(
      onRefresh: _refresh,
      child: ListView(padding: const EdgeInsets.all(16), children: [
        Row(children: [
          Expanded(child: Text('Live resource catalogue', style: Theme.of(context).textTheme.headlineSmall)),
          IconButton(onPressed: _refresh, icon: const Icon(Icons.refresh)),
        ]),
        const Text('Skills, system prompts, tools and knowledge are synced into a separate local BM25 index automatically. '
            'Project files use a separate project-memory index.'),
        if (_error != null)
          Padding(padding: const EdgeInsets.all(8), child: Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error))),
        Card(
            child: ListTile(
                leading: const Icon(Icons.hub_outlined),
                title: const Text('Resource retrieval'),
                subtitle: Text(_catalog['selection_mode'] == 'local_bm25'
                    ? 'Local BM25 · automatic index sync'
                    : asString(_catalog['decision_model_id'], '(not configured)')),
                trailing: Icon((_catalog['resource_index_auto_sync'] == true) ? Icons.sync : Icons.warning_amber))),
        Card(
            child: ListTile(
                leading: const Icon(Icons.folder_open_outlined),
                title: const Text('Project memory'),
                subtitle: Text('${asString(_catalog['project_memory_root'], '(not configured)')}\n'
                    '${asString(_catalog['project_memory_index_path'], 'index not created yet')}'),
                isThreeLine: true)),
        for (final entry in categories.entries)
          Card(
              child: ExpansionTile(
            leading: Icon(switch (entry.key) {
              'skills' => Icons.psychology_outlined,
              'tools' => Icons.build_outlined,
              'knowledge' => Icons.menu_book_outlined,
              _ => Icons.description_outlined,
            }),
            title: Text(entry.value),
            subtitle: Text('${asList(files[entry.key]).length} files · ${asString(dirs[entry.key], 'directory not reported')}'),
            children: [
              for (final file in asList(files[entry.key]))
                ListTile(dense: true, leading: const Icon(Icons.insert_drive_file_outlined), title: Text('$file', softWrap: true)),
              if (asList(files[entry.key]).isEmpty) const ListTile(title: Text('No Markdown files discovered.')),
            ],
          )),
      ]),
    );
  }
}

class NodesPage extends StatefulWidget {
  const NodesPage({super.key, required this.client});
  final CoreClient client;
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
  void initState() {
    super.initState();
    _refresh();
  }

  Future<void> _refresh() async {
    try {
      final results = await Future.wait([
        widget.client.rpc({'op': 'config_get'}),
        widget.client.rpc({'op': 'models'}),
      ]);
      if (!mounted) return;
      setState(() {
        _configs = results[0];
        _modelStatus = asMap(results[1]['model_status']);
        _resources = asMap(results[1]['resources']);
        _loading = false;
        _error = null;
      });
    } catch (error) {
      if (mounted) {
        setState(() {
          _loading = false;
          _error = _err(error, widget.client);
        });
      }
    }
  }

  Future<void> _saveConfigs(Map<String, dynamic> configs) async {
    setState(() {
      _saving = true;
      _error = null;
    });
    try {
      final result = await widget.client
          .rpc({'op': 'config_save', 'main': configs['main'], 'models': configs['models'], 'agents': configs['agents']});
      if (mounted) {
        ScaffoldMessenger.of(context)
            .showSnackBar(SnackBar(content: Text(asString(result['message'], 'Configuration saved; restart may be required.'))));
        await _refresh();
      }
    } catch (error) {
      if (mounted) setState(() => _error = _err(error, widget.client));
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  Future<void> _changeModel(Map<String, dynamic> model, bool decision, String operation) async {
    final id = asString(model['id']);
    try {
      await widget.client.rpc({'op': operation, 'model_id': id, 'kind': decision ? 'decision' : 'llm'});
      await _refresh();
    } catch (error) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(_err(error, widget.client))));
    }
  }

  Future<void> _toggleNode(Map<String, dynamic> node, bool enabled) async {
    final configs = _deepCopy(_configs);
    final modelsRoot = asMap(configs['models']);
    final nodeList = asList(modelsRoot['nodes']);
    final nodeId = asString(node['id']);
    if (!enabled) {
      for (final raw in [
        ...asList(modelsRoot['models']).map((e) => {'row': e, 'decision': false}),
        ...asList(modelsRoot['decision_models']).map((e) => {'row': e, 'decision': true}),
      ]) {
        final model = asMap(raw['row']);
        if (asString(model['node_id']) == nodeId) {
          model['enabled'] = false;
          await _changeModel(model, raw['decision'] == true, 'model_disable');
        }
      }
    }
    final target = nodeList.map(asMap).firstWhere((row) => asString(row['id']) == nodeId, orElse: () => <String, dynamic>{});
    if (target.isEmpty) return;
    target['enabled'] = enabled;
    await _saveConfigs(configs);
  }

  Future<void> _addNode() async {
    final result = await showDialog<Map<String, dynamic>>(context: context, builder: (_) => const _NodeEditor());
    if (result == null) return;
    final configs = _deepCopy(_configs);
    final modelsRoot = asMap(configs['models']);
    final nodes = [...asList(modelsRoot['nodes'])];
    if (nodes.any((raw) => asString(asMap(raw)['id']) == result['id'])) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('A node with this ID already exists.')));
      return;
    }
    nodes.add(result);
    modelsRoot['nodes'] = nodes;
    configs['models'] = modelsRoot;
    await _saveConfigs(configs);
  }

  Future<void> _addModel(bool decision) async {
    final nodes = asList(asMap(_configs['models'])['nodes']).map(asMap).toList();
    final result =
        await showDialog<Map<String, dynamic>>(context: context, builder: (_) => _ModelEditor(nodes: nodes, decision: decision));
    if (result == null) return;
    final configs = _deepCopy(_configs);
    final modelsRoot = asMap(configs['models']);
    final key = decision ? 'decision_models' : 'models';
    final list = [...asList(modelsRoot[key])];
    if (asList(modelsRoot['models']).any((row) => asString(asMap(row)['id']) == result['id']) ||
        asList(modelsRoot['decision_models']).any((row) => asString(asMap(row)['id']) == result['id'])) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Model ID already exists.')));
      return;
    }
    list.add(result);
    modelsRoot[key] = list;
    configs['models'] = modelsRoot;
    await _saveConfigs(configs);
  }

  @override
  Widget build(BuildContext context) {
    final regular = asList(asMap(_configs['models'])['models']).map(asMap).toList();
    final decisions = asList(asMap(_configs['models'])['decision_models']).map(asMap).toList();
    final nodes = asList(asMap(_configs['models'])['nodes']).map(asMap).toList();
    final runtimeModels = asList(_modelStatus['models']).map(asMap).toList();
    final runtimeDecisions = asList(_modelStatus['decision_models']).map(asMap).toList();
    Map<String, dynamic> withRuntime(Map<String, dynamic> model, bool decision) {
      final statuses = decision ? runtimeDecisions : runtimeModels;
      final id = asString(model['id']);
      Map<String, dynamic>? runtime;
      for (final row in statuses) {
        if (asString(row['id']) == id) {
          runtime = row;
          break;
        }
      }
      return {...model, 'running': runtime?['running'] == true};
    }

    if (_loading) return const Center(child: CircularProgressIndicator());
    return RefreshIndicator(
        onRefresh: _refresh,
        child: ListView(padding: const EdgeInsets.all(16), children: [
          Row(children: [
            Expanded(child: Text('Nodes and models', style: Theme.of(context).textTheme.headlineSmall)),
            IconButton(onPressed: _refresh, icon: const Icon(Icons.refresh)),
          ]),
          if (_error != null)
            Padding(padding: const EdgeInsets.all(8), child: Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error))),
          Card(
              child: Padding(
                  padding: const EdgeInsets.all(14),
                  child: Wrap(spacing: 20, runSpacing: 10, children: [
                    _ResourceMetric(label: 'Free RAM', value: _resourceValue(_resources['ram_available_mb'])),
                    _ResourceMetric(label: 'Free VRAM', value: _resourceValue(_resources['vram_free_mb'])),
                    _ResourceMetric(label: 'Running models', value: '${asList(_resources['running_models']).length}'),
                  ]))),
          Row(children: [
            Expanded(child: Text('Nodes', style: Theme.of(context).textTheme.titleLarge)),
            FilledButton.tonalIcon(onPressed: _saving ? null : _addNode, icon: const Icon(Icons.add), label: const Text('Add node')),
          ]),
          for (final node in nodes)
            Card(
                child: SwitchListTile(
              title: Text(asString(node['name'], asString(node['id']))),
              subtitle: Text('${asString(node['type'])} · ${asString(node['endpoint'], 'local runtime')}'),
              value: node['enabled'] == true,
              onChanged: _saving ? null : (value) => _toggleNode(node, value),
            )),
          const SizedBox(height: 12),
          Row(children: [
            Expanded(child: Text('LLM models', style: Theme.of(context).textTheme.titleLarge)),
            FilledButton.tonalIcon(
                onPressed: _saving ? null : () => _addModel(false), icon: const Icon(Icons.add), label: const Text('Add LLM')),
          ]),
          for (final model in regular) _ModelCard(model: withRuntime(model, false), decision: false, onChange: _changeModel),
          Row(children: [
            Expanded(child: Text('Decision models', style: Theme.of(context).textTheme.titleLarge)),
            FilledButton.tonalIcon(
                onPressed: _saving ? null : () => _addModel(true), icon: const Icon(Icons.add), label: const Text('Add decision')),
          ]),
          for (final model in decisions) _ModelCard(model: withRuntime(model, true), decision: true, onChange: _changeModel),
        ]));
  }

  String _resourceValue(dynamic value) => value is num && value >= 0 ? '${(value / 1024).toStringAsFixed(1)} GB' : 'Unknown';
}

class _ResourceMetric extends StatelessWidget {
  const _ResourceMetric({required this.label, required this.value});
  final String label;
  final String value;
  @override
  Widget build(BuildContext context) => SizedBox(
      width: 135,
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(label, style: Theme.of(context).textTheme.labelMedium),
        Text(value, style: Theme.of(context).textTheme.titleMedium),
      ]));
}

class _ModelCard extends StatelessWidget {
  const _ModelCard({required this.model, required this.decision, required this.onChange});
  final Map<String, dynamic> model;
  final bool decision;
  final Future<void> Function(Map<String, dynamic>, bool, String) onChange;
  @override
  Widget build(BuildContext context) {
    final enabled = model['enabled'] == true;
    final running = model['running'] == true;
    return Card(
        child: Padding(
            padding: const EdgeInsets.all(10),
            child: Column(children: [
              Row(children: [
                Expanded(child: Text(asString(model['name'], asString(model['id'])), style: Theme.of(context).textTheme.titleMedium)),
                Switch(value: enabled, onChanged: (value) => onChange(model, decision, value ? 'model_enable' : 'model_disable')),
              ]),
              Align(
                  alignment: Alignment.centerLeft,
                  child: Text('${asString(model['id'])} · ${asString(model['tier'], decision ? 'decision' : 'medium')} · '
                      '${asString(model['node_id'])}')),
              Align(
                  alignment: Alignment.centerLeft,
                  child: Text('${running ? 'Running' : 'Stopped'} · RAM ${model['ram_estimate_mb'] ?? 0} MB · '
                      'VRAM ${model['vram_estimate_mb'] ?? 0} MB')),
              Row(mainAxisAlignment: MainAxisAlignment.end, children: [
                TextButton.icon(
                    onPressed: enabled ? () => onChange(model, decision, running ? 'model_stop' : 'model_start') : null,
                    icon: Icon(running ? Icons.stop : Icons.play_arrow),
                    label: Text(running ? 'Stop process' : 'Start process')),
              ]),
            ])));
  }
}

class _NodeEditor extends StatefulWidget {
  const _NodeEditor();
  @override
  State<_NodeEditor> createState() => _NodeEditorState();
}

class _NodeEditorState extends State<_NodeEditor> {
  final _id = TextEditingController();
  final _name = TextEditingController();
  final _endpoint = TextEditingController();
  String _type = 'local';
  @override
  void dispose() {
    _id.dispose();
    _name.dispose();
    _endpoint.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => AlertDialog(
          title: const Text('Add node'),
          content: SizedBox(
              width: 440,
              child: Column(mainAxisSize: MainAxisSize.min, children: [
                TextField(controller: _id, decoration: const InputDecoration(labelText: 'ID (letters, digits, _ and -)')),
                TextField(controller: _name, decoration: const InputDecoration(labelText: 'Name')),
                DropdownButtonFormField<String>(
                    value: _type,
                    decoration: const InputDecoration(labelText: 'Type'),
                    items: const [
                      DropdownMenuItem(value: 'local', child: Text('Local device')),
                      DropdownMenuItem(value: 'lan', child: Text('Local network')),
                      DropdownMenuItem(value: 'internet', child: Text('Internet (HTTPS only)')),
                    ],
                    onChanged: (v) => setState(() => _type = v ?? 'local')),
                TextField(controller: _endpoint, decoration: const InputDecoration(labelText: 'Inference endpoint (optional for local)')),
              ])),
          actions: [
            TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
            FilledButton(
                onPressed: () {
                  final id = _id.text.trim();
                  final name = _name.text.trim();
                  final endpoint = _endpoint.text.trim();
                  if (id.isEmpty ||
                      name.isEmpty ||
                      (_type == 'internet' && !endpoint.startsWith('https://')) ||
                      (_type == 'lan' && endpoint.isNotEmpty && !endpoint.startsWith('http://') && !endpoint.startsWith('https://'))) {
                    return;
                  }
                  Navigator.pop(context, {
                    'id': id, 'name': name, 'type': _type, 'enabled': true, 'endpoint': endpoint, 'health_url': '',
                    'api_key': '', 'api_key_env': '', 'description': 'Added from desktop/Android client.',
                  });
                },
                child: const Text('Add node')),
          ]);
}

class _ModelEditor extends StatefulWidget {
  const _ModelEditor({required this.nodes, required this.decision});
  final List<Map<String, dynamic>> nodes;
  final bool decision;
  @override
  State<_ModelEditor> createState() => _ModelEditorState();
}

class _ModelEditorState extends State<_ModelEditor> {
  final _id = TextEditingController();
  final _name = TextEditingController();
  final _modelName = TextEditingController();
  final _baseUrl = TextEditingController();
  final _executable = TextEditingController();
  final _modelPath = TextEditingController();
  final _args = TextEditingController(text: '-m\n{model_path}\n--host\n127.0.0.1\n--port\n{port}');
  final _port = TextEditingController(text: '8103');
  final _ram = TextEditingController(text: '0');
  final _vram = TextEditingController(text: '0');
  String _nodeId = 'local';
  String _tier = 'large';

  @override
  void initState() {
    super.initState();
    if (widget.nodes.isNotEmpty && !widget.nodes.any((n) => n['id'] == _nodeId)) _nodeId = asString(widget.nodes.first['id']);
  }

  @override
  void dispose() {
    for (final c in [_id, _name, _modelName, _baseUrl, _executable, _modelPath, _args, _port, _ram, _vram]) {
      c.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final currentNode =
        widget.nodes.firstWhere((n) => asString(n['id']) == _nodeId, orElse: () => <String, dynamic>{'type': 'local'});
    final type = asString(currentNode['type'], 'local');
    final nodeIds = widget.nodes.map((n) => asString(n['id'])).toSet();
    return AlertDialog(
        title: Text(widget.decision ? 'Add decision model' : 'Add LLM model'),
        content: SizedBox(
            width: 520,
            child: SingleChildScrollView(
                child: Column(mainAxisSize: MainAxisSize.min, children: [
              TextField(controller: _id, decoration: const InputDecoration(labelText: 'Unique model ID')),
              TextField(controller: _name, decoration: const InputDecoration(labelText: 'Display name')),
              TextField(controller: _modelName, decoration: const InputDecoration(labelText: 'Model/API name')),
              DropdownButtonFormField<String>(
                  value: nodeIds.contains(_nodeId) ? _nodeId : null,
                  decoration: const InputDecoration(labelText: 'Node'),
                  items: widget.nodes
                      .map((n) => DropdownMenuItem(value: asString(n['id']), child: Text(asString(n['name'], asString(n['id'])))))
                      .toList(),
                  onChanged: (v) => setState(() {
                        _nodeId = v ?? _nodeId;
                        final selected =
                            widget.nodes.firstWhere((n) => asString(n['id']) == _nodeId, orElse: () => <String, dynamic>{});
                        _baseUrl.text = asString(selected['endpoint']);
                      })),
              DropdownButtonFormField<String>(
                  value: _tier,
                  decoration: const InputDecoration(labelText: 'Capability tier'),
                  items: const [
                    DropdownMenuItem(value: 'large', child: Text('Large — analysis/decomposition/review')),
                    DropdownMenuItem(value: 'medium', child: Text('Medium — agent model')),
                    DropdownMenuItem(value: 'fast', child: Text('Fast — simple subtasks')),
                    DropdownMenuItem(value: 'ultra_fast', child: Text('Ultra fast — lightweight tasks')),
                  ],
                  onChanged: widget.decision ? null : (v) => setState(() => _tier = v ?? 'medium')),
              TextField(controller: _baseUrl, decoration: const InputDecoration(labelText: 'OpenAI-compatible base URL')),
              if (type == 'local') ...[
                TextField(controller: _executable, decoration: const InputDecoration(labelText: 'Executable path (optional if external service)')),
                TextField(controller: _modelPath, decoration: const InputDecoration(labelText: 'Weights directory/file (optional)')),
                TextField(controller: _args, minLines: 2, maxLines: 4, decoration: const InputDecoration(labelText: 'One launch argument per line')),
                TextField(controller: _port, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'Port')),
              ],
              Row(children: [
                Expanded(
                    child: TextField(
                        controller: _ram, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'RAM estimate, MB'))),
                const SizedBox(width: 12),
                Expanded(
                    child: TextField(
                        controller: _vram, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'VRAM estimate, MB'))),
              ]),
              const SizedBox(height: 8),
              const Text('New profiles are disabled until you verify paths and endpoints.', style: TextStyle(fontSize: 12)),
            ]))),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () {
                final id = _id.text.trim();
                final name = _name.text.trim();
                final modelName = _modelName.text.trim();
                final port = int.tryParse(_port.text) ?? 0;
                final ram = int.tryParse(_ram.text) ?? -1;
                final vram = int.tryParse(_vram.text) ?? -1;
                final baseUrl = _baseUrl.text.trim();
                if (id.isEmpty ||
                    name.isEmpty ||
                    modelName.isEmpty ||
                    baseUrl.isEmpty ||
                    ram < 0 ||
                    vram < 0 ||
                    (type == 'local' && _executable.text.trim().isNotEmpty && (port < 1 || port > 65535))) {
                  return;
                }
                final args = _args.text.split('\n').map((s) => s.trim()).where((s) => s.isNotEmpty).toList();
                Navigator.pop(context, {
                  'id': id, 'name': name, 'enabled': false, 'tier': widget.decision ? 'decision' : _tier,
                  'auto_start': false, 'start_at_boot': false, 'executable': _executable.text.trim(),
                  'model_path': _modelPath.text.trim(), 'working_directory': '.', 'args': args, 'port': port,
                  'base_url': baseUrl, 'health_url': '', 'model_name': modelName, 'api_key': '',
                  'startup_timeout_seconds': 120, 'shutdown_timeout_seconds': 10, 'request_timeout_seconds': 120,
                  'temperature': widget.decision ? 0.1 : 0.2, 'node_id': _nodeId,
                  'location_type': type == 'local' ? 'local' : 'remote', 'ram_estimate_mb': ram, 'vram_estimate_mb': vram,
                });
              },
              child: const Text('Add model')),
        ]);
  }
}

class AgentsPage extends StatefulWidget {
  const AgentsPage({super.key, required this.client});
  final CoreClient client;
  @override
  State<AgentsPage> createState() => _AgentsPageState();
}

class _AgentsPageState extends State<AgentsPage> {
  Map<String, dynamic> _configs = {};
  bool _loading = true;
  bool _saving = false;
  String? _error;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    try {
      final data = await widget.client.rpc({'op': 'config_get'});
      if (mounted) {
        setState(() {
          _configs = data;
          _loading = false;
          _error = null;
        });
      }
    } catch (e) {
      if (mounted) {
        setState(() {
          _error = _err(e, widget.client);
          _loading = false;
        });
      }
    }
  }

  Future<void> _save(Map<String, dynamic> data) async {
    setState(() {
      _saving = true;
      _error = null;
    });
    try {
      await widget.client.rpc({'op': 'config_save', 'main': data['main'], 'models': data['models'], 'agents': data['agents']});
      await _load();
      if (mounted) {
        ScaffoldMessenger.of(context)
            .showSnackBar(const SnackBar(content: Text('Execution profile saved. Restart the core to apply changes.')));
      }
    } catch (e) {
      if (mounted) setState(() => _error = _err(e, widget.client));
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  Future<void> _edit(Map<String, dynamic>? existing) async {
    final result = await showDialog<Map<String, dynamic>>(
        context: context,
        builder: (_) => _AgentEditor(existing: existing, models: asList(asMap(_configs['models'])['models']).map(asMap).toList()));
    if (result == null) return;
    final data = _deepCopy(_configs);
    final agents = [...asList(data['agents'])];
    if (existing == null && agents.any((a) => asString(asMap(a)['id']) == result['id'])) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Profile ID already exists.')));
      return;
    }
    if (existing == null) {
      agents.add(result);
    } else {
      final idx = agents.indexWhere((a) => asString(asMap(a)['id']) == asString(existing['id']));
      if (idx >= 0) agents[idx] = result;
    }
    data['agents'] = agents;
    await _save(data);
  }

  @override
  Widget build(BuildContext context) {
    final agents = asList(_configs['agents']).map(asMap).toList();
    final models = asList(asMap(_configs['models'])['models']).map(asMap).toList();
    if (_loading) return const Center(child: CircularProgressIndicator());
    return RefreshIndicator(
        onRefresh: _load,
        child: ListView(padding: const EdgeInsets.all(16), children: [
          Row(children: [
            Expanded(child: Text('Execution profiles', style: Theme.of(context).textTheme.headlineSmall)),
            FilledButton.icon(onPressed: _saving ? null : () => _edit(null), icon: const Icon(Icons.add), label: const Text('Add profile')),
          ]),
          const Padding(
              padding: EdgeInsets.only(bottom: 12),
              child: Text('Execution profiles define capabilities, permissions and instructions. The decision model selects one '
                  'profile for the whole task; independent resource classifiers choose resources for each stage.')),
          if (_error != null) Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error)),
          for (final agent in agents)
            Card(
                child: ListTile(
                    leading: const Icon(Icons.smart_toy_outlined),
                    title: Text(asString(agent['name'], asString(agent['id']))),
                    subtitle: Text('${asString(agent['id'])} · model ${asString(agent['model_id'], '(default)')}\n'
                        '${asString(agent['description'])}'),
                    isThreeLine: true,
                    trailing: IconButton(onPressed: _saving ? null : () => _edit(agent), icon: const Icon(Icons.edit_outlined)))),
          if (models.isEmpty) const Text('No regular LLM profiles are configured yet. Add them in Nodes.'),
        ]));
  }
}

class _AgentEditor extends StatefulWidget {
  const _AgentEditor({required this.existing, required this.models});
  final Map<String, dynamic>? existing;
  final List<Map<String, dynamic>> models;
  @override
  State<_AgentEditor> createState() => _AgentEditorState();
}

class _AgentEditorState extends State<_AgentEditor> {
  late final TextEditingController _id, _name, _description, _prompt, _skills, _tools;
  String _model = '';

  @override
  void initState() {
    super.initState();
    final a = widget.existing ?? <String, dynamic>{};
    _id = TextEditingController(text: asString(a['id']));
    _name = TextEditingController(text: asString(a['name']));
    _description = TextEditingController(text: asString(a['description']));
    _prompt = TextEditingController(
        text: asString(a['system_prompt'],
            'You are a flexible agent. Follow the assigned subtask, selected skills, allowed tools and supplied knowledge.'));
    _skills = TextEditingController(text: asList(a['skills']).join(', '));
    _tools = TextEditingController(text: asList(a['tools']).join(', '));
    _model = asString(a['model_id']);
  }

  @override
  void dispose() {
    for (final c in [_id, _name, _description, _prompt, _skills, _tools]) {
      c.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final modelIds = widget.models.map((m) => asString(m['id'])).toSet();
    return AlertDialog(
        title: Text(widget.existing == null ? 'Add execution profile' : 'Edit execution profile'),
        content: SizedBox(
            width: 560,
            child: SingleChildScrollView(
                child: Column(mainAxisSize: MainAxisSize.min, children: [
              TextField(controller: _id, enabled: widget.existing == null, decoration: const InputDecoration(labelText: 'Unique ID')),
              TextField(controller: _name, decoration: const InputDecoration(labelText: 'Name')),
              TextField(controller: _description, decoration: const InputDecoration(labelText: 'Description')),
              TextField(controller: _prompt, minLines: 3, maxLines: 7, decoration: const InputDecoration(labelText: 'System prompt')),
              DropdownButtonFormField<String>(
                  value: _model.isEmpty || modelIds.contains(_model) ? _model : '',
                  decoration: const InputDecoration(labelText: 'Preferred model (optional)'),
                  items: [
                    const DropdownMenuItem(value: '', child: Text('(Use default model)')),
                    ...widget.models.map((m) => DropdownMenuItem(value: asString(m['id']), child: Text(asString(m['name'], asString(m['id']))))),
                  ],
                  onChanged: (v) => setState(() => _model = v ?? '')),
              TextField(controller: _skills, decoration: const InputDecoration(labelText: 'Default skill IDs, comma-separated')),
              TextField(controller: _tools, decoration: const InputDecoration(labelText: 'Allowed tool IDs, comma-separated')),
            ]))),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () {
                final id = _id.text.trim();
                final name = _name.text.trim();
                if (id.isEmpty || name.isEmpty) return;
                List<String> split(TextEditingController c) => c.text.split(',').map((v) => v.trim()).where((v) => v.isNotEmpty).toList();
                Navigator.pop(context, {
                  'id': id, 'name': name, 'description': _description.text.trim(), 'system_prompt': _prompt.text.trim(),
                  'model_id': _model, 'skills': split(_skills), 'tools': split(_tools),
                });
              },
              child: const Text('Save agent')),
        ]);
  }
}
