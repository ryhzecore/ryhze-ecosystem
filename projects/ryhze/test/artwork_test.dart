import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/design/header_menu.dart';
import 'package:ryhze/features/entertainment/widgets/artwork.dart';

void main() {
  testWidgets('two menu lines morph smoothly and respect reduced motion', (
    tester,
  ) async {
    const key = Key('icon-pixels');
    Future<void> show(bool open, bool reduced) => tester.pumpWidget(
      MaterialApp(
        home: MediaQuery(
          data: MediaQueryData(disableAnimations: reduced),
          child: Center(
            child: RepaintBoundary(
              key: key,
              child: MenuToggleIcon(isOpen: open),
            ),
          ),
        ),
      ),
    );
    Future<List<int>> pixels() async {
      final boundary = tester.renderObject<RenderRepaintBoundary>(
        find.byKey(key),
      );
      return (await tester.runAsync(() async {
        final image = await boundary.toImage();
        final bytes = await image.toByteData(
          format: ui.ImageByteFormat.rawRgba,
        );
        image.dispose();
        return bytes!.buffer.asUint8List().toList();
      }))!;
    }

    await show(false, false);
    await tester.pumpAndSettle();
    final closed = await pixels();
    await show(true, false);
    await tester.pump(const Duration(milliseconds: 70));
    final moving = await pixels();
    await tester.pumpAndSettle();
    final opened = await pixels();
    expect(moving, isNot(closed));
    expect(moving, isNot(opened));
    expect(opened, isNot(closed));
    await show(false, true);
    await tester.pump();
    expect(await pixels(), closed);
    await show(true, true);
    await tester.pump();
    expect(await pixels(), opened);
  });

  testWidgets(
    'page and frame stay flat black while preserving artwork and corners',
    (tester) async {
      const key = Key('flat-artwork-pixels');
      await tester.pumpWidget(
        const MaterialApp(
          home: Scaffold(
            body: Center(
              child: RepaintBoundary(
                key: key,
                child: SizedBox(
                  width: 360,
                  height: 280,
                  child: ColoredBox(
                    color: Colors.black,
                    child: RyhzeAmbience(
                      child: Align(
                        alignment: Alignment.topCenter,
                        child: Padding(
                          padding: EdgeInsets.only(top: 20),
                          child: ArtworkFrame(
                            child: SizedBox(
                              width: 320,
                              height: 180,
                              child: ColoredBox(color: Color(0xFF303238)),
                            ),
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final boundary = tester.renderObject<RenderRepaintBoundary>(
        find.byKey(key),
      );
      final pixels = await tester.runAsync(() async {
        final image = await boundary.toImage(pixelRatio: 1);
        final bytes = await image.toByteData(
          format: ui.ImageByteFormat.rawRgba,
        );
        image.dispose();
        return bytes!;
      });
      List<int> rgb(int x, int y) =>
          List.generate(3, (c) => pixels!.getUint8((y * 360 + x) * 4 + c));
      expect(rgb(180, 100), [48, 50, 56]);
      expect(rgb(180, 195), [48, 50, 56]);
      // The page corners and space beneath the frame must not acquire a glow.
      for (final point in <(int, int)>[
        (5, 5),
        (180, 5),
        (355, 275),
        (180, 212),
        (180, 250),
        // Inside the child's square bounds but outside its rounded corner.
        (22, 22),
      ]) {
        expect(rgb(point.$1, point.$2), [0, 0, 0], reason: '$point');
      }
    },
  );

  for (final reduced in [false, true]) {
    testWidgets(
      'catalog strips removed; art hover stays centered, reduced=$reduced',
      (tester) async {
        const size = Size(1280, 800);
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = size;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await tester.pumpWidget(
          MediaQuery(
            data: MediaQueryData(size: size, disableAnimations: reduced),
            child: const RyhzeApp(),
          ),
        );
        await tester.pumpAndSettle();
        // Position the catalogue directly: the hero has its own accessible scroll area.
        tester
            .widget<CustomScrollView>(find.byKey(const Key('catalog-scroll')))
            .controller!
            .jumpTo(500);
        await tester.pumpAndSettle();
        expect(find.text('To Be Continued'), findsNothing);
        expect(find.text('Open world · Driving'), findsNothing);
        expect(
          find.descendant(
            of: find.byKey(const Key('catalog-tile')),
            matching: find.byIcon(Icons.add_rounded),
          ),
          findsNothing,
        );
        final tile = find.byKey(const Key('tile-art-16-9'));
        final before = tester.getRect(tile);
        expect(before.width / before.height, closeTo(16 / 9, .001));
        final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
        await mouse.addPointer(location: Offset.zero);
        await mouse.moveTo(before.center);
        for (var i = 0; i < 13; i++) {
          await tester.pump(const Duration(milliseconds: 50));
          final frame = tester.getRect(tile);
          expect(frame.center.dx, closeTo(before.center.dx, .01));
          expect(frame.center.dy, closeTo(before.center.dy, .01));
          expect(frame.width / frame.height, closeTo(16 / 9, .001));
          if (reduced) expect(frame, before);
        }
        await mouse.removePointer();
        await tester.tap(tile);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('detail-back')), findsOneWidget);
        await tester.tap(find.byKey(const Key('detail-back')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('category-films')));
        await tester.pumpAndSettle();
        expect(find.text('To Be Continued'), findsNothing);
        expect(find.text('Open world · Driving'), findsNothing);
        expect(find.byKey(const Key('catalog-tile')), findsNothing);
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.linux),
    );
  }
}
