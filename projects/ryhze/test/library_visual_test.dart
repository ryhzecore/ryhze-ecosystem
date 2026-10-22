import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/features/library/model/library_entry.dart';

import 'support/golden_diagnostics.dart';

// These are new candidate baselines for the approved Library interface. They
// use real app navigation and the explicitly selected fictional Demo Library;
// no injected repository claims verified ownership or functioning providers.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
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
    ('desktop', const Size(1440, 1000), 1.0, false),
    ('deck', const Size(1280, 800), 1.0, false),
    ('phone', const Size(390, 844), 1.0, false),
    ('phone-large', const Size(320, 640), 1.6, true),
    ('phone-text-2', const Size(320, 800), 2.0, true),
  ]) {
    testWidgets('${record.$1} truthful Library visual evidence', (
      tester,
    ) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = record.$2;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MediaQuery(
          data: MediaQueryData(
            size: record.$2,
            textScaler: TextScaler.linear(record.$3),
            disableAnimations: record.$4,
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
      await tester.tap(find.byKey(const Key('destination-library')));
      await tester.pumpAndSettle();

      final compact = record.$2.width < 820 || record.$3 > 1.3;
      final collection = find.byKey(
        Key(compact ? 'library-list' : 'library-grid'),
      );
      final collectionScrollable = find.descendant(
        of: collection,
        matching: _verticalScrollable(),
      );
      await _reveal(
        tester,
        find.text('Library unavailable'),
        collectionScrollable,
      );
      expect(find.text('No owned games'), findsNothing);
      expect(find.byKey(const Key('library-demo-indicator')), findsNothing);
      await _capture(tester, '${record.$1}-unavailable');

      await tester.ensureVisible(find.byKey(const Key('library-demo-toggle')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('library-demo-toggle')));
      await tester.pumpAndSettle();
      final aurora = _entry('aurora-orchard', compact: compact);
      if (compact) await _reveal(tester, aurora, collectionScrollable);
      _expectDemoBanner(tester);
      expect(aurora.hitTestable(), findsOneWidget);
      await _capture(tester, '${record.$1}-demo-collection');

      await tester.tap(aurora);
      await tester.pumpAndSettle();
      final details = find.byKey(const Key('library-detail'));
      final detailScrollable = find.descendant(
        of: find.byKey(const Key('library-detail-scroll')),
        matching: _verticalScrollable(),
      );
      expect(
        find.descendant(of: details, matching: find.text('Aurora Orchard')),
        findsOneWidget,
      );
      _expectDemoBanner(tester);
      await _capture(tester, '${record.$1}-aurora-detail-top');

      await _reveal(tester, find.text('Ownership'), detailScrollable);
      expect(find.text('Demo only · Not ownership evidence'), findsOneWidget);
      _expectDemoBanner(tester);
      await _capture(tester, '${record.$1}-aurora-detail-ownership');

      await _reveal(tester, find.text('Installation'), detailScrollable);
      expect(find.text('Simulated · Installed'), findsOneWidget);
      _expectDemoBanner(tester);
      await _capture(tester, '${record.$1}-aurora-detail-installation');

      await _reveal(
        tester,
        find.byKey(const Key('library-launch')),
        detailScrollable,
      );
      for (final key in ['library-launch', 'library-install']) {
        expect(
          tester.widget<RyhzeControl>(find.byKey(Key(key))).onPressed,
          isNull,
        );
      }
      _expectDemoBanner(tester);
      await _capture(tester, '${record.$1}-aurora-detail-actions');

      // A second title demonstrates that details and installation status are
      // selected-entry specific, rather than a fixed Store detail panel.
      if (record.$1 == 'desktop') {
        await _reveal(
          tester,
          find.byKey(const Key('library-back')),
          detailScrollable,
          delta: -180,
        );
        await tester.tap(find.byKey(const Key('library-back')));
        await tester.pumpAndSettle();
        final copper = _entry('copper-compass', compact: compact);
        await _reveal(tester, copper, collectionScrollable);
        await tester.tap(copper);
        await tester.pumpAndSettle();
        expect(
          find.descendant(of: details, matching: find.text('Copper Compass')),
          findsOneWidget,
        );
        _expectDemoBanner(tester);
        await _capture(tester, '${record.$1}-copper-detail-top');
        await _reveal(tester, find.text('Installation'), detailScrollable);
        expect(find.text('Simulated · Not installed'), findsOneWidget);
        _expectDemoBanner(tester);
        await _capture(tester, '${record.$1}-copper-detail-installation');
      }
      expect(tester.takeException(), isNull);
    }, variant: TargetPlatformVariant.only(TargetPlatform.linux));
  }
}

Finder _entry(String id, {required bool compact}) {
  final identity = LibraryIdentity(providerId: 'ryhze-demo', titleId: id);
  return find.byKey(Key('library-${compact ? 'row' : 'card'}-$identity'));
}

Finder _verticalScrollable() => find.byWidgetPredicate(
  (widget) =>
      widget is Scrollable &&
      (widget.axisDirection == AxisDirection.down ||
          widget.axisDirection == AxisDirection.up),
);

Future<void> _reveal(
  WidgetTester tester,
  Finder target,
  Finder scrollable, {
  double delta = 180,
}) async {
  await tester.scrollUntilVisible(target, delta, scrollable: scrollable);
  await tester.pumpAndSettle();
  expect(target.hitTestable(), findsOneWidget);
  expect(tester.takeException(), isNull);
}

void _expectDemoBanner(WidgetTester tester) {
  final banner = find.byKey(const Key('library-demo-indicator'));
  expect(banner.hitTestable(), findsOneWidget);
  expect(
    find.text('Demo Library · Fictional games and simulated statuses'),
    findsOneWidget,
  );
}

Future<void> _capture(WidgetTester tester, String name) async {
  expect(tester.takeException(), isNull);
  await expectGoldenWithDiagnostics(
    tester,
    find.byType(RyhzeApp),
    'goldens/monochrome-20261011/v005-library/$name.png',
  );
  expect(tester.takeException(), isNull);
}
