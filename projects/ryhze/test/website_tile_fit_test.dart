import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/store_sections.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final family in ['Inter', 'SpaceGrotesk']) {
      final loader = FontLoader(family)
        ..addFont(rootBundle.load('assets/fonts/$family.ttf'));
      await loader.load();
    }
  });

  const longTitle = EntertainmentTitle(
    id: 'long-title',
    title:
        'An unusually long game title with a second chapter and an extended '
        'edition for the journey across San Coronado',
    description: 'Layout fixture',
    tags: [],
  );
  for (final width in [761.0, 800.0, 850.0]) {
    for (final scale in [1.0, 1.6, 2.0]) {
      for (final title in [Catalog.larcenous, longTitle]) {
        testWidgets('website tile readable $width text=$scale ${title.id}', (
          tester,
        ) async {
          final size = Size(width, 1400);
          tester.view.devicePixelRatio = 1;
          tester.view.physicalSize = size;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          var opens = 0;
          await tester.pumpWidget(
            MaterialApp(
              theme: RyhzeTokens.theme,
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context).copyWith(
                  textScaler: TextScaler.linear(scale),
                  disableAnimations: true,
                ),
                child: child!,
              ),
              home: Scaffold(
                body: SingleChildScrollView(
                  child: Align(
                    alignment: Alignment.topLeft,
                    child: Padding(
                      padding: EdgeInsets.all(StoreLayout.gutter(width)),
                      child: SizedBox(
                        width: StoreLayout.cardWidth(width),
                        child: GameTile(title: title, onOpen: () => opens++),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          final titleFinder = find.byKey(
            ValueKey('game-tile-title-${title.id}'),
          );
          final titleRect = tester.getRect(titleFinder);
          final artRect = tester.getRect(
            find.byKey(const Key('tile-art-16-9')),
          );
          final brandRect = tester.getRect(
            find.byKey(const Key('thumbnail-brand')),
          );
          final tileRect = tester.getRect(
            find.byKey(const Key('catalog-tile')),
          );
          expect(artRect.size.aspectRatio, closeTo(16 / 9, .001));
          expect(titleRect.overlaps(brandRect), isFalse);
          expect(titleRect.top, greaterThanOrEqualTo(brandRect.bottom + 15.9));
          expect(titleRect.left, greaterThanOrEqualTo(tileRect.left));
          expect(titleRect.right, lessThanOrEqualTo(tileRect.right + .01));
          expect(titleRect.bottom, lessThanOrEqualTo(tileRect.bottom + .01));
          if (titleRect.top >= artRect.bottom) {
            expect(titleRect.top, greaterThanOrEqualTo(artRect.bottom + 11.9));
          } else {
            expect(titleRect.bottom, lessThanOrEqualTo(artRect.bottom - 23.9));
          }
          if (scale == 1 && title == Catalog.larcenous) {
            expect(titleRect.bottom, lessThan(artRect.bottom));
          }
          if (title == longTitle) {
            expect(titleRect.top, greaterThan(artRect.bottom));
          }
          expect(tester.takeException(), isNull);

          Focus.of(tester.element(titleFinder)).requestFocus();
          await tester.pump();
          await tester.sendKeyEvent(LogicalKeyboardKey.enter);
          expect(opens, 1);
          await tester.ensureVisible(titleFinder);
          await tester.pumpAndSettle();
          await tester.tap(titleFinder);
          expect(opens, 2);
          expect(tester.takeException(), isNull);
        });
      }
    }
  }
}
