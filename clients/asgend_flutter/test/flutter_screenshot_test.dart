import 'dart:typed_data';

import 'package:asgend_client/src/ui/common/flutter_screenshot.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('captures a mounted workspace RepaintBoundary as PNG', (tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: RepaintBoundary(
          key: key,
          child: const SizedBox(width: 320, height: 180, child: ColoredBox(color: Colors.blue)),
        ),
      ),
    ));
    await tester.pumpAndSettle();
    final bytes = await FlutterScreenshot.capturePng(key, pixelRatio: 1.0);
    expect(bytes, isA<Uint8List>());
    expect(bytes.length, greaterThan(32));
    expect(bytes.take(8).toList(), [137, 80, 78, 71, 13, 10, 26, 10]);
  });
  testWidgets('rejects unsafe pixel ratios before allocating an image', (tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: RepaintBoundary(
          key: key,
          child: const SizedBox(width: 320, height: 180, child: ColoredBox(color: Colors.black)),
        ),
      ),
    ));
    await tester.pumpAndSettle();
    await expectLater(
      FlutterScreenshot.capturePng(key, pixelRatio: 4),
      throwsA(isA<ArgumentError>()),
    );
    await expectLater(
      FlutterScreenshot.capturePng(key, pixelRatio: 1, maxPixels: 100),
      throwsA(isA<StateError>()),
    );
  });

}
