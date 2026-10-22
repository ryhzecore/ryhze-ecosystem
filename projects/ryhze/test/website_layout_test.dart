import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/entertainment_header.dart';
import 'package:ryhze/features/entertainment/widgets/store_sections.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final family in ['Inter', 'SpaceGrotesk']) {
      await (FontLoader(
        family,
      )..addFont(rootBundle.load('assets/fonts/$family.ttf'))).load();
    }
  });

  for (final sample in <(Size, double)>[
    (const Size(1280, 900), 1),
    (const Size(390, 1000), 2),
  ]) {
    testWidgets(
      'Exclusive adjoins the unified header at ${sample.$1.width}, text ${sample.$2}',
      (tester) async {
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = sample.$1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await tester.pumpWidget(
          MediaQuery(
            data: MediaQueryData(
              size: sample.$1,
              textScaler: TextScaler.linear(sample.$2),
              disableAnimations: true,
            ),
            child: const RyhzeApp(),
          ),
        );
        await tester.pumpAndSettle();
        for (final category in ['games', 'films', 'games']) {
          await tester.tap(find.byKey(Key('category-$category')));
          await tester.pumpAndSettle();
          final header = tester.getRect(find.byType(EntertainmentHeader));
          final chrome = tester.getRect(
            find.byKey(const Key('app-chrome-scroll')),
          );
          final hero = tester.getRect(find.byKey(const Key('hero-surface')));
          expect(chrome.bottom, closeTo(header.bottom, .01));
          expect(hero.top, closeTo(header.bottom, .01));
          expect(hero.width, sample.$1.width);
          for (final destination in ['store', 'library']) {
            expect(
              find.byKey(Key('destination-$destination')),
              category == 'games' ? findsOneWidget : findsNothing,
            );
          }
          expect(tester.takeException(), isNull);
        }
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
    );
  }

  // Measurements from the public website's 2026-10-11 CSS, not from app helpers.
  for (final sample in <(double, double, double)>[
    (320, 360, 320),
    (390, 360, 390),
    (700, 360, 700),
    (800, 400, 512),
    (1280, 469.76, 819.2),
    (1440, 528.48, 921.6),
    (2560, 580, 1638.4),
  ]) {
    testWidgets('Exclusive uses website proportions at ${sample.$1}', (
      tester,
    ) async {
      final width = sample.$1;
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = Size(width, 1000);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final favourite = ValueNotifier(false);
      addTearDown(favourite.dispose);
      var explored = 0;
      await tester.pumpWidget(
        MaterialApp(
          theme: RyhzeTokens.theme,
          home: Scaffold(
            body: SingleChildScrollView(
              child: StoreHero(
                width: width,
                gutter: StoreLayout.gutter(width),
                category: EntertainmentCategory.games,
                favourite: favourite,
                onFavourite: () => favourite.value = !favourite.value,
                onExplore: () => explored++,
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final hero = tester.getSize(find.byKey(const Key('hero-surface')));
      expect(hero.width, width);
      expect(hero.height, closeTo(sample.$2, 1));
      expect(
        tester.getSize(find.byKey(const Key('exclusive-art-region'))).width,
        closeTo(sample.$3, 1),
      );
      final media = tester.getSize(find.byKey(const Key('hero-art-16-9')));
      expect(media.width / media.height, closeTo(16 / 9, .001));
      final copy = tester.getRect(
        find.byKey(const Key('exclusive-copy-region')),
      );
      for (final key in ['hero-explore', 'hero-favourite']) {
        final rect = tester.getRect(find.byKey(Key(key)));
        expect(copy.contains(rect.topLeft), isTrue);
        expect(rect.right, lessThanOrEqualTo(copy.right));
        expect(rect.bottom, lessThanOrEqualTo(copy.bottom));
        expect(rect.height, greaterThanOrEqualTo(48));
      }
      await tester.tap(find.byKey(const Key('hero-explore')));
      await tester.tap(find.byKey(const Key('hero-favourite')));
      await tester.pumpAndSettle();
      expect(explored, 1);
      expect(favourite.value, isTrue);
      await tester.tap(find.byKey(const Key('hero-favourite')));
      await tester.pumpAndSettle();
      expect(favourite.value, isFalse);
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets(
    'Exclusive expands for enlarged text and retains keyboard actions',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(390, 844);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final favourite = ValueNotifier(false);
      addTearDown(favourite.dispose);
      var explored = 0;
      await tester.pumpWidget(
        MaterialApp(
          theme: RyhzeTokens.theme,
          home: MediaQuery(
            data: const MediaQueryData(
              size: Size(390, 844),
              textScaler: TextScaler.linear(2),
              disableAnimations: true,
            ),
            child: Scaffold(
              body: SingleChildScrollView(
                child: StoreHero(
                  width: 390,
                  gutter: 22,
                  category: EntertainmentCategory.games,
                  favourite: favourite,
                  onFavourite: () => favourite.value = !favourite.value,
                  onExplore: () => explored++,
                ),
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(
        tester.getSize(find.byKey(const Key('hero-surface'))).height,
        greaterThan(360),
      );
      final explore = find.byKey(const Key('hero-explore'));
      await tester.ensureVisible(explore);
      final detector = tester.widget<FocusableActionDetector>(
        find.descendant(
          of: explore,
          matching: find.byType(FocusableActionDetector),
        ),
      );
      Focus.of(
        tester.element(
          find
              .descendant(of: explore, matching: find.byType(GestureDetector))
              .first,
        ),
      ).requestFocus();
      await tester.pump();
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      expect(detector.enabled, isTrue);
      await tester.pumpAndSettle();
      expect(explored, 1);
      expect(tester.takeException(), isNull);
    },
  );

  test('catalogue card dimensions follow website breakpoints', () {
    for (final sample in <(double, double, int)>[
      (390, 346, 1),
      (700, 379.2, 1),
      (800, 265.6, 2),
      (1280, 440.32, 2),
      (1440, 498.56, 2),
    ]) {
      expect(StoreLayout.cardWidth(sample.$1), closeTo(sample.$2, .001));
      expect(StoreLayout.columns(sample.$1), sample.$3);
    }
  });
}
