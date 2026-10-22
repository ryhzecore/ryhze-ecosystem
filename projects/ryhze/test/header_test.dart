import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/entertainment_header.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final family in ['Inter', 'SpaceGrotesk']) {
      await (FontLoader(
        family,
      )..addFont(rootBundle.load('assets/fonts/$family.ttf'))).load();
    }
  });

  Future<void> load(
    WidgetTester tester,
    Size size, {
    bool reduced = false,
    double scale = 1,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MediaQuery(
        data: MediaQueryData(
          size: size,
          disableAnimations: reduced,
          textScaler: TextScaler.linear(scale),
        ),
        child: const RyhzeApp(),
      ),
    );
    await tester.pumpAndSettle();
  }

  Rect rect(WidgetTester tester, String key) =>
      tester.getRect(find.byKey(Key(key)));

  const navigationKeys = [
    'category-games',
    'category-films',
    'destination-store',
    'destination-library',
    'search-trigger',
    'menu-trigger',
  ];

  for (final direction in [TextDirection.ltr, TextDirection.rtl]) {
    testWidgets('category ink follows the moving white pill in $direction', (
      tester,
    ) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(600, 360);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final searchFocus = FocusNode(), menuFocus = FocusNode();
      addTearDown(searchFocus.dispose);
      addTearDown(menuFocus.dispose);
      var category = EntertainmentCategory.games;
      var reduced = false;
      late StateSetter update;
      const pixelsKey = Key('chooser-contrast-pixels');
      await tester.pumpWidget(
        MaterialApp(
          theme: RyhzeTokens.theme,
          home: StatefulBuilder(
            builder: (context, setState) {
              update = setState;
              return MediaQuery(
                data: MediaQueryData(
                  size: const Size(600, 360),
                  disableAnimations: reduced,
                ),
                child: Directionality(
                  textDirection: direction,
                  child: Scaffold(
                    body: Align(
                      alignment: Alignment.topCenter,
                      child: RepaintBoundary(
                        key: pixelsKey,
                        child: EntertainmentHeader(
                          width: 600,
                          gutter: 20,
                          category: category,
                          onCategory: (value) =>
                              setState(() => category = value),
                          onSearch: () {},
                          searchLabel: 'Search',
                          onMenuChoice: (_) {},
                          searchFocus: searchFocus,
                          menuFocus: menuFocus,
                        ),
                      ),
                    ),
                  ),
                ),
              );
            },
          ),
        ),
      );
      await tester.pumpAndSettle();
      Future<void> checkInk() async {
        final origin = tester.getTopLeft(find.byKey(pixelsKey));
        final pill = rect(tester, 'category-indicator').shift(-origin);
        final boundary = tester.renderObject<RenderRepaintBoundary>(
          find.byKey(pixelsKey),
        );
        final raster = (await tester.runAsync(() async {
          final image = await boundary.toImage(pixelRatio: 1);
          final bytes = await image.toByteData(
            format: ui.ImageByteFormat.rawRgba,
          );
          final width = image.width;
          image.dispose();
          return (width, bytes!);
        }))!;
        var checked = 0;
        for (final name in ['games', 'films']) {
          final label = tester
              .getRect(
                find.descendant(
                  of: find.byKey(Key('category-$name')),
                  matching: find.byType(Text),
                ),
              )
              .shift(-origin)
              .deflate(1);
          final covered =
              pill.deflate(3).contains(label.topLeft) &&
              pill.deflate(3).contains(label.bottomRight);
          final uncovered = !pill.inflate(3).overlaps(label);
          if (!covered && !uncovered) continue;
          var darkest = 255, lightest = 0;
          for (var y = label.top.ceil(); y < label.bottom.floor(); y++) {
            for (var x = label.left.ceil(); x < label.right.floor(); x++) {
              final value = raster.$2.getUint8((y * raster.$1 + x) * 4);
              if (value < darkest) darkest = value;
              if (value > lightest) lightest = value;
            }
          }
          if (covered) {
            expect(
              darkest,
              lessThan(90),
              reason: '$name needs dark ink on the white pill',
            );
          } else {
            expect(
              lightest,
              greaterThan(150),
              reason: '$name needs light ink on the dark surface',
            );
          }
          checked++;
        }
        expect(checked, greaterThan(0));
      }

      await tester.tap(find.byKey(const Key('category-films')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 25));
      await checkInk();
      await tester.pumpAndSettle();
      final films = rect(tester, 'category-films');
      final games = rect(tester, 'category-games');
      final gesture = await tester.startGesture(films.center);
      await gesture.moveBy(Offset((games.center.dx - films.center.dx) * .9, 0));
      await tester.pump();
      expect(category, EntertainmentCategory.films);
      await checkInk();
      await gesture.cancel();
      await tester.pumpAndSettle();
      expect(category, EntertainmentCategory.films);
      expect(rect(tester, 'category-indicator'), films);
      update(() => reduced = true);
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('category-games')));
      await tester.pump();
      expect(rect(tester, 'category-indicator'), games);
      await checkInk();
      expect(find.text('Games'), findsOneWidget);
      expect(find.text('Films'), findsOneWidget);
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets('desktop destinations share the top header row in order', (
    tester,
  ) async {
    await load(tester, const Size(1280, 900));
    final header = find.byType(EntertainmentHeader);
    final brand = rect(tester, 'header-brand');
    var previous = brand;
    for (final key in [
      'category-chooser',
      'destination-store',
      'destination-library',
      'search-trigger',
      'menu-trigger',
    ]) {
      final target = find.byKey(Key(key));
      expect(find.descendant(of: header, matching: target), findsOneWidget);
      final bounds = tester.getRect(target);
      expect(bounds.left, greaterThanOrEqualTo(previous.right));
      expect(bounds.center.dy, closeTo(brand.center.dy, .01));
      previous = bounds;
    }
    for (final key in ['search-trigger', 'menu-trigger']) {
      final bounds = rect(tester, key);
      expect(bounds.width, bounds.height);
    }
    expect(tester.takeException(), isNull);
  }, variant: TargetPlatformVariant.only(TargetPlatform.windows));

  for (final sample in <(Size, double)>[
    (const Size(320, 640), 1),
    (const Size(761, 900), 1),
    (const Size(390, 844), 2),
    (const Size(850, 900), 1.6),
  ]) {
    testWidgets(
      'header navigation fits ${sample.$1.width} at text ${sample.$2}',
      (tester) async {
        await load(tester, sample.$1, scale: sample.$2, reduced: true);
        final header = find.byType(EntertainmentHeader);
        final headerBounds = tester.getRect(header);
        final measured = <Rect>[];
        for (final key in navigationKeys) {
          final target = find.byKey(Key(key));
          expect(target, findsOneWidget);
          expect(find.descendant(of: header, matching: target), findsOneWidget);
          final bounds = tester.getRect(target);
          expect(bounds.width, greaterThanOrEqualTo(48));
          expect(bounds.height, greaterThanOrEqualTo(48));
          expect(headerBounds.inflate(.01).contains(bounds.topLeft), isTrue);
          expect(
            headerBounds.inflate(.01).contains(bounds.bottomRight),
            isTrue,
          );
          for (final previous in measured) {
            expect(bounds.overlaps(previous), isFalse, reason: key);
          }
          measured.add(bounds);
          if (!key.endsWith('-trigger')) {
            final label = tester.getRect(
              find.descendant(of: target, matching: find.byType(Text)),
            );
            expect(bounds.inflate(.01).contains(label.topLeft), isTrue);
            expect(bounds.inflate(.01).contains(label.bottomRight), isTrue);
          }
        }
        for (final key in navigationKeys) {
          final target = find.byKey(Key(key));
          await tester.ensureVisible(target);
          await tester.pumpAndSettle();
          expect(target.hitTestable(), findsOneWidget);
        }
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
    );
  }

  testWidgets('keyboard destination changes preserve header bounds', (
    tester,
  ) async {
    await load(tester, const Size(1280, 900), reduced: true);
    final originalHeader = tester.getRect(find.byType(EntertainmentHeader));
    final originalBounds = {
      for (final key in navigationKeys) key: rect(tester, key),
    };
    for (final destination in ['library', 'store', 'library', 'store']) {
      final target = find.byKey(Key('destination-$destination'));
      Focus.of(
        tester.element(
          find
              .descendant(of: target, matching: find.byType(GestureDetector))
              .first,
        ),
      ).requestFocus();
      await tester.pump();
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await tester.pumpAndSettle();
      expect(tester.widget<RyhzeControl>(target).selected, isTrue);
      final other = destination == 'store' ? 'library' : 'store';
      expect(
        tester
            .widget<RyhzeControl>(find.byKey(Key('destination-$other')))
            .selected,
        isFalse,
      );
      expect(
        find.byKey(const Key('hero-surface')),
        destination == 'store' ? findsOneWidget : findsNothing,
      );
      expect(tester.getRect(find.byType(EntertainmentHeader)), originalHeader);
      for (final key in navigationKeys) {
        expect(rect(tester, key), originalBounds[key], reason: key);
      }
      expect(tester.takeException(), isNull);
    }
  }, variant: TargetPlatformVariant.only(TargetPlatform.windows));

  testWidgets('Deck category chooser stays beside the brand', (tester) async {
    await load(tester, const Size(1280, 800));
    final brand = rect(tester, 'header-brand');
    final chooser = rect(tester, 'category-chooser');
    expect(chooser.left - brand.right, inInclusiveRange(12, 24));
    expect(chooser.center.dy, closeTo(brand.center.dy, .01));
    expect(rect(tester, 'search-trigger').left, greaterThan(chooser.right));
  }, variant: TargetPlatformVariant.only(TargetPlatform.linux));

  for (final size in [
    const Size(1280, 800),
    const Size(390, 844),
    const Size(320, 640),
    const Size(740, 320),
  ]) {
    for (final reduced in [false, true]) {
      testWidgets('category pill bounds ${size.width}, reduced=$reduced', (
        tester,
      ) async {
        await load(tester, size, reduced: reduced);
        final shell = rect(tester, 'category-chooser');
        final games = rect(tester, 'category-games');
        final films = rect(tester, 'category-films');
        final initial = rect(tester, 'category-indicator');
        expect(initial, games);
        expect(initial.left - shell.left, greaterThanOrEqualTo(5));
        for (final category in ['films', 'games']) {
          await tester.tap(find.byKey(Key('category-$category')));
          await tester.pump();
          for (var frame = 0; frame < 14; frame++) {
            final pill = rect(tester, 'category-indicator');
            expect(pill.left, greaterThanOrEqualTo(games.left - .01));
            expect(pill.right, lessThanOrEqualTo(films.right + .01));
            expect(pill.width, closeTo(games.width, .01));
            expect(pill.height, closeTo(games.height, .01));
            if (reduced) expect(pill, rect(tester, 'category-$category'));
            await tester.pump(const Duration(milliseconds: 50));
          }
          await tester.pumpAndSettle();
          expect(
            rect(tester, 'category-indicator'),
            rect(tester, 'category-$category'),
          );
          expect(tester.takeException(), isNull);
        }
        await tester.tap(find.byKey(const Key('category-films')));
        await tester.pumpAndSettle();
        expect(
          shell.right - rect(tester, 'category-indicator').right,
          closeTo(initial.left - shell.left, .01),
        );
      }, variant: TargetPlatformVariant.only(TargetPlatform.linux));
    }
  }

  testWidgets('large text keeps equal usable category targets', (tester) async {
    await load(tester, const Size(320, 640), scale: 1.6, reduced: true);
    for (final category in ['games', 'films']) {
      await tester.tap(find.byKey(Key('category-$category')));
      await tester.pumpAndSettle();
      final button = rect(tester, 'category-$category');
      final text = tester.getRect(
        find.descendant(
          of: find.byKey(Key('category-$category')),
          matching: find.byType(Text),
        ),
      );
      expect(button.contains(text.topLeft), isTrue);
      expect(button.contains(text.bottomRight), isTrue);
      expect(rect(tester, 'category-indicator'), button);
      expect(tester.takeException(), isNull);
    }
  }, variant: TargetPlatformVariant.only(TargetPlatform.linux));
}
