import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';

import 'support/golden_diagnostics.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  goldenFileComparator = _PreserveHistoricalGoldens(goldenFileComparator);
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
  for (final record in [
    ('desktop', const Size(1440, 1000), 1.0),
    ('phone', const Size(390, 844), 1.0),
    ('deck', const Size(1280, 800), 1.0),
    ('phone-large', const Size(320, 640), 1.6),
    ('landscape', const Size(740, 320), 1.0),
  ]) {
    testWidgets('${record.$1} visual evidence', (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = record.$2;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MediaQuery(
          data: MediaQueryData(
            size: record.$2,
            textScaler: TextScaler.linear(record.$3),
          ),
          child: const RyhzeApp(),
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
      await expectGoldenWithDiagnostics(
        tester,
        find.byType(RyhzeApp),
        'goldens/monochrome-20261011/v005-store/${record.$1}-games.png',
      );
      if (record.$2.width < 1000) {
        await tester.ensureVisible(find.byKey(const Key('hero-explore')));
        await tester.pumpAndSettle();
        await expectGoldenWithDiagnostics(
          tester,
          find.byType(RyhzeApp),
          'goldens/monochrome-20261011/v005-store/${record.$1}-hero-controls.png',
        );
      }
      await tester.drag(
        find.byKey(const Key('catalog-scroll')),
        const Offset(0, -650),
      );
      await tester.pumpAndSettle();
      await expectGoldenWithDiagnostics(
        tester,
        find.byType(RyhzeApp),
        'goldens/monochrome-20261011/v005-store/${record.$1}-catalog.png',
      );
      await tester.tap(find.byKey(const Key('search-trigger')));
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byKey(const Key('search-input')),
        'larcenous',
      );
      await tester.pumpAndSettle();
      await expectGoldenWithDiagnostics(
        tester,
        find.byType(RyhzeApp),
        'goldens/monochrome-20261011/v005-store/${record.$1}-search.png',
      );
      await tester.tap(find.text('Larcenous Driftscape').last);
      await tester.pumpAndSettle();
      await expectGoldenWithDiagnostics(
        tester,
        find.byType(RyhzeApp),
        'goldens/monochrome-20261011/v005-store/${record.$1}-detail.png',
      );
      await tester.tap(find.byKey(const Key('detail-back')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('category-films')));
      await tester.pumpAndSettle();
      await expectGoldenWithDiagnostics(
        tester,
        find.byType(RyhzeApp),
        'goldens/monochrome-20261011/${record.$1}-films.png',
      );
      await tester.ensureVisible(find.byKey(const Key('hero-explore')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('hero-explore')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('detail-back')), findsOneWidget);
      await expectGoldenWithDiagnostics(
        tester,
        find.byType(RyhzeApp),
        'goldens/monochrome-20261011/films/${record.$1}-detail.png',
      );
      await tester.tap(find.byKey(const Key('detail-back')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('menu-trigger')));
      await tester.pumpAndSettle();
      await expectGoldenWithDiagnostics(
        tester,
        find.byType(RyhzeApp),
        'goldens/monochrome-20261011/v060-shell/${record.$1}-menu.png',
      );
      if (record.$1 == 'deck') {
        await tester.tap(find.byKey(const Key('menu-about')));
        await tester.pumpAndSettle();
        expect(find.text('Version 0.6.0'), findsOneWidget);
        await expectGoldenWithDiagnostics(
          tester,
          find.byType(RyhzeApp),
          'goldens/monochrome-20261011/v060-shell/deck-about.png',
        );
      }
      expect(tester.takeException(), isNull);
    }, variant: TargetPlatformVariant.only(TargetPlatform.linux));
  }
}

// New navigation gets distinct candidate references. Historical images remain
// comparisons even during an explicit update of the new candidate references.
class _PreserveHistoricalGoldens extends GoldenFileComparator {
  _PreserveHistoricalGoldens(this.delegate);
  final GoldenFileComparator delegate;

  @override
  Future<bool> compare(Uint8List imageBytes, Uri golden) =>
      delegate.compare(imageBytes, golden);

  @override
  Uri getTestUri(Uri key, int? version) => delegate.getTestUri(key, version);

  @override
  Future<void> update(Uri golden, Uint8List imageBytes) async {
    if (golden.path.startsWith('goldens/monochrome-20261011/v060-shell/')) {
      await delegate.update(golden, imageBytes);
      return;
    }
    if (!await delegate.compare(imageBytes, golden)) {
      throw TestFailure('Historical golden must remain unchanged: $golden');
    }
  }
}
