import 'dart:convert';

import 'package:asgend_client/src/api/endpoint_policy.dart';
import 'package:asgend_client/src/models/remote_models.dart';
import 'package:asgend_client/src/storage/settings.dart';
import 'package:asgend_client/src/storage/stores.dart';
import 'package:asgend_client/src/util/redact.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('EndpointPolicy', () {
    test('normalizes an HTTPS endpoint and strips a terminal /v1', () {
      final uri = const EndpointPolicy().validate(' https://control.example.test/base/v1/ ');
      expect(uri.toString(), 'https://control.example.test/base');
    });

    test('rejects remote cleartext endpoints and embedded credentials', () {
      const policy = EndpointPolicy();
      expect(() => policy.validate('http://control.example.test'), throwsA(isA<Exception>()));
      expect(() => policy.validate('https://user:pass@control.example.test'), throwsA(isA<Exception>()));
      expect(() => policy.validate('https://control.example.test/?token=secret'), throwsA(isA<Exception>()));
    });

    test('permits explicit loopback HTTP for local test servers only', () {
      final uri = const EndpointPolicy().validate('http://127.0.0.1:8080');
      expect(uri.scheme, 'http');
      expect(uri.host, '127.0.0.1');
      expect(() => const EndpointPolicy().validate('http://192.168.1.20:8080'), throwsA(isA<Exception>()));
    });

    test('loopback detection rejects malformed IPv4 octets', () {
      expect(EndpointPolicy.isLoopbackHost('127.10.20.30'), isTrue);
      expect(EndpointPolicy.isLoopbackHost('127.256.20.30'), isFalse);
      expect(EndpointPolicy.isLoopbackHost('127.1.example.30'), isFalse);
    });

    test('pairing link parses endpoint/code and redacts the one-time code in diagnostics', () {
      final link = PairingLink.tryParse(
        'asgend-pair://pair?endpoint=https%3A%2F%2Fcontrol.example.test&code=ONE-TIME-SECRET',
      );
      expect(link, isNotNull);
      expect(link!.endpoint, 'https://control.example.test');
      expect(link.code, 'ONE-TIME-SECRET');
      expect(link.toString(), isNot(contains('ONE-TIME-SECRET')));
      expect(PairingLink.tryParse('https://control.example.test'), isNull);
    });

    test('download references cannot escape the configured origin', () {
      const policy = EndpointPolicy();
      final base = policy.validate('https://control.example.test/gateway');
      expect(
        () => policy.resolveSameOrigin(base, 'https://attacker.example/file'),
        throwsA(isA<Exception>()),
      );
    });
  });

  group('credential and settings storage', () {
    late MemoryKeyValueStore prefs;
    late MemorySecretStore secrets;
    late SettingsRepository settings;

    setUp(() {
      prefs = MemoryKeyValueStore();
      secrets = MemorySecretStore();
      settings = SettingsRepository(prefs: prefs, secrets: secrets);
    });

    test('legacy core token is migrated to secure storage then removed from preferences', () async {
      await prefs.setString(SettingsRepository.secretCoreToken, 'legacy-core-secret');
      final migrated = await settings.migrateLegacyCoreToken();
      expect(migrated, 'legacy-core-secret');
      expect(await secrets.read(SettingsRepository.secretCoreToken), 'legacy-core-secret');
      expect(await prefs.getString(SettingsRepository.secretCoreToken), isNull);
    });

    test('explicit legacy plugin value is removed only after secure write', () async {
      var legacyRemoved = false;
      final migrated = await settings.migrateLegacyCoreToken(
        legacyToken: 'legacy-shared-preferences-secret',
        removeLegacyPreference: () async {
          // The callback is allowed only after the secure write has completed.
          expect(await secrets.read(SettingsRepository.secretCoreToken), 'legacy-shared-preferences-secret');
          legacyRemoved = true;
        },
      );
      expect(migrated, 'legacy-shared-preferences-secret');
      expect(legacyRemoved, isTrue);
    });

    test('a secure-store write failure leaves legacy token intact for retry', () async {
      final failing = _FailingSecretStore();
      final failingSettings = SettingsRepository(prefs: prefs, secrets: failing);
      var legacyRemoved = false;
      await expectLater(
        failingSettings.migrateLegacyCoreToken(
          legacyToken: 'legacy-core-secret',
          removeLegacyPreference: () async { legacyRemoved = true; },
        ),
        throwsA(isA<StateError>()),
      );
      expect(legacyRemoved, isFalse);
    });

    test('voice gateway token is stored only in secure storage and excluded from exports', () async {
      await settings.saveLocalCore(const LocalCoreSettings(
        host: '127.0.0.1',
        port: 8765,
        token: 'core-secret',
        voiceGatewayUrl: 'https://voice.example.test',
        voiceGatewayToken: 'ui-voice-secret',
      ));
      expect(await secrets.read(SettingsRepository.secretVoiceGatewayToken), 'ui-voice-secret');
      expect(await prefs.getString(SettingsRepository.secretVoiceGatewayToken), isNull);
      final exported = await settings.exportSettings();
      expect(exported, contains('https://voice.example.test'));
      expect(exported, isNot(contains('ui-voice-secret')));
      expect(exported, isNot(contains('core-secret')));
    });

    test('settings export cannot include credentials or cache entries', () async {
      await settings.saveRemoteCredentials(const RemoteCredentials(
        endpoint: 'https://control.example.test',
        token: 'ucp_supersecret',
        workspaceId: 'workspace-a',
        userId: 'user-a',
        role: WorkspaceRole.operator,
      ));
      await prefs.setString('cache.tasks.workspace-a', '{"tasks":[]}');
      final exported = await settings.exportSettings();
      final decoded = jsonDecode(exported) as Map<String, dynamic>;
      expect(exported, isNot(contains('ucp_supersecret')));
      expect(decoded, isNot(contains('cp_token')));
      expect(decoded, isNot(contains('cache.tasks.workspace-a')));
      expect(decoded['cp_endpoint'], 'https://control.example.test');
    });

    test('logout removes the token and cached objects but retains the endpoint', () async {
      await settings.saveRemoteCredentials(const RemoteCredentials(
        endpoint: 'https://control.example.test',
        token: 'ucp_supersecret',
        workspaceId: 'workspace-a',
        userId: 'user-a',
        role: WorkspaceRole.viewer,
      ));
      await prefs.setString('cache.tasks.workspace-a', '{}');
      await prefs.setString('cache.cursor.task-a', '4');
      await settings.clearRemoteCredentials();
      expect(await secrets.read(SettingsRepository.secretCpToken), isNull);
      expect(await prefs.getString(SettingsRepository.kCpWorkspace), isNull);
      expect(await prefs.getString('cache.tasks.workspace-a'), isNull);
      expect(await prefs.getString('cache.cursor.task-a'), isNull);
      expect(await settings.lastEndpoint(), 'https://control.example.test');
    });

    test('import ignores non-allowlisted fields, including accidental token fields', () async {
      await settings.importSettings(jsonEncode({
        'format': 'asgend-client-settings',
        'version': 1,
        'cp_endpoint': 'https://other.example.test',
        'cp_token': 'ucp_do_not_import',
        'core_token': 'do_not_import_either',
      }));
      expect(await prefs.getString(SettingsRepository.kCpEndpoint), 'https://other.example.test');
      expect(await secrets.read(SettingsRepository.secretCpToken), isNull);
      expect(await secrets.read(SettingsRepository.secretCoreToken), isNull);
    });
  });

  group('redaction and tolerant models', () {
    test('redacts bearer, prefixed, JSON and query credentials', () {
      const raw = 'Authorization: Bearer abc123 "token":"ucp_secret" ?ticket=dlt_ticket /v1/downloads/dlt_path';
      final safe = redact(raw);
      expect(safe, isNot(contains('abc123')));
      expect(safe, isNot(contains('ucp_secret')));
      expect(safe, isNot(contains('dlt_ticket')));
      expect(safe, isNot(contains('dlt_path')));
    });

    test('unknown fields do not prevent task/event parsing; cursors never go backwards', () {
      final task = RemoteTask.fromJson({
        'task_id': 'task-1',
        'state': 'running',
        'future_server_field': {'value': 1},
      });
      expect(task.taskId, 'task-1');
      expect(task.state, 'running');
      expect(task.raw['future_server_field'], {'value': 1});

      final page = EventPage.fromJson({
        'last_event_id': 1,
        'events': [
          {'event_id': 3, 'task_id': 'task-1', 'type': 'updated'},
          {'event_id': 2, 'task_id': 'task-1', 'type': 'created'},
        ],
      }, fallbackCursor: 8);
      expect(page.events.map((e) => e.eventId), [2, 3]);
      expect(page.lastEventId, 8);
    });
  });
}


class _FailingSecretStore implements SecretStore {
  @override
  Future<void> delete(String key) async {}

  @override
  Future<String?> read(String key) async => null;

  @override
  Future<void> write(String key, String value) async => throw StateError('simulated key store failure');
}
