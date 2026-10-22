import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';

import 'support/golden_diagnostics.dart';

/// Linux-rendered platform overrides; these are not simulator/device captures.
void main() {
  installHostGoldens();
  setUpAll(() async {
    for (final name in ['Inter', 'SpaceGrotesk']) {
      final loader = FontLoader(name)
        ..addFont(rootBundle.load('assets/fonts/$name.ttf'));
      await loader.load();
    }
    final icons = FontLoader('MaterialIcons')
      ..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'));
    await icons.load();
  });

  Future<void> show(
    WidgetTester tester,
    Size size, {
    double scale = 1,
    EdgeInsets padding = EdgeInsets.zero,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      RepaintBoundary(
        key: const Key('apple-visual'),
        child: MediaQuery(
          data: MediaQueryData(
            size: size,
            textScaler: TextScaler.linear(scale),
            padding: padding,
            viewPadding: padding,
            disableAnimations: true,
          ),
          child: const RyhzeApp(),
        ),
      ),
    );
    await tester.runAsync(() async {
      final context = tester.element(find.byType(RyhzeApp));
      for (final provider in [
        const ResizeImage(
          AssetImage('assets/brand/symbol-480.webp'),
          width: 96,
        ),
        const ResizeImage(
          AssetImage('assets/brand/symbol-480.webp'),
          width: 480,
        ),
        const ResizeImage(
          AssetImage('assets/brand/wordmark-348.webp'),
          width: 348,
        ),
      ]) {
        await precacheImage(provider, context);
      }
    });
    await tester.pumpAndSettle();
  }

  testWidgets('iOS compact Store appearance', (tester) async {
    await show(
      tester,
      const Size(390, 844),
      padding: const EdgeInsets.only(top: 47, bottom: 34),
    );
    expect(tester.takeException(), isNull);
    await expectGoldenWithDiagnostics(
      tester,
      find.byKey(const Key('apple-visual')),
      'goldens/monochrome-20261011/apple/ios-store.png',
    );
  }, variant: TargetPlatformVariant.only(TargetPlatform.iOS));

  testWidgets('iOS compact Library at enlarged text', (tester) async {
    await show(
      tester,
      const Size(320, 640),
      scale: 2,
      padding: const EdgeInsets.only(top: 47, bottom: 34),
    );
    await tester.tap(find.byKey(const Key('destination-library')));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    await expectGoldenWithDiagnostics(
      tester,
      find.byKey(const Key('apple-visual')),
      'goldens/monochrome-20261011/apple/ios-library-large-text.png',
    );
  }, variant: TargetPlatformVariant.only(TargetPlatform.iOS));

  testWidgets('macOS Command comma Settings appearance', (tester) async {
    await show(tester, const Size(1280, 800));
    await tester.sendKeyDownEvent(LogicalKeyboardKey.metaLeft);
    await tester.sendKeyEvent(LogicalKeyboardKey.comma);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.metaLeft);
    await tester.pumpAndSettle();
    expect(find.text('Reduce motion'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await expectGoldenWithDiagnostics(
      tester,
      find.byKey(const Key('apple-visual')),
      'goldens/monochrome-20261011/apple/macos-settings.png',
    );
  }, variant: TargetPlatformVariant.only(TargetPlatform.macOS));
}
