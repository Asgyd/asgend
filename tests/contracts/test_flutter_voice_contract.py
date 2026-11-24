#!/usr/bin/env python3
"""Static contract checks for the Flutter native Cowork voice integration."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
FLUTTER = ROOT / "clients/asgend_flutter"


class FlutterVoiceContractTests(unittest.TestCase):
    def test_record_plugin_is_compatible_with_project_dart_floor(self):
        pubspec = (FLUTTER / "pubspec.yaml").read_text()
        self.assertRegex(pubspec, r"record:\s*\^6\.2\.1")
        self.assertRegex(pubspec, r"audioplayers:\s*\^6\.4\.0")
        self.assertRegex(pubspec, r"sdk:\s*'>=3\.5\.0 <4\.0\.0'")

    def test_android_declares_microphone_permission(self):
        manifest = (FLUTTER / "android/app/src/main/AndroidManifest.xml").read_text()
        self.assertIn('android.permission.RECORD_AUDIO', manifest)
        self.assertIn('android.hardware.microphone', manifest)
        self.assertIn('android:required="false"', manifest)

    def test_voice_recording_uses_mono_16khz_wav(self):
        source = (FLUTTER / "lib/src/ui/local/cowork_page.dart").read_text()
        self.assertIn('encoder: AudioEncoder.wav', source)
        self.assertIn('sampleRate: 16000', source)
        self.assertIn('numChannels: 1', source)
        self.assertIn("mimeType: 'audio/wav'", source)
        self.assertIn("Duration(minutes: 3)", source)

    def test_failed_recordings_are_retryable_and_cleaned_on_route_disposal(self):
        source = (FLUTTER / "lib/src/ui/local/cowork_page.dart").read_text()
        self.assertIn("Retry saved voice clip", source)
        self.assertIn("Discard saved clip", source)
        self.assertIn("await _recorder.cancel()", source)
        self.assertIn("final failedPath = path ?? _recordingPath", source)
        self.assertIn("pendingAudioPath", source)
        self.assertIn("await _recorder.dispose()", source)

    def test_audio_reply_is_optional_and_player_lifecycle_is_owned(self):
        pubspec = (FLUTTER / "pubspec.yaml").read_text()
        source = (FLUTTER / "lib/src/ui/local/cowork_page.dart").read_text()
        self.assertIn("audioplayers: ^6.4.0", pubspec)
        self.assertIn("speech_wav_base64", source)
        self.assertIn("await _player.dispose()", source)
        self.assertIn("TTS is optional", source)

    def test_flutter_cowork_sends_report_to_gateway_and_confirms_revision(self):
        source = (FLUTTER / "lib/src/ui/local/cowork_page.dart").read_text()
        self.assertIn('gateway.sendTurn(', source)
        self.assertIn('gateway.confirm(sessionId: session, revision: _revision)', source)
        self.assertIn('TaskChatPage(client: widget.coreClient, taskId: taskId)', source)
        self.assertNotRegex(source, r"rpc\(\s*\{\s*['\"]op['\"]\s*:\s*['\"]submit['\"]")

    def test_separate_gateway_token_is_secret_and_not_exportable(self):
        settings = (FLUTTER / "lib/src/storage/settings.dart").read_text()
        self.assertIn("secretVoiceGatewayToken = 'voice_gateway_token'", settings)
        self.assertIn('secrets.write(secretVoiceGatewayToken', settings)
        export_block = settings.split('static const exportableKeys', 1)[1].split('];', 1)[0]
        self.assertNotIn('secretVoiceGatewayToken', export_block)
        self.assertIn('kVoiceGatewayUrl', export_block)

    def test_endpoint_policy_requires_tls_for_android(self):
        source = (FLUTTER / "lib/src/api/voice_gateway_client.dart").read_text()
        self.assertIn("if (isAndroid && scheme != 'https')", source)
        self.assertIn("scheme == 'http' && !loopback", source)

    def test_screenshot_is_flutter_workspace_capture_not_device_screen_capture(self):
        source = (FLUTTER / "lib/src/ui/common/flutter_screenshot.dart").read_text()
        self.assertIn('RenderRepaintBoundary', source)
        self.assertIn('renderObject.toImage', source)
        self.assertIn('ui.ImageByteFormat.png', source)
        self.assertIn('capture other apps, notifications', source)
        self.assertNotIn('MediaProjection', source.split('class FlutterScreenshot', 1)[1])

    def test_task_workspace_can_capture_and_share_the_actual_preview_pane(self):
        source = (FLUTTER / "lib/src/ui/local/local_tasks.dart").read_text()
        self.assertIn("_workspacePreviewKey", source)
        self.assertIn("FlutterScreenshot.capturePng", source)
        self.assertIn("SharePlus.instance.share", source)
        self.assertIn("Capture native PNG snapshot", source)
        self.assertIn("XFile(snapshotFile.path, mimeType: 'image/png'", source)

    def test_native_screenshot_has_preallocation_pixel_and_dimension_limits(self):
        source = (FLUTTER / "lib/src/ui/common/flutter_screenshot.dart").read_text()
        self.assertIn("maxPixels = 16 * 1000 * 1000", source)
        self.assertIn("maxDimension = 8192", source)
        self.assertIn("pixelRatio > 3.0", source)

    def test_shared_workspace_snapshot_is_removed_from_app_cache(self):
        source = (FLUTTER / "lib/src/ui/local/local_tasks.dart").read_text()
        capture = source.split("Future<void> _captureNativeWorkspacePreview()", 1)[1].split("Widget _workspacePreviewContent", 1)[0]
        self.assertIn("SharePlus.instance.share", capture)
        self.assertIn("finally", capture)
        self.assertIn("await snapshotFile.delete()", capture)

    def test_local_home_exposes_cowork_tab(self):
        source = (FLUTTER / "lib/src/ui/local/local_home.dart").read_text()
        self.assertIn('length: 5', source)
        self.assertIn("text: 'Cowork'", source)
        self.assertIn('CoworkPage(', source)

    def test_flutter_ci_will_analyze_test_and_build_native_apk(self):
        workflow = (ROOT / ".github/workflows/flutter-android.yml").read_text()
        self.assertIn('flutter analyze --no-pub', workflow)
        self.assertIn('flutter test --no-pub', workflow)
        self.assertIn('flutter build apk --release --no-pub', workflow)


if __name__ == "__main__":
    unittest.main()
