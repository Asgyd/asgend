import 'package:flutter/material.dart';

import '../../api/core_client.dart';
import '../../api/endpoint_policy.dart';
import '../../api/voice_gateway_client.dart';
import '../../storage/settings.dart';
import '../../util/redact.dart';
import '../app_environment.dart';
import 'cowork_page.dart';
import 'local_config_pages.dart';
import 'local_tasks.dart';

/// Local/desktop core mode (raw line-delimited JSON TCP, docs/PROTOCOL.md).
class LocalHome extends StatefulWidget {
  const LocalHome({super.key, required this.env, required this.onSwitchMode});
  final AppEnvironment env;
  final VoidCallback onSwitchMode;
  @override
  State<LocalHome> createState() => _LocalHomeState();
}

class _LocalHomeState extends State<LocalHome> {
  LocalCoreSettings? _settings;
  bool _connected = false;
  String? _connectionError;

  CoreClient? get _client {
    final s = _settings;
    if (s == null || !s.configured) return null;
    if (widget.env.isAndroid && !s.tcpRiskAcknowledged) return null;
    return CoreClient(host: s.host, port: s.port, token: s.token);
  }

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final s = await widget.env.settings.localCore(defaultHost: widget.env.isAndroid ? '' : '127.0.0.1');
    AppLog.registerSecret(s.token);
    AppLog.registerSecret(s.voiceGatewayToken);
    if (!mounted) return;
    setState(() => _settings = s);
    if (_client == null) {
      await _editConnection();
    } else {
      await _checkConnection();
    }
  }

  Future<void> _checkConnection() async {
    final client = _client;
    if (client == null) return;
    try {
      await client.rpc({'op': 'ping'}, timeout: const Duration(seconds: 5));
      if (mounted) {
        setState(() {
          _connected = true;
          _connectionError = null;
        });
      }
    } catch (error) {
      if (mounted) {
        setState(() {
          _connected = false;
          _connectionError = safeError(error, [client.token]);
        });
      }
    }
  }

  Future<void> _editConnection() async {
    final current = _settings ?? const LocalCoreSettings(host: '', port: 8765, token: '');
    final result = await showDialog<LocalCoreSettings>(
      context: context,
      builder: (_) => _LocalConnectionDialog(initial: current, isAndroid: widget.env.isAndroid),
    );
    if (result == null) return;
    await widget.env.settings.saveLocalCore(result);
    await widget.env.settings.setMode(ConnectionMode.local);
    AppLog.registerSecret(result.token);
    AppLog.registerSecret(result.voiceGatewayToken);
    if (!mounted) return;
    setState(() => _settings = result);
    await _checkConnection();
  }

  @override
  Widget build(BuildContext context) {
    final s = _settings;
    if (s == null) return const Scaffold(body: Center(child: CircularProgressIndicator()));
    final client = _client;
    return DefaultTabController(
      length: 5,
      child: Scaffold(
        appBar: AppBar(
          title: const Row(mainAxisSize: MainAxisSize.min, children: [Icon(Icons.hub_outlined), SizedBox(width: 10), Text('ASGEND · local core')]),
          actions: [
            Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                child: Center(
                    child: Tooltip(
                  message: _connectionError ?? 'Connected to ${s.host}:${s.port}',
                  child: Icon(_connected ? Icons.cloud_done_outlined : Icons.cloud_off_outlined,
                      color: _connected ? Colors.green : Theme.of(context).colorScheme.error),
                ))),
            IconButton(tooltip: 'Connection settings', onPressed: _editConnection, icon: const Icon(Icons.settings_ethernet)),
            IconButton(tooltip: 'Check connection', onPressed: _checkConnection, icon: const Icon(Icons.refresh)),
            IconButton(tooltip: 'Switch to Control Plane (HTTPS)', onPressed: widget.onSwitchMode, icon: const Icon(Icons.swap_horiz)),
          ],
          bottom: const TabBar(isScrollable: true, tabs: [
            Tab(icon: Icon(Icons.forum_outlined), text: 'Tasks'),
            Tab(icon: Icon(Icons.hub_outlined), text: 'Nodes'),
            Tab(icon: Icon(Icons.groups_outlined), text: 'Agents'),
            Tab(icon: Icon(Icons.library_books_outlined), text: 'Resources'),
            Tab(icon: Icon(Icons.forum_outlined), text: 'Cowork'),
          ]),
        ),
        body: client == null
            ? Center(
                child: Padding(
                    padding: const EdgeInsets.all(24),
                    child: Column(mainAxisSize: MainAxisSize.min, children: [
                      const Text('The local core connection is not configured.'),
                      const SizedBox(height: 12),
                      FilledButton(onPressed: _editConnection, child: const Text('Configure')),
                    ])))
            : TabBarView(children: [
                TaskDashboard(
                    client: client,
                    onConnectionError: (e) => setState(() {
                          _connected = false;
                          _connectionError = e;
                        })),
                NodesPage(client: client),
                AgentsPage(client: client),
                ResourcesPage(client: client),
                CoworkPage(coreClient: client, settings: s, isAndroid: widget.env.isAndroid, onConfigure: _editConnection),
              ]),
        bottomNavigationBar: _connected
            ? null
            : Material(
                color: Theme.of(context).colorScheme.errorContainer,
                child: Padding(
                    padding: const EdgeInsets.all(8),
                    child: Row(children: [
                      const Icon(Icons.warning_amber_rounded),
                      const SizedBox(width: 8),
                      Expanded(
                          child: Text(_connectionError ?? 'Not connected to the core', maxLines: 2, overflow: TextOverflow.ellipsis)),
                      TextButton(onPressed: _editConnection, child: const Text('Configure')),
                    ])),
              ),
      ),
    );
  }
}

class _LocalConnectionDialog extends StatefulWidget {
  const _LocalConnectionDialog({required this.initial, required this.isAndroid});
  final LocalCoreSettings initial;
  final bool isAndroid;
  @override
  State<_LocalConnectionDialog> createState() => _LocalConnectionDialogState();
}

class _LocalConnectionDialogState extends State<_LocalConnectionDialog> {
  late final TextEditingController _host = TextEditingController(text: widget.initial.host);
  late final TextEditingController _port = TextEditingController(text: '${widget.initial.port}');
  late final TextEditingController _token = TextEditingController(text: widget.initial.token);
  late final TextEditingController _voiceUrl = TextEditingController(
      text: widget.initial.voiceGatewayUrl.isNotEmpty ? widget.initial.voiceGatewayUrl : (widget.isAndroid ? '' : 'http://127.0.0.1:8788'));
  late final TextEditingController _voiceToken = TextEditingController(text: widget.initial.voiceGatewayToken);
  late bool _ack = widget.initial.tcpRiskAcknowledged;
  String? _error;

  @override
  void dispose() {
    _host.dispose();
    _port.dispose();
    _token.dispose();
    _voiceUrl.dispose();
    _voiceToken.dispose();
    super.dispose();
  }

  void _save() {
    final host = _host.text.trim();
    final port = int.tryParse(_port.text.trim());
    if (host.isEmpty || port == null || port < 1 || port > 65535) {
      setState(() => _error = 'Enter a host and a TCP port 1..65535.');
      return;
    }
    if (widget.isAndroid && EndpointPolicy.isLoopbackHost(host)) {
      setState(() => _error = '127.0.0.1/localhost is this phone, not the core host. Use the core\'s VPN/LAN address, '
          'or better, the Control Plane (HTTPS) mode.');
      return;
    }
    if (widget.isAndroid && !_ack) {
      setState(() => _error = 'Confirm that the core is reached through a VPN or SSH tunnel.');
      return;
    }
    final voiceUrl = _voiceUrl.text.trim();
    final voiceToken = _voiceToken.text.trim();
    if (voiceToken.isNotEmpty && voiceUrl.isEmpty) {
      setState(() => _error = 'Set the voice gateway URL together with ASGEND_UI_TOKEN.');
      return;
    }
    if (voiceUrl.isNotEmpty) {
      final voiceError = VoiceGatewayClient.validateEndpoint(voiceUrl, isAndroid: widget.isAndroid);
      if (voiceError != null) {
        setState(() => _error = voiceError);
        return;
      }
    }
    Navigator.pop(context, LocalCoreSettings(
      host: host, port: port, token: _token.text, tcpRiskAcknowledged: _ack,
      voiceGatewayUrl: voiceUrl, voiceGatewayToken: voiceToken,
    ));
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('Connection to ASGEND core'),
      content: SizedBox(
        width: 460,
        child: SingleChildScrollView(
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            TextField(controller: _host, decoration: const InputDecoration(labelText: 'Host / IP address')),
            TextField(controller: _port, keyboardType: TextInputType.number, decoration: const InputDecoration(labelText: 'TCP port')),
            TextField(controller: _token, obscureText: true, decoration: const InputDecoration(labelText: 'Core auth token (stored in secure storage)')),
            const Divider(height: 24),
            Align(alignment: Alignment.centerLeft, child: Text('Cowork voice gateway', style: Theme.of(context).textTheme.titleMedium)),
            TextField(controller: _voiceUrl, keyboardType: TextInputType.url, decoration: const InputDecoration(labelText: 'Gateway base URL', hintText: 'http://127.0.0.1:8788 on desktop; HTTPS URL on Android')),
            TextField(controller: _voiceToken, obscureText: true, decoration: const InputDecoration(labelText: 'ASGEND_UI_TOKEN (stored in secure storage)')),
            const SizedBox(height: 8),
            const Text('The gateway token is separate from the core TCP token. Android requires HTTPS; expose the loopback-only gateway through a secured reverse proxy or VPN endpoint.', style: TextStyle(fontSize: 12)),
            const SizedBox(height: 8),
            const Text(
                'The core TCP API is unencrypted. Never expose it to the Internet. For remote access use the Control Plane '
                '(HTTPS) mode; raw TCP only through WireGuard/VPN or an SSH tunnel.',
                style: TextStyle(fontSize: 12)),
            if (widget.isAndroid)
              CheckboxListTile(
                contentPadding: EdgeInsets.zero,
                value: _ack,
                onChanged: (v) => setState(() => _ack = v ?? false),
                title: const Text('I reach the core through a VPN/SSH tunnel', style: TextStyle(fontSize: 13)),
              ),
            if (_error != null)
              Padding(padding: const EdgeInsets.only(top: 8), child: Text(_error!, style: TextStyle(color: Theme.of(context).colorScheme.error))),
          ]),
        ),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(onPressed: _save, child: const Text('Save and connect')),
      ],
    );
  }
}
