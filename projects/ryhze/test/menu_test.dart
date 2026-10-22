import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/design/header_menu.dart';
import 'package:ryhze/features/entertainment/entertainment_screen.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final name in ['Inter', 'SpaceGrotesk']) {
      final loader = FontLoader(name)
        ..addFont(rootBundle.load('assets/fonts/$name.ttf'));
      await loader.load();
    }
  });

  for (final size in [
    const Size(1280, 800),
    const Size(390, 844),
    const Size(320, 640),
    const Size(740, 320),
  ]) {
    for (final scale in [1.0, 1.6]) {
      testWidgets('anchored menu ${size.width}, text=$scale', (tester) async {
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = size;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await tester.pumpWidget(
          MediaQuery(
            data: MediaQueryData(
              size: size,
              textScaler: TextScaler.linear(scale),
              disableAnimations: scale > 1,
            ),
            child: const RyhzeApp(),
          ),
        );
        await tester.pumpAndSettle();
        final trigger = find.byKey(const Key('menu-trigger'));
        final triggerRect = tester.getRect(trigger);
        expect(triggerRect.width, triggerRect.height);
        expect(
          tester.widget<MenuToggleIcon>(find.byType(MenuToggleIcon)).isOpen,
          isFalse,
        );

        Future<void> open() async {
          await tester.tap(trigger);
          await tester.pumpAndSettle();
          expect(find.byType(MenuItemButton), findsNWidgets(4));
          expect(
            tester.widget<MenuToggleIcon>(find.byType(MenuToggleIcon)).isOpen,
            isTrue,
          );
          expect(find.text('Ryhze'), findsNothing);
          expect(find.byIcon(Icons.close_rounded), findsNothing);
          expect(
            find.text(
              'Accounts, owned libraries and launcher connections are being developed.',
            ),
            findsNothing,
          );
          final first = tester.getRect(find.byKey(const Key('menu-games')));
          final lastFinder = find.byKey(const Key('menu-workspace'));
          await tester.ensureVisible(lastFinder);
          await tester.pumpAndSettle();
          final last = tester.getRect(lastFinder);
          expect(first.top, greaterThanOrEqualTo(triggerRect.bottom));
          expect(first.left, greaterThanOrEqualTo(8));
          expect(last.right, lessThanOrEqualTo(size.width - 8));
          expect(last.right, closeTo(triggerRect.right - 22, 2));
          expect(last.bottom, lessThanOrEqualTo(size.height - 8));
          expect(tester.takeException(), isNull);
          await tester.ensureVisible(find.byKey(const Key('menu-games')));
          await tester.pumpAndSettle();
        }

        await open();
        await tester.tap(trigger);
        await tester.pumpAndSettle();
        expect(find.byType(MenuItemButton), findsNothing);
        await open();
        await tester.tapAt(Offset(4, size.height - 4));
        await tester.pumpAndSettle();
        expect(find.byType(MenuItemButton), findsNothing);
        expect(FocusManager.instance.primaryFocus?.debugLabel, 'Menu trigger');
        await open();
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byType(MenuItemButton), findsNothing);
        expect(FocusManager.instance.primaryFocus?.debugLabel, 'Menu trigger');
        await open();
        await tester.sendKeyEvent(
          LogicalKeyboardKey.gameButtonB,
          platform: 'android',
        );
        await tester.pumpAndSettle();
        expect(find.byType(MenuItemButton), findsNothing);
        await open();
        await tester.tap(find.byKey(const Key('menu-films')));
        await tester.pumpAndSettle();
        expect(
          find.descendant(
            of: find.byKey(const Key('hero-surface')),
            matching: find.textContaining(RegExp(r'Tommy\s+Riley')),
          ),
          findsOneWidget,
        );
        expect(find.byType(MenuItemButton), findsNothing);
        await open();
        await tester.tap(find.byKey(const Key('menu-games')));
        await tester.pumpAndSettle();
        expect(
          find.descendant(
            of: find.byKey(const Key('hero-surface')),
            matching: find.textContaining(RegExp(r'Larcenous\s+Driftscape')),
          ),
          findsOneWidget,
        );
        await open();
        await tester.tap(find.byKey(const Key('menu-about')));
        await tester.pumpAndSettle();
        expect(find.text('Entertainment,\ntaking shape.'), findsOneWidget);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(FocusManager.instance.primaryFocus?.debugLabel, 'Menu trigger');
        expect(tester.takeException(), isNull);
      }, variant: TargetPlatformVariant.only(TargetPlatform.linux));
    }
  }

  for (final scenario in [
    (
      name: 'desktop to narrow phone and back',
      layouts: [(1280.0, 1.0), (320.0, 1.0), (1280.0, 1.0)],
    ),
    (
      name: 'phone to desktop and back',
      layouts: [(390.0, 1.0), (1280.0, 1.0), (390.0, 1.0)],
    ),
    (
      name: 'text scale changes at desktop width',
      layouts: [(1280.0, 1.0), (1280.0, 1.6), (1280.0, 1.0)],
    ),
  ]) {
    for (final openBeforeReflow in [false, true]) {
      testWidgets('menu survives ${scenario.name}, open=$openBeforeReflow', (
        tester,
      ) async {
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        Future<void> reflow((double, double) layout) async {
          final size = Size(layout.$1, 844);
          tester.view.physicalSize = size;
          // Keep the same widget tree and app state while its metrics change.
          await tester.pumpWidget(
            MediaQuery(
              data: MediaQueryData(
                size: size,
                textScaler: TextScaler.linear(layout.$2),
              ),
              child: const RyhzeApp(),
            ),
          );
          await tester.pumpAndSettle();
          expect(tester.takeException(), isNull);
          expect(
            MediaQuery.textScalerOf(
              tester.element(find.byType(EntertainmentScreen)),
            ).scale(1),
            layout.$2,
          );
        }

        final trigger = find.byKey(const Key('menu-trigger'));
        Future<void> openMenu() async {
          await tester.tap(trigger);
          await tester.pumpAndSettle();
          expect(find.byType(MenuItemButton), findsNWidgets(4));
          expect(
            tester.widget<MenuToggleIcon>(find.byType(MenuToggleIcon)).isOpen,
            isTrue,
          );
          expect(tester.takeException(), isNull);
        }

        await reflow(scenario.layouts.first);
        final screenState = tester.state(find.byType(EntertainmentScreen));
        final triggerFocus = tester
            .widget<HeaderMenu>(find.byType(HeaderMenu))
            .focusNode;
        await tester.tap(find.byKey(const Key('category-films')));
        await tester.pumpAndSettle();

        for (final layout in scenario.layouts.skip(1)) {
          if (openBeforeReflow) await openMenu();
          await reflow(layout);
          expect(
            tester.state(find.byType(EntertainmentScreen)),
            same(screenState),
          );
          expect(
            tester.widget<HeaderMenu>(find.byType(HeaderMenu)).focusNode,
            same(triggerFocus),
          );
          expect(
            find.descendant(
              of: find.byKey(const Key('hero-surface')),
              matching: find.textContaining(RegExp(r'Tommy\s+Riley')),
            ),
            findsOneWidget,
          );
          // A reparented menu may close; one that remains open must dismiss.
          if (tester
              .widget<MenuToggleIcon>(find.byType(MenuToggleIcon))
              .isOpen) {
            await tester.sendKeyEvent(LogicalKeyboardKey.escape);
            await tester.pumpAndSettle();
          }
          expect(find.byType(MenuItemButton), findsNothing);
          await openMenu();
          await tester.sendKeyEvent(LogicalKeyboardKey.escape);
          await tester.pumpAndSettle();
          expect(find.byType(MenuItemButton), findsNothing);
          expect(FocusManager.instance.primaryFocus, same(triggerFocus));
          expect(tester.takeException(), isNull);
        }
      }, variant: TargetPlatformVariant.only(TargetPlatform.linux));
    }
  }
}
