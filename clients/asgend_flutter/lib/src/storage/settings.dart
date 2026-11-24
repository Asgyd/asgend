import 'dart:convert';

import '../models/json.dart';
import '../models/remote_models.dart';
import 'stores.dart';

enum ConnectionMode { local, remote }

class LocalCoreSettings {
  const LocalCoreSettings({
    required this.host,
    required this.port,
    required this.token,
    this.tcpRiskAcknowledged = false,
    this.voiceGatewayUrl = '',
    this.voiceGatewayToken = '',
  });
  final String host;
  final int port;
  final String token;

  /// Android only: the user confirmed the core is reached through VPN/tunnel.
  final bool tcpRiskAcknowledged;

  /// HTTP(S) base URL of the voice gateway. Desktop may use loopback HTTP;
  /// Android requires HTTPS because gateway requests contain audio and a token.
  final String voiceGatewayUrl;

  /// ASGEND_UI_TOKEN used by the gateway, stored separately from the core token.
  final String voiceGatewayToken;

  bool get voiceGatewayConfigured => voiceGatewayUrl.isNotEmpty && voiceGatewayToken.isNotEmpty;

  bool get configured => host.isNotEmpty && port > 0;

  @override
  String toString() => 'LocalCoreSettings($host:$port, coreToken: ${token.isEmpty ? 'none' : '[REDACTED]'}, '
      'voiceGateway: $voiceGatewayUrl, voiceToken: ${voiceGatewayToken.isEmpty ? 'none' : '[REDACTED]'})';
}

class RemoteCredentials {
  const RemoteCredentials({
    required this.endpoint,
    required this.token,
    required this.workspaceId,
    this.userId = '',
    this.role = WorkspaceRole.unknown,
  });
  final String endpoint;
  final String token;
  final String workspaceId;
  final String userId;
  final WorkspaceRole role;

  @override
  String toString() => 'RemoteCredentials($endpoint, ws: $workspaceId, role: ${role.name}, token: [REDACTED])';
}

/// Single place that knows which keys are secret. Secrets live only in the
/// [SecretStore]; everything else in [KeyValueStore].
class SettingsRepository {
  SettingsRepository({required this.prefs, required this.secrets});

  final KeyValueStore prefs;
  final SecretStore secrets;

  static const secretCpToken = 'cp_token';
  static const secretCoreToken = 'core_token';
  static const secretVoiceGatewayToken = 'voice_gateway_token';

  static const kMode = 'mode';
  static const kCpEndpoint = 'cp_endpoint';
  static const kCpWorkspace = 'cp_workspace_id';
  static const kCpUser = 'cp_user_id';
  static const kCpRole = 'cp_role';
  static const kDeviceName = 'cp_device_name';
  static const kCoreHost = 'core_host';
  static const kCorePort = 'core_port';
  static const kDevInsecureHttp = 'dev_allow_insecure_http';
  static const kLocalTcpAck = 'local_tcp_acknowledged';
  static const kVoiceGatewayUrl = 'voice_gateway_url';

  /// Keys that may appear in a settings export. Anything not listed here —
  /// including any secret and any cache — is never exported.
  static const exportableKeys = <String>[
    kMode, kCpEndpoint, kCpWorkspace, kDeviceName, kCoreHost, kCorePort, kDevInsecureHttp, kVoiceGatewayUrl,
  ];

  Future<ConnectionMode?> mode() async => switch (await prefs.getString(kMode)) {
        'local' => ConnectionMode.local,
        'remote' => ConnectionMode.remote,
        _ => null,
      };

  Future<void> setMode(ConnectionMode mode) => prefs.setString(kMode, mode.name);

  /// One-time migration from the legacy `core_token` preference into the
  /// platform secret store. The old value is removed only after a successful
  /// secure write; storage failures propagate so callers never silently fall
  /// back to plaintext persistence.
  Future<String> migrateLegacyCoreToken({
    String? legacyToken,
    Future<void> Function()? removeLegacyPreference,
  }) async {
    final secure = await secrets.read(secretCoreToken);
    // Callers migrating from SharedPreferences (the pre-Async API) pass its
    // value explicitly, since SharedPreferencesAsync can use a different
    // Android DataStore backend. Tests/new callers may use this repository's
    // normal preference backend by omitting [legacyToken].
    final legacy = legacyToken ?? await prefs.getString(secretCoreToken);
    Future<void> removeLegacy() => removeLegacyPreference == null
        ? prefs.remove(secretCoreToken)
        : removeLegacyPreference();
    if (secure != null && secure.isNotEmpty) {
      if (legacy != null) await removeLegacy();
      return secure;
    }
    if (legacy == null || legacy.isEmpty) {
      if (legacy != null) await removeLegacy();
      return secure ?? '';
    }
    await secrets.write(secretCoreToken, legacy);
    await removeLegacy();
    return legacy;
  }

  Future<bool> devAllowInsecureHttp() async => await prefs.getString(kDevInsecureHttp) == 'true';
  Future<void> setDevAllowInsecureHttp(bool value) => prefs.setString(kDevInsecureHttp, '$value');

  Future<String> deviceName() async => await prefs.getString(kDeviceName) ?? '';
  Future<void> setDeviceName(String value) => prefs.setString(kDeviceName, value);

  /// [defaultHost] is `127.0.0.1` on desktop and empty on Android (a phone's
  /// loopback is never the core host).
  Future<LocalCoreSettings> localCore({required String defaultHost}) async {
    return LocalCoreSettings(
      host: await prefs.getString(kCoreHost) ?? defaultHost,
      port: int.tryParse(await prefs.getString(kCorePort) ?? '') ?? 8765,
      token: await secrets.read(secretCoreToken) ?? '',
      tcpRiskAcknowledged: await prefs.getString(kLocalTcpAck) == 'true',
      voiceGatewayUrl: await prefs.getString(kVoiceGatewayUrl) ?? '',
      voiceGatewayToken: await secrets.read(secretVoiceGatewayToken) ?? '',
    );
  }

  Future<void> saveLocalCore(LocalCoreSettings s) async {
    await prefs.setString(kCoreHost, s.host);
    await prefs.setString(kCorePort, '${s.port}');
    await prefs.setString(kLocalTcpAck, '${s.tcpRiskAcknowledged}');
    await prefs.setString(kVoiceGatewayUrl, s.voiceGatewayUrl.trim());
    if (s.voiceGatewayToken.isEmpty) {
      await secrets.delete(secretVoiceGatewayToken);
    } else {
      await secrets.write(secretVoiceGatewayToken, s.voiceGatewayToken);
    }
    if (s.token.isEmpty) {
      await secrets.delete(secretCoreToken);
    } else {
      await secrets.write(secretCoreToken, s.token);
    }
  }

  Future<RemoteCredentials?> remoteCredentials() async {
    final token = await secrets.read(secretCpToken);
    final endpoint = await prefs.getString(kCpEndpoint);
    final ws = await prefs.getString(kCpWorkspace);
    if (token == null || token.isEmpty || endpoint == null || endpoint.isEmpty || ws == null || ws.isEmpty) return null;
    return RemoteCredentials(
      endpoint: endpoint,
      token: token,
      workspaceId: ws,
      userId: await prefs.getString(kCpUser) ?? '',
      role: WorkspaceRole.parse(await prefs.getString(kCpRole)),
    );
  }

  Future<String> lastEndpoint() async => await prefs.getString(kCpEndpoint) ?? '';

  Future<void> saveRemoteCredentials(RemoteCredentials c) async {
    await secrets.write(secretCpToken, c.token);
    await prefs.setString(kCpEndpoint, c.endpoint);
    await prefs.setString(kCpWorkspace, c.workspaceId);
    await prefs.setString(kCpUser, c.userId);
    await prefs.setString(kCpRole, c.role.name);
  }

  Future<void> setActiveWorkspace(String workspaceId, WorkspaceRole role) async {
    await prefs.setString(kCpWorkspace, workspaceId);
    await prefs.setString(kCpRole, role.name);
  }

  /// Logout / revoked token: removes the credential and every cached remote
  /// object (tasks, events, cursors). The endpoint is kept to ease re-pairing.
  Future<void> clearRemoteCredentials() async {
    await secrets.delete(secretCpToken);
    await prefs.remove(kCpWorkspace);
    await prefs.remove(kCpUser);
    await prefs.remove(kCpRole);
    for (final key in await prefs.keys()) {
      if (key.startsWith('cache.')) await prefs.remove(key);
    }
  }

  /// Settings export (JSON). Contains only [exportableKeys]; secrets are
  /// structurally impossible to include.
  Future<String> exportSettings() async {
    final out = <String, dynamic>{'format': 'asgend-client-settings', 'version': 1};
    for (final key in exportableKeys) {
      final value = await prefs.getString(key);
      if (value != null) out[key] = value;
    }
    return const JsonEncoder.withIndent('  ').convert(out);
  }

  /// Imports an export produced by [exportSettings]; unknown keys and any key
  /// outside [exportableKeys] are ignored.
  Future<void> importSettings(String json) async {
    final map = asMap(jsonDecode(json));
    if (map['format'] != 'asgend-client-settings') throw const FormatException('Not an ASGEND settings export.');
    for (final key in exportableKeys) {
      final value = map[key];
      if (value is String) await prefs.setString(key, value);
    }
  }
}
