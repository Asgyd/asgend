import 'dart:convert';
import 'dart:io';

import 'package:asgend_client/src/api/artifact_download.dart';
import 'package:asgend_client/src/storage/artifact_export.dart';
import 'package:asgend_client/src/storage/settings.dart';
import 'package:asgend_client/src/storage/stores.dart';
import 'package:asgend_client/src/ui/app_environment.dart';
import 'package:asgend_client/src/ui/remote/remote_home.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('viewer can pair and sees a read-only remote workspace', (tester) async {
    final prefs = MemoryKeyValueStore();
    final secrets = MemorySecretStore();
    final settings = SettingsRepository(prefs: prefs, secrets: secrets);
    final requests = <http.Request>[];
    final environment = AppEnvironment(
      settings: settings,
      isAndroid: true,
      artifactExporter: _FakeArtifactExporter(),
      httpClientFactory: () => MockClient((request) async {
        requests.add(request);
        switch (request.url.path) {
          case '/v1/pairing/redeem':
            expect(request.method, 'POST');
            expect(_header(request, 'authorization'), isNull);
            final body = jsonDecode(request.body) as Map<String, dynamic>;
            expect(body['code'], 'pair-once');
            return _json({'token': 'ucp_widget-secret', 'user_id': 'user-1', 'workspace_id': 'workspace-1', 'role': 'viewer'});
          case '/v1/me':
            expect(_header(request, 'authorization'), 'Bearer ucp_widget-secret');
            return _json({
              'user': {'user_id': 'user-1', 'name': 'Widget User'},
              'workspaces': [
                {'workspace_id': 'workspace-1', 'name': 'Widget workspace', 'role': 'viewer'},
              ],
            });
          case '/v1/workspaces/workspace-1/nodes':
            return _json({'nodes': [
              {'node_id': 'node-1', 'workspace_id': 'workspace-1', 'name': 'Widget node', 'status': 'online'},
            ]});
          case '/v1/workspaces/workspace-1/tasks':
            return _json({'tasks': [
              {'task_id': 'task-1', 'workspace_id': 'workspace-1', 'node_id': 'node-1', 'title': 'Visible task', 'state': 'queued'},
            ]});
          case '/v1/auth/logout':
            expect(request.method, 'POST');
            expect(_header(request, 'authorization'), 'Bearer ucp_widget-secret');
            return _json({'ok': true});
          default:
            return _json({'error': {'code': 'not_found', 'message': 'unexpected widget route'}}, status: 404);
        }
      }),
    );

    await tester.pumpWidget(MaterialApp(home: RemoteHomePage(environment: environment)));
    await tester.pumpAndSettle();
    expect(find.text('Connect a device'), findsOneWidget);

    await tester.enterText(find.byType(TextField).at(0), 'https://control.example.test');
    await tester.enterText(find.byType(TextField).at(1), 'pair-once');
    await tester.tap(find.text('Pair device'));
    await tester.pumpAndSettle();

    expect(find.text('Your workspace role is read-only. You can view tasks but cannot create or change them.'), findsOneWidget);
    // FilledButton.icon is a private FilledButton subclass on some SDKs; match by `is`, not exact type.
    final createTaskButton = tester.widget<FilledButton>(find.ancestor(
      of: find.text('Create task'),
      matching: find.byWidgetPredicate((widget) => widget is FilledButton),
    ));
    expect(createTaskButton.onPressed, isNull);
    await tester.scrollUntilVisible(
      find.text('Visible task'),
      180,
      scrollable: find.descendant(
        of: find.byKey(const Key('remote-task-list')),
        matching: find.byType(Scrollable),
      ).first,
    );
    expect(find.text('Visible task'), findsOneWidget);
    expect(find.text('Widget node · online'), findsOneWidget);
    expect(find.text('viewer · workspace workspace-1'), findsOneWidget);
    expect(await secrets.read(SettingsRepository.secretCpToken), 'ucp_widget-secret');
    expect(await prefs.getString(SettingsRepository.secretCpToken), isNull);
    expect(requests.any((r) => r.url.path == '/v1/pairing/redeem'), isTrue);
    expect(requests.every((r) => !r.url.toString().contains('ucp_widget-secret')), isTrue);

    // Stop the session's polling timer before the test ends.
    await tester.tap(find.byTooltip('Logout and revoke token'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Disconnect'));
    await tester.pumpAndSettle();
    expect(await secrets.read(SettingsRepository.secretCpToken), isNull);
  });


  testWidgets('pairing preserves visible offline state when the Control Plane is unavailable', (tester) async {
    final prefs = MemoryKeyValueStore();
    final secrets = MemorySecretStore();
    final environment = AppEnvironment(
      settings: SettingsRepository(prefs: prefs, secrets: secrets),
      isAndroid: true,
      artifactExporter: _FakeArtifactExporter(),
      httpClientFactory: () => MockClient((request) async {
        if (request.url.path == '/v1/pairing/redeem') {
          return _json({'token': 'ucp_offline-token', 'user_id': 'user-1', 'workspace_id': 'workspace-1', 'role': 'operator'});
        }
        if (request.url.path == '/v1/me') {
          return _json({'error': {'code': 'unavailable', 'message': 'simulated Control Plane outage'}}, status: 503);
        }
        return _json({'error': {'code': 'unavailable', 'message': 'simulated Control Plane outage'}}, status: 503);
      }),
    );
    await tester.pumpWidget(MaterialApp(home: RemoteHomePage(environment: environment)));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField).at(0), 'https://control.example.test');
    await tester.enterText(find.byType(TextField).at(1), 'pair-once');
    await tester.tap(find.text('Pair device'));
    await tester.pumpAndSettle();

    expect(find.text('Retry'), findsOneWidget);
    expect(find.textContaining('simulated Control Plane outage'), findsOneWidget);
    expect(await secrets.read(SettingsRepository.secretCpToken), 'ucp_offline-token');
    // Failures are visible; the UI does not silently discard the valid paired token.
    await tester.tap(find.byTooltip('Logout and revoke token'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Disconnect'));
    await tester.pumpAndSettle();
    expect(await secrets.read(SettingsRepository.secretCpToken), isNull);
  });

  testWidgets('pairing validation reports empty endpoint and code', (tester) async {
    final environment = AppEnvironment(
      settings: SettingsRepository(prefs: MemoryKeyValueStore(), secrets: MemorySecretStore()),
      isAndroid: true,
      artifactExporter: _FakeArtifactExporter(),
      httpClientFactory: () => MockClient((_) async => _json({'error': {'code': 'not_found'}}, status: 404)),
    );
    await tester.pumpWidget(MaterialApp(home: RemoteHomePage(environment: environment)));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Pair device'));
    await tester.pumpAndSettle();
    expect(find.text('Enter the Control Plane HTTPS endpoint and the one-time pairing code.'), findsOneWidget);
  });
}

http.Response _json(Map<String, dynamic> body, {int status = 200}) => http.Response(
  jsonEncode(body),
  status,
  headers: const {'content-type': 'application/json', 'x-asgend-api-version': '1'},
);

String? _header(http.Request request, String name) {
  for (final entry in request.headers.entries) {
    if (entry.key.toLowerCase() == name.toLowerCase()) return entry.value;
  }
  return null;
}

class _FakeArtifactExporter implements ArtifactExporter {
  @override
  Future<Directory> downloadDirectory() => Directory.systemTemp.createTemp('asgend-widget-artifacts-');

  @override
  Future<String?> saveAs(DownloadedArtifact artifact) async => artifact.file.path;

  @override
  Future<void> share(DownloadedArtifact artifact) async {}
}
