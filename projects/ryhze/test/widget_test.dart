import 'package:flutter/material.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';

void main() {
  test('catalog has the published game and film projects', () {
    expect(Catalog.games.length, 1);
    expect(Catalog.films, [Catalog.tommyRiley]);
    expect(Catalog.search(' LARCENOUS  drift '), [Catalog.larcenous]);
    expect(Catalog.search('Driving'), [Catalog.larcenous]);
    expect(Catalog.search('missing'), isEmpty);
  });

  Future<void> load(
    WidgetTester tester,
    Size size, {
    double textScale = 1,
    bool reduced = false,
    EdgeInsets viewInsets = EdgeInsets.zero,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MediaQuery(
        data: MediaQueryData(
          size: size,
          textScaler: TextScaler.linear(textScale),
          disableAnimations: reduced,
          viewInsets: viewInsets,
        ),
        child: const RyhzeApp(),
      ),
    );
    await tester.pumpAndSettle();
  }

  for (final size in [
    const Size(1440, 1000),
    const Size(1280, 800),
    const Size(800, 1280),
    const Size(390, 844),
    const Size(320, 640),
  ]) {
    testWidgets('responsive shell ${size.width} has real 16:9 frames', (
      tester,
    ) async {
      await load(tester, size);
      final art = tester.getSize(find.byKey(const Key('hero-art-16-9')));
      expect(art.width / art.height, closeTo(16 / 9, .001));
      expect(
        find.descendant(
          of: find.byKey(const Key('hero-surface')),
          matching: find.textContaining(RegExp(r'Larcenous\s+Driftscape')),
        ),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
      await tester.tap(find.byKey(const Key('category-films')));
      await tester.pumpAndSettle();
      expect(
        find.descendant(
          of: find.byKey(const Key('hero-surface')),
          matching: find.textContaining(RegExp(r'Tommy\s+Riley')),
        ),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
      await tester.tap(find.byKey(const Key('category-games')));
      await tester.pumpAndSettle();
      expect(
        find.descendant(
          of: find.byKey(const Key('hero-surface')),
          matching: find.textContaining(RegExp(r'Larcenous\s+Driftscape')),
        ),
        findsOneWidget,
      );
    });
  }

  testWidgets('search empty, result, detail, back and focus restore', (
    tester,
  ) async {
    await load(tester, const Size(1280, 900));
    await tester.tap(find.byKey(const Key('search-trigger')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('search-input')), 'nobody');
    await tester.pumpAndSettle();
    expect(
      find.text('No matching titles. Try another search.'),
      findsOneWidget,
    );
    await tester.enterText(find.byKey(const Key('search-input')), 'larcenous');
    await tester.pumpAndSettle();
    await tester.tap(find.text('Larcenous Driftscape').last);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('detail-back')), findsOneWidget);
    await tester.tap(find.byKey(const Key('detail-back')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('detail-back')), findsNothing);
    expect(FocusManager.instance.primaryFocus?.debugLabel, 'Search trigger');
    expect(tester.takeException(), isNull);
  });

  testWidgets('repeated opens, Escape and menu navigation are clean', (
    tester,
  ) async {
    await load(tester, const Size(1280, 900));
    for (var i = 0; i < 3; i++) {
      await tester.tap(find.byKey(const Key('hero-explore')));
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('detail-back')), findsNothing);
    }
    await tester.tap(find.byKey(const Key('menu-trigger')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Films').last);
    await tester.pumpAndSettle();
    expect(
      find.descendant(
        of: find.byKey(const Key('hero-surface')),
        matching: find.textContaining(RegExp(r'Tommy\s+Riley')),
      ),
      findsOneWidget,
    );
    expect(find.text('About this app'), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'favourite is shared across hero and dialog, with honest status',
    (tester) async {
      await load(tester, const Size(1440, 1000));
      await tester.tap(find.text('Favourites').first);
      await tester.pumpAndSettle();
      expect(find.text('Favourited'), findsOneWidget);
      await tester.tap(find.byKey(const Key('hero-explore')));
      await tester.pumpAndSettle();
      expect(find.text('Favourited').last, findsOneWidget);
      expect(find.text(Catalog.availability), findsOneWidget);
      expect(find.text('Play'), findsNothing);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('small screen with large text and reduced motion stays usable', (
    tester,
  ) async {
    await load(tester, const Size(320, 640), textScale: 1.6, reduced: true);
    await tester.tap(find.byKey(const Key('search-trigger')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('search-input')), findsOneWidget);
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });
  testWidgets('Films Explore works with reduced motion', (tester) async {
    await load(tester, const Size(390, 844), reduced: true);
    await tester.tap(find.byKey(const Key('category-films')));
    await tester.pumpAndSettle();
    await tester.ensureVisible(find.byKey(const Key('hero-explore')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('hero-explore')));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    expect(find.byKey(const Key('detail-back')), findsOneWidget);
    expect(find.text(Catalog.tommyRiley.availability), findsOneWidget);
    expect(find.text(Catalog.availability), findsNothing);
  });

  testWidgets('landscape menu and search remain cancellable', (tester) async {
    await load(tester, const Size(740, 320));
    await tester.tap(find.byKey(const Key('menu-trigger')));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('search-trigger')));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('search-input')), findsNothing);
  });

  testWidgets('large text title detail stays scrollable', (tester) async {
    await load(tester, const Size(320, 640), textScale: 1.6);
    await tester.tap(find.byKey(const Key('search-trigger')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('search-input')), 'larcenous');
    await tester.pumpAndSettle();
    await tester.tap(find.text('Larcenous Driftscape').last);
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    expect(find.byKey(const Key('detail-scroll')), findsOneWidget);
    await tester.tap(find.byKey(const Key('detail-back')));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });
  testWidgets(
    'keyboard shortcuts and synthetic controller keys navigate dialogs',
    (tester) async {
      await load(tester, const Size(1280, 900));
      await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
      await tester.sendKeyEvent(LogicalKeyboardKey.keyK);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('search-input')), findsOneWidget);
      await tester.sendKeyEvent(LogicalKeyboardKey.gameButtonB);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('search-input')), findsNothing);
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'Search trigger');
      await tester.sendKeyEvent(LogicalKeyboardKey.gameButtonA);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('search-input')), findsOneWidget);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      await tester.pumpAndSettle();
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'Menu trigger');
      await tester.sendKeyEvent(LogicalKeyboardKey.gameButtonA);
      await tester.pumpAndSettle();
      expect(find.text('About this app'), findsOneWidget);
      final firstFocus = FocusManager.instance.primaryFocus;
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.pumpAndSettle();
      expect(FocusManager.instance.primaryFocus, isNot(firstFocus));
      await tester.sendKeyEvent(LogicalKeyboardKey.gameButtonB);
      await tester.pumpAndSettle();
      expect(find.text('About this app'), findsNothing);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('toolbar hover preserves layout geometry', (tester) async {
    await load(tester, const Size(1280, 900));
    final target = find.byKey(const Key('search-trigger'));
    final before = tester.getRect(target);
    final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
    await mouse.addPointer(location: Offset.zero);
    await mouse.moveTo(tester.getCenter(target));
    await tester.pumpAndSettle();
    expect(tester.getRect(target), before);
    await mouse.removePointer();
    expect(tester.takeException(), isNull);
  });
  testWidgets('landscape search survives simulated keyboard insets', (
    tester,
  ) async {
    await load(
      tester,
      const Size(740, 320),
      viewInsets: const EdgeInsets.only(bottom: 180),
    );
    await tester.tap(find.byKey(const Key('search-trigger')));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('search-input')), 'unknown');
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('search-input')), findsNothing);
  });
}
