import 'dart:async';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;

import '../models/remote_models.dart';
import 'api_error.dart';
import 'control_plane_client.dart';

/// Result of a verified download. The file lives in app-private storage until
/// the user exports it through SAF or the share sheet.
class DownloadedArtifact {
  const DownloadedArtifact({required this.file, required this.artifact, required this.sha256Hex});
  final File file;
  final RemoteArtifact artifact;
  final String sha256Hex;
}

typedef DownloadProgress = void Function(int received, int? total);

/// Artifact download: ticket → streamed GET → incremental SHA-256 → compare →
/// atomic rename. A mismatching or oversized file is deleted and never exposed.
class ArtifactDownloader {
  ArtifactDownloader(this.client, {this.maxBytes = 512 * 1024 * 1024});

  final ControlPlaneClient client;

  /// Hard upper bound regardless of what the server claims (node uploads are
  /// limited to 256 MiB by the contract; this leaves headroom).
  final int maxBytes;

  Future<DownloadedArtifact> download(RemoteArtifact artifact, Directory targetDir, {DownloadProgress? onProgress}) async {
    if (!artifact.available) {
      throw ApiException(ApiErrorCode.conflict, 'Artifact is not available (state: ${artifact.state}).');
    }
    final expected = artifact.sha256.toLowerCase();
    if (!RegExp(r'^[0-9a-f]{64}$').hasMatch(expected)) {
      throw ApiException(ApiErrorCode.badResponse, 'Artifact has no valid SHA-256; refusing to download.');
    }
    if (artifact.size > maxBytes) {
      throw ApiException(ApiErrorCode.payloadTooLarge, 'Artifact is larger than the client limit.');
    }

    var ticket = await client.downloadTicket(artifact.artifactId);
    http.StreamedResponse response;
    try {
      response = await client.openDownload(ticket);
    } on ApiException catch (e) {
      if (e.code != ApiErrorCode.gone) rethrow;
      // Ticket expired between issue and use (300 s TTL): get one fresh ticket.
      ticket = await client.downloadTicket(artifact.artifactId);
      response = await client.openDownload(ticket);
    }
    final stream = response.stream;
    final headers = response.headers;

    final headerSum = headers['x-checksum-sha256']?.trim().toLowerCase();
    if (headerSum != null && headerSum.isNotEmpty && headerSum != expected) {
      await _drain(stream);
      throw ApiException(ApiErrorCode.checksumMismatch, 'Server checksum header does not match artifact metadata.');
    }
    final contentLength = int.tryParse(headers['content-length'] ?? '');
    if (contentLength != null && artifact.size > 0 && contentLength != artifact.size) {
      await _drain(stream);
      throw ApiException(ApiErrorCode.checksumMismatch, 'Content-Length does not match artifact size.');
    }
    final total = contentLength ?? (artifact.size > 0 ? artifact.size : null);

    await targetDir.create(recursive: true);
    final safeName = sanitizeFileName(artifact.name);
    final part = File('${targetDir.path}${Platform.pathSeparator}.$safeName.${artifact.artifactId.hashCode.toUnsigned(32)}.part');
    final sink = part.openWrite();
    final digestSink = _DigestSink();
    final hasher = sha256.startChunkedConversion(digestSink);
    var received = 0;
    try {
      await for (final chunk in stream) {
        received += chunk.length;
        if (received > maxBytes || (artifact.size > 0 && received > artifact.size)) {
          throw ApiException(ApiErrorCode.checksumMismatch, 'Received more bytes than announced.');
        }
        hasher.add(chunk);
        sink.add(chunk);
        onProgress?.call(received, total);
      }
      hasher.close();
      await sink.flush();
      await sink.close();
    } catch (e) {
      await _safeClose(sink);
      await _safeDelete(part);
      if (e is ApiException) rethrow;
      throw ApiException(ApiErrorCode.network, 'Download interrupted.');
    }

    final actual = digestSink.value?.toString() ?? '';
    if (actual != expected || (artifact.size > 0 && received != artifact.size)) {
      await _safeDelete(part);
      throw ApiException(ApiErrorCode.checksumMismatch, 'SHA-256 mismatch: expected $expected, got $actual.');
    }

    final target = await _uniqueTarget(targetDir, safeName);
    final file = await part.rename(target.path);
    return DownloadedArtifact(file: file, artifact: artifact, sha256Hex: actual);
  }

  static Future<void> _drain(Stream<List<int>> s) async {
    try {
      await s.drain<void>();
    } catch (_) {}
  }

  static Future<void> _safeClose(IOSink s) async {
    try {
      await s.close();
    } catch (_) {}
  }

  static Future<void> _safeDelete(File f) async {
    try {
      if (await f.exists()) await f.delete();
    } catch (_) {}
  }

  static Future<File> _uniqueTarget(Directory dir, String name) async {
    final dot = name.lastIndexOf('.');
    final stem = dot > 0 ? name.substring(0, dot) : name;
    final ext = dot > 0 ? name.substring(dot) : '';
    var candidate = File('${dir.path}${Platform.pathSeparator}$name');
    var i = 1;
    while (await candidate.exists()) {
      candidate = File('${dir.path}${Platform.pathSeparator}$stem ($i)$ext');
      i++;
    }
    return candidate;
  }
}

class _DigestSink implements Sink<Digest> {
  Digest? value;
  @override
  void add(Digest data) => value = data;
  @override
  void close() {}
}

/// Basename only, no separators/NUL/control characters, no leading dots, at
/// most 255 UTF-16 units. Server names are already validated (contract §5),
/// this is defence in depth against path traversal.
String sanitizeFileName(String raw) {
  var name = raw.split(RegExp(r'[/\\]')).last;
  name = name.replaceAll(RegExp(r'[\x00-\x1f\x7f<>:"|?*]'), '_').trim();
  while (name.startsWith('.')) {
    name = name.substring(1);
  }
  if (name.length > 255) name = name.substring(name.length - 255);
  return name.isEmpty ? 'artifact' : name;
}

const Set<String> _executableExtensions = {
  'apk', 'aab', 'apks', 'xapk', 'dex', 'jar', 'exe', 'msi', 'msix', 'appx', 'com', 'scr', 'bat', 'cmd', 'ps1',
  'psm1', 'vbs', 'vbe', 'js', 'jse', 'wsf', 'hta', 'lnk', 'reg', 'dll', 'sys', 'sh', 'bash', 'zsh', 'ksh',
  'csh', 'fish', 'run', 'bin', 'elf', 'so', 'ko', 'appimage', 'deb', 'rpm', 'pkg', 'dmg', 'app', 'command',
  'py', 'pyc', 'pyz', 'pl', 'rb', 'php', 'lua', 'html', 'htm', 'xhtml', 'svg', 'desktop', 'service',
};

const Set<String> _executableMimes = {
  'application/vnd.android.package-archive', 'application/x-executable', 'application/x-sharedlib',
  'application/x-msdownload', 'application/x-msdos-program', 'application/x-sh', 'application/x-shellscript',
  'text/x-shellscript', 'application/java-archive', 'application/x-elf', 'application/x-mach-binary',
  'application/x-python-code', 'text/x-python', 'text/html', 'application/xhtml+xml', 'image/svg+xml',
  'application/javascript', 'text/javascript',
};

/// True for files that can execute code when opened (installers, scripts,
/// binaries, active documents). The UI must ask for confirmation first.
bool isPotentiallyExecutable(String name, String mime) {
  final lower = name.toLowerCase();
  final parts = lower.split('.');
  // Check every extension so "report.pdf.exe" and "x.tar.sh" are caught.
  for (final ext in parts.skip(1)) {
    if (_executableExtensions.contains(ext)) return true;
  }
  final m = mime.toLowerCase().split(';').first.trim();
  return _executableMimes.contains(m);
}
