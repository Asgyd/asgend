import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/rendering.dart';
import 'package:flutter/widgets.dart';

/// Captures a Flutter-rendered workspace subtree to PNG.
///
/// This intentionally does not use Android's MediaProjection API and cannot
/// capture other apps, notifications, or the device screen outside ASGEND.
/// Pixel and encoded-byte bounds are checked before allocating the image.
class FlutterScreenshot {
  const FlutterScreenshot._();

  static Future<Uint8List> capturePng(
    GlobalKey repaintBoundaryKey, {
    double pixelRatio = 1.5,
    int maxBytes = 8 * 1024 * 1024,
    int maxPixels = 16 * 1000 * 1000,
    int maxDimension = 8192,
  }) async {
    if (!pixelRatio.isFinite || pixelRatio <= 0 || pixelRatio > 3.0) {
      throw ArgumentError.value(pixelRatio, 'pixelRatio', 'must be > 0 and <= 3');
    }
    if (maxBytes < 1024 || maxPixels < 1 || maxDimension < 1) {
      throw ArgumentError('Screenshot output bounds must be positive and non-trivial.');
    }
    final context = repaintBoundaryKey.currentContext;
    if (context == null) throw StateError('The workspace preview is not mounted.');
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderRepaintBoundary) {
      throw StateError('The workspace preview is not a RepaintBoundary.');
    }
    if (!renderObject.hasSize || renderObject.size.isEmpty) {
      throw StateError('The workspace preview has no laid-out size.');
    }
    final pixelWidth = (renderObject.size.width * pixelRatio).ceil();
    final pixelHeight = (renderObject.size.height * pixelRatio).ceil();
    final pixelCount = pixelWidth * pixelHeight;
    if (pixelWidth > maxDimension || pixelHeight > maxDimension || pixelCount > maxPixels) {
      throw StateError('Workspace snapshot dimensions exceed the configured pixel limit.');
    }
    if (renderObject.debugNeedsPaint) {
      await WidgetsBinding.instance.endOfFrame;
      if (!context.mounted) throw StateError('The workspace preview was removed before capture.');
    }
    final ui.Image image = await renderObject.toImage(pixelRatio: pixelRatio);
    try {
      final data = await image.toByteData(format: ui.ImageByteFormat.png);
      if (data == null) throw StateError('Flutter could not encode the workspace snapshot.');
      final bytes = data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
      if (bytes.isEmpty || bytes.length > maxBytes) {
        throw StateError('Workspace snapshot exceeds the $maxBytes-byte limit.');
      }
      return Uint8List.fromList(bytes);
    } finally {
      image.dispose();
    }
  }
}
