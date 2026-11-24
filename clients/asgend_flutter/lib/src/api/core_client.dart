import 'dart:async';
import 'dart:convert';
import 'dart:io';

import '../util/redact.dart';

/// Line-delimited JSON client for the local/desktop core TCP API
/// (docs/PROTOCOL.md). One request per connection.
///
/// Security: the protocol has no TLS. This mode is intended for desktop
/// (loopback) or a phone reaching the core through VPN/SSH tunnel only; the
/// Android UI requires an explicit acknowledgement before it is used. The
/// `auth_token` is sent in the request body as the protocol requires and is
/// redacted from every error message produced here.
class CoreClient {
  const CoreClient({required this.host, required this.port, required this.token});

  final String host;
  final int port;
  final String token;

  Future<Map<String, dynamic>> rpc(Map<String, dynamic> payload, {Duration timeout = const Duration(seconds: 60)}) async {
    Socket? socket;
    try {
      socket = await Socket.connect(host, port, timeout: const Duration(seconds: 5));
      final request = <String, dynamic>{...payload};
      if (token.isNotEmpty) request['auth_token'] = token;
      socket.write('${jsonEncode(request)}\n');
      await socket.flush();
      final line = await socket.cast<List<int>>().transform(utf8.decoder).transform(const LineSplitter()).first.timeout(timeout);
      final decoded = jsonDecode(line);
      if (decoded is! Map<String, dynamic>) {
        throw const FormatException('Core returned a non-object JSON response.');
      }
      if (decoded['ok'] != true) {
        throw CoreException(redactWith((decoded['error'] ?? 'Request failed').toString(), [token]));
      }
      return decoded;
    } on CoreException {
      rethrow;
    } on TimeoutException {
      throw const CoreException('Core did not answer in time.');
    } on SocketException catch (e) {
      throw CoreException('Cannot reach core at $host:$port (${e.osError?.message ?? e.message}).');
    } on FormatException catch (e) {
      throw CoreException(redactWith('Bad core response: ${e.message}', [token]));
    } finally {
      socket?.destroy();
    }
  }
}

class CoreException implements Exception {
  const CoreException(this.message);
  final String message;
  @override
  String toString() => message;
}
