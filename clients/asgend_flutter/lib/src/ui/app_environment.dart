import 'dart:io' show Platform;

import 'package:http/http.dart' as http;

import '../storage/artifact_export.dart';
import '../storage/settings.dart';
import '../storage/stores.dart';

/// Composition root shared by the extracted local and remote UI modules.
/// Platform checks live here rather than being repeated across widgets.
class AppEnvironment {
  AppEnvironment({
    required this.settings,
    required this.isAndroid,
    http.Client Function()? httpClientFactory,
    ArtifactExporter? artifactExporter,
  })  : httpClientFactory = httpClientFactory ?? http.Client.new,
        artifactExporter = artifactExporter ?? const PlatformArtifactExporter();

  factory AppEnvironment.platform({SettingsRepository? settings}) => AppEnvironment(
        settings: settings ?? SettingsRepository(prefs: SharedPrefsStore(), secrets: PlatformSecretStore()),
        isAndroid: Platform.isAndroid,
      );

  final SettingsRepository settings;
  final bool isAndroid;

  /// Injectable HTTP transport and exporter keep UI tests hermetic and avoid
  /// making platform channels or external services part of widget tests.
  final http.Client Function() httpClientFactory;
  final ArtifactExporter artifactExporter;
}
