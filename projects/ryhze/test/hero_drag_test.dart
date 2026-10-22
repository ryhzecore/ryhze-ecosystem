import 'package:flutter/material.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/features/entertainment/widgets/title_detail.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final name in ['Inter', 'SpaceGrotesk']) {
      final loader = FontLoader(name)
        ..addFont(rootBundle.load('assets/fonts/$name.ttf'));
      await loader.load();
    }
  });
  Future<void> load(WidgetTester tester, Size size, double scale) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MediaQuery(
        data: MediaQueryData(size: size, textScaler: TextScaler.linear(scale)),
        child: const RyhzeApp(),
      ),
    );
    await tester.pumpAndSettle();
  }

  for (final size in [
    const Size(320, 640),
    const Size(390, 844),
    const Size(740, 320),
  ]) {
    for (final scale in [1.0, 1.6]) {
      testWidgets(
        'hero stays inside artwork and controls work $size text=$scale',
        (tester) async {
          await load(tester, size, scale);
          final frame = find.byKey(const Key('hero-art-16-9'));
          final surface = find.byKey(const Key('hero-surface'));
          expect(tester.getSize(frame).aspectRatio, closeTo(16 / 9, .001));
          expect(
            find.descendant(of: surface, matching: find.byType(Scrollable)),
            findsNothing,
          );
          expect(find.byKey(const Key('pre-alpha-badge')), findsOneWidget);
          for (final key in ['hero-explore', 'hero-favourite']) {
            final target = find.byKey(Key(key));
            final rect = tester.getRect(target);
            expect(tester.getRect(surface).contains(rect.topLeft), isTrue);
            expect(
              tester
                  .getRect(surface)
                  .contains(rect.bottomRight - const Offset(.01, .01)),
              isTrue,
            );
            expect(rect.height, greaterThanOrEqualTo(52));
            await tester.ensureVisible(target);
            await tester.pumpAndSettle();
            expect(target.hitTestable(), findsOneWidget);
          }
          final favourite = find.byKey(const Key('hero-favourite'));
          await tester.tap(favourite);
          await tester.pumpAndSettle();
          expect(find.byTooltip('Remove favourite'), findsOneWidget);
          // Real fonts make this an actual geometry check, rather than Ahem metrics.
          debugPrint(
            'HERO_GEOMETRY width=${size.width} scale=$scale '
            'art=${tester.getSize(frame).height} surface=${tester.getSize(surface).height}',
          );
          final explore = find.byKey(const Key('hero-explore'));
          await tester.ensureVisible(explore);
          await tester.pumpAndSettle();
          expect(explore.hitTestable(), findsOneWidget);
          Future<void> openDetail(bool saved) async {
            await tester.tap(explore);
            await tester.pumpAndSettle();
            expect(find.byKey(const Key('detail-back')), findsOneWidget);
            final detail = find.byType(TitleDetail);
            expect(tester.widget<TitleDetail>(detail).favourite.value, saved);
            expect(
              MediaQuery.textScalerOf(tester.element(detail)).scale(1),
              scale,
            );
            expect(tester.takeException(), isNull);
            await tester.sendKeyEvent(LogicalKeyboardKey.escape);
            await tester.pumpAndSettle();
            expect(detail, findsNothing);
            expect(tester.takeException(), isNull);
          }

          await openDetail(true);
          if (size.width == 740 && scale == 1.6) {
            // Cover both favourite states in the formerly excluded layout.
            await tester.ensureVisible(favourite);
            await tester.pumpAndSettle();
            await tester.tap(favourite);
            await tester.pumpAndSettle();
            expect(find.byTooltip('Add favourite'), findsOneWidget);
            await tester.ensureVisible(explore);
            await tester.pumpAndSettle();
            await openDetail(false);
          }
          await tester.tap(find.byKey(const Key('category-films')));
          await tester.pumpAndSettle();
          expect(tester.getSize(frame).aspectRatio, closeTo(16 / 9, .001));
          expect(
            find.descendant(of: surface, matching: find.byType(Scrollable)),
            findsNothing,
          );
          await tester.ensureVisible(explore);
          await tester.pumpAndSettle();
          expect(explore.hitTestable(), findsOneWidget);
          final detail = find.byType(TitleDetail);
          await tester.tap(explore);
          await tester.pumpAndSettle();
          expect(detail, findsOneWidget);
          expect(find.textContaining('Tommy Riley'), findsWidgets);
          await tester.sendKeyEvent(LogicalKeyboardKey.escape);
          await tester.pumpAndSettle();
          expect(detail, findsNothing);
          expect(tester.takeException(), isNull);
        },
        variant: TargetPlatformVariant.only(TargetPlatform.linux),
      );
    }
  }
  for (final kind in [PointerDeviceKind.touch, PointerDeviceKind.mouse]) {
    testWidgets(
      'category drag follows pointer, snaps both ways and cancels $kind',
      (tester) async {
        await load(tester, const Size(390, 844), 1);
        final games = find.byKey(const Key('category-games')),
            films = find.byKey(const Key('category-films')),
            pill = find.byKey(const Key('category-indicator'));
        final start = tester.getCenter(games), end = tester.getCenter(films);
        var gesture = await tester.startGesture(start, kind: kind);
        await gesture.moveBy(const Offset(20, 0));
        await tester.pump();
        await gesture.moveBy(const Offset(25, 0));
        await tester.pump();
        expect(tester.getCenter(pill).dx, greaterThan(start.dx));
        expect(tester.getCenter(pill).dx, lessThan(end.dx));
        await gesture.moveTo(end + const Offset(25, 0));
        await gesture.up();
        await tester.pumpAndSettle();
        expect(tester.getRect(pill), tester.getRect(films));
        expect(
          find.descendant(
            of: find.byKey(const Key('hero-surface')),
            matching: find.textContaining(RegExp(r'Tommy\s+Riley')),
          ),
          findsOneWidget,
        );
        gesture = await tester.startGesture(end, kind: kind);
        await gesture.moveTo(start - const Offset(25, 0));
        await gesture.up();
        await tester.pumpAndSettle();
        expect(tester.getRect(pill), tester.getRect(games));
        gesture = await tester.startGesture(start, kind: kind);
        await gesture.moveTo(end + const Offset(30, 0));
        await gesture.cancel();
        await tester.pumpAndSettle();
        expect(tester.getRect(pill), tester.getRect(games));
        await tester.tap(films);
        await tester.pumpAndSettle();
        expect(tester.getRect(pill), tester.getRect(films));
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.linux),
    );
  }
}
