import 'dart:convert';

import 'package:asgend_client/src/api/voice_gateway_client.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';

void main() {
  group('VoiceGatewayClient endpoint policy', () {
    test('desktop permits loopback HTTP and HTTPS endpoints', () {
      expect(VoiceGatewayClient.validateEndpoint('http://127.0.0.1:8788', isAndroid: false), isNull);
      expect(VoiceGatewayClient.validateEndpoint('https://voice.example.test', isAndroid: false), isNull);
    });

    test('desktop rejects cleartext non-loopback and URL credentials', () {
      expect(VoiceGatewayClient.validateEndpoint('http://192.168.1.20:8788', isAndroid: false), isNotNull);
      expect(VoiceGatewayClient.validateEndpoint('https://user:pass@voice.example.test', isAndroid: false), isNotNull);
    });

    test('Android requires HTTPS', () {
      expect(VoiceGatewayClient.validateEndpoint('http://10.0.0.5:8788', isAndroid: true), isNotNull);
      expect(VoiceGatewayClient.validateEndpoint('https://voice.example.test', isAndroid: true), isNull);
    });
  });

  test('create, media turn and confirm use the separate gateway token and revision', () async {
    final seen = <http.Request>[];
    final transport = MockClient((request) async {
      seen.add(request);
      if (request.url.path == '/v1/sessions') {
        return http.Response(jsonEncode({'ok': true, 'session_id': '12345678-1234-4234-9234-123456789abc'}), 200);
      }
      if (request.url.path.endsWith('/turn')) {
        final body = jsonDecode(request.body) as Map<String, dynamic>;
        expect(body['text'], 'Review this screen');
        expect((body['attachments'] as List).single['mime_type'], 'image/png');
        expect((body['selections'] as List).single['label'], 'selected code');
        return http.Response(jsonEncode({'ok': true, 'revision': 7, 'report': '# Updated report', 'attachment_count': 1}), 200);
      }
      if (request.url.path.endsWith('/confirm')) {
        expect(jsonDecode(request.body), {'revision': 7});
        return http.Response(jsonEncode({'ok': true, 'task_id': 'task-confirmed', 'revision': 7}), 200);
      }
      return http.Response('not found', 404);
    });
    final gateway = VoiceGatewayClient(baseUrl: 'https://voice.example.test/', token: 'ui-token-test', httpClient: transport);
    final session = await gateway.createSession('ASGEND project context');
    final turn = await gateway.sendTurn(
      sessionId: session,
      text: 'Review this screen',
      selections: const [{'kind': 'code', 'label': 'selected code', 'text': 'final class A {}'}],
      attachments: const [VoiceAttachment(name: 'screen.png', mimeType: 'image/png', dataBase64: 'iVBORw0KGgo=')],
      projectState: 'Current state',
    );
    final confirmation = await gateway.confirm(sessionId: session, revision: turn['revision'] as int);
    expect(turn['report'], '# Updated report');
    expect(confirmation['task_id'], 'task-confirmed');
    expect(gateway.toString(), isNot(contains('ui-token-test')));
    expect(seen, hasLength(3));
    for (final request in seen) {
      expect(request.headers['X-AO-Token'], 'ui-token-test');
    }
    expect(seen.last.url.path, '/v1/sessions/$session/confirm');
  });

  test('stale report revision is returned as a typed gateway error', () async {
    final transport = MockClient((_) async => http.Response(jsonEncode({'ok': false, 'error': 'Report revision is stale.'}), 409));
    final gateway = VoiceGatewayClient(baseUrl: 'https://voice.example.test', token: 'ui-token-test', httpClient: transport);
    await expectLater(
      gateway.confirm(sessionId: '12345678-1234-4234-9234-123456789abc', revision: 3),
      throwsA(isA<VoiceGatewayException>().having((error) => error.statusCode, 'statusCode', 409)),
    );
  });
}
