import 'dart:io';

import 'package:flutter_file_dialog/flutter_file_dialog.dart';
import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';

import '../api/artifact_download.dart';

/// Where verified artifacts go after download.
///
/// Downloads are first written to the app-private cache directory (not
/// backed up, not visible to other apps). The user then explicitly exports:
/// * Android: "Save…" opens the Storage Access Framework create-document
///   picker (flutter_file_dialog → ACTION_CREATE_DOCUMENT), so the app needs no
///   storage permission; "Share…" uses the system share sheet (share_plus,
///   FileProvider URI with a temporary read grant).
/// * Desktop (Linux): "Save…" copies into the user's Downloads directory.
abstract class ArtifactExporter {
  Future<Directory> downloadDirectory();
  Future<String?> saveAs(DownloadedArtifact artifact);
  Future<void> share(DownloadedArtifact artifact);
}

class PlatformArtifactExporter implements ArtifactExporter {
  const PlatformArtifactExporter();

  @override
  Future<Directory> downloadDirectory() async {
    final base = await getApplicationCacheDirectory();
    return Directory('${base.path}${Platform.pathSeparator}artifacts');
  }

  @override
  Future<String?> saveAs(DownloadedArtifact artifact) async {
    if (Platform.isAndroid || Platform.isIOS) {
      return FlutterFileDialog.saveFile(
        params: SaveFileDialogParams(sourceFilePath: artifact.file.path, fileName: sanitizeFileName(artifact.artifact.name)),
      );
    }
    final downloads = await getDownloadsDirectory() ?? await getApplicationDocumentsDirectory();
    var target = File('${downloads.path}${Platform.pathSeparator}${sanitizeFileName(artifact.artifact.name)}');
    var i = 1;
    while (await target.exists()) {
      target = File('${downloads.path}${Platform.pathSeparator}($i) ${sanitizeFileName(artifact.artifact.name)}');
      i++;
    }
    final copy = await artifact.file.copy(target.path);
    return copy.path;
  }

  @override
  Future<void> share(DownloadedArtifact artifact) async {
    await SharePlus.instance.share(ShareParams(
      files: [XFile(artifact.file.path, mimeType: artifact.artifact.mime, name: sanitizeFileName(artifact.artifact.name))],
      title: artifact.artifact.name,
    ));
  }
}
