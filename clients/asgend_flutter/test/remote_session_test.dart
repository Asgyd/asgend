import 'dart:convert';

import 'package:asgend_client/src/api/backoff.dart';
import 'package:asgend_client/src/models/remote_models.dart';
import 'package:asgend_client/src/state/remote_session.dart';
import 'package:asgend_client/src/storage/settings.dart';
import 'package:asgend_client/src/storage/stores.dart';
import 'package:asgend_client/src/storage/task_cache.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';


void main() {
  group('RemoteSession pairing and lifecycle', () {
    late MemoryKeyValueStore prefs;
    late MemorySecretStore secrets;
    late SettingsRepository settings;
    late TaskCache cache;
    late List<http.Request> requests;

    setUp(() {
      prefs = MemoryKeyValueStore();
      secrets = MemorySecretStore();
      settings = SettingsRepository(prefs: prefs, secrets: secrets);
      cache = TaskCache(prefs);
      requests = <http.Request>[];
    });

    http.Response response(Map<String, dynamic> body, [int status = 200]) => http.Response(
          jsonEncode(body),
          status,
          headers: const {'content-type': 'application/json', 'x-asgend-api-version': '1'},
        );

    RemoteSession createSession({String role = 'operator', int failMeCalls = 0}) {
      var meCalls = 0;
      return RemoteSession(
          settings: settings,
          cache: cache,
          refreshInterval: const Duration(hours: 1),
          backoff: Backoff(base: const Duration(milliseconds: 10), cap: const Duration(milliseconds: 10), minDelay: const Duration(milliseconds: 10)),
          httpClientFactory: () => MockClient((request) async {
            requests.add(request);
            switch (request.url.path) {
              case '/v1/pairing/redeem':
                expect(request.method, 'POST');
                expect(_header(request, 'authorization'), isNull);
                final body = jsonDecode(request.body) as Map<String, dynamic>;
                expect(body['code'], 'pair-once');
                expect(body['device_name'], 'test-device');
                return response({
                  'token': 'ucp_test-token-secret',
                  'user_id': 'user-1',
                  'workspace_id': 'workspace-1',
                  'role': role,
                });
              case '/v1/me':
                expect(_header(request, 'authorization'), 'Bearer ucp_test-token-secret');
                meCalls++;
                if (meCalls <= failMeCalls) {
                  return response({'error': {'code': 'unavailable', 'message': 'temporary test outage'}}, 503);
                }
                return response({
                  'user': {'user_id': 'user-1', 'name': 'Test user'},
                  'workspaces': [
                    {'workspace_id': 'workspace-1', 'name': 'Test workspace', 'role': role},
                    {'workspace_id': 'workspace-2', 'name': 'Another workspace', 'role': 'viewer'},
                  ],
                });
              case '/v1/workspaces/workspace-1/nodes':
                return response({
                  'nodes': [
                    {'node_id': 'node-1', 'workspace_id': 'workspace-1', 'name': 'Test node', 'status': 'online'},
                  ],
                });
              case '/v1/workspaces/workspace-1/tasks':
                return response({
                  'tasks': [
                    {'task_id': 'task-1', 'workspace_id': 'workspace-1', 'node_id': 'node-1', 'title': 'Test task', 'state': 'queued'},
                  ],
                });
              case '/v1/auth/logout':
                expect(request.method, 'POST');
                expect(_header(request, 'authorization'), 'Bearer ucp_test-token-secret');
                return response({'ok': true});
              default:
                return response({'error': {'code': 'not_found', 'message': 'unexpected route'}}, 404);
            }
          }),
        );
    }

    test('pairing saves bearer only in secure storage and refreshes workspace data', () async {
      final session = createSession();
      addTearDown(session.dispose);

      await session.init();
      expect(session.status, RemoteStatus.unpaired);

      await session.pair(
        endpoint: 'https://control.example.test',
        code: 'pair-once',
        deviceName: 'test-device',
      );

      expect(session.status, RemoteStatus.online);
      expect(session.canOperate, isTrue);
      expect(session.me?.userId, 'user-1');
      expect(session.me?.workspaces, hasLength(2));
      expect(session.nodes.map((n) => n.nodeId), ['node-1']);
      expect(session.tasks.map((t) => t.taskId), ['task-1']);
      expect(secrets.data[SettingsRepository.secretCpToken], 'ucp_test-token-secret');
      expect(prefs.data.containsKey(SettingsRepository.secretCpToken), isFalse);
      expect(prefs.data[SettingsRepository.kCpWorkspace], 'workspace-1');
      expect(requests.any((r) => r.url.path == '/v1/me' && _header(r, 'authorization') == 'Bearer ucp_test-token-secret'), isTrue);
    });


    test('initial offline response keeps the retry timer and recovers automatically', () async {
      final session = createSession(failMeCalls: 1);
      addTearDown(session.dispose);
      await session.pair(endpoint: 'https://control.example.test', code: 'pair-once', deviceName: 'test-device');
      expect(session.status, RemoteStatus.offline);

      for (var i = 0; i < 60 && session.status != RemoteStatus.online; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 10));
      }
      expect(session.status, RemoteStatus.online);
      expect(requests.where((r) => r.url.path == '/v1/me').length, greaterThanOrEqualTo(2));
    });

    test('viewer role remains read-only and logout clears credentials and cached objects', () async {
      final session = createSession(role: 'viewer');
      addTearDown(session.dispose);
      await session.pair(endpoint: 'https://control.example.test', code: 'pair-once', deviceName: 'test-device');

      expect(session.role, WorkspaceRole.viewer);
      expect(session.canOperate, isFalse);
      await cache.saveTasks('workspace-1', session.tasks);
      await cache.saveCursor('task-1', 42);
      await session.logout();

      expect(session.status, RemoteStatus.unpaired);
      expect(await secrets.read(SettingsRepository.secretCpToken), isNull);
      expect(await prefs.getString(SettingsRepository.kCpWorkspace), isNull);
      expect(await prefs.getString('cache.tasks.workspace-1'), isNull);
      expect(await cache.cursor('task-1'), 0);
      expect(requests.any((r) => r.url.path == '/v1/auth/logout'), isTrue);
      expect(await settings.lastEndpoint(), 'https://control.example.test');
    });
  });
}

String? _header(http.Request request, String name) {
  for (final entry in request.headers.entries) {
    if (entry.key.toLowerCase() == name.toLowerCase()) return entry.value;
  }
  return null;
}
