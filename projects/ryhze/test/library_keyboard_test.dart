import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/features/library/library_screen.dart';

void main() {
  for (final width in [740.0, 1280.0]) {
    for (final demo in [false, true]) {
      for (final scale in [1.0, 1.6, 2.0]) {
        testWidgets(
          'Library search remains usable with landscape keyboard at $width, demo=$demo, text=$scale',
          (tester) async {
            final previousHitTestWarnings =
                WidgetController.hitTestWarningShouldBeFatal;
            WidgetController.hitTestWarningShouldBeFatal = true;
            addTearDown(
              () => WidgetController.hitTestWarningShouldBeFatal =
                  previousHitTestWarnings,
            );
            final size = Size(width, 320);
            tester.view.devicePixelRatio = 1;
            tester.view.physicalSize = size;
            addTearDown(tester.view.resetPhysicalSize);
            addTearDown(tester.view.resetDevicePixelRatio);

            Future<void> show(double inset) async {
              await tester.pumpWidget(
                MediaQuery(
                  data: MediaQueryData(
                    size: size,
                    viewInsets: EdgeInsets.only(bottom: inset),
                    textScaler: TextScaler.linear(scale),
                    disableAnimations: true,
                  ),
                  child: const RyhzeApp(),
                ),
              );
              await tester.pumpAndSettle();
            }

            await show(0);
            final destination = find.byKey(const Key('destination-library'));
            await tester.ensureVisible(destination);
            await tester.pumpAndSettle();
            await tester.tap(destination);
            await tester.pumpAndSettle();
            if (demo) {
              final trigger = find.byKey(const Key('library-demo-toggle'));
              await tester.ensureVisible(trigger);
              await tester.pumpAndSettle();
              await tester.tap(trigger);
              await tester.pumpAndSettle();
            }
            final search = find.byKey(const Key('library-search'));
            await tester.ensureVisible(search);
            await tester.pumpAndSettle();
            await tester.tap(search);
            await tester.enterText(search, 'Aurora');
            await tester.pumpAndSettle();

            await show(180);
            final editable = find.descendant(
              of: search,
              matching: find.byType(EditableText),
            );
            await tester.ensureVisible(editable);
            await tester.pumpAndSettle();
            expect(tester.takeException(), isNull);
            expect(search.hitTestable(), findsOneWidget);
            expect(
              tester.widget<TextField>(search).focusNode!.hasFocus,
              isTrue,
            );
            final viewport = tester.getRect(
              find.byKey(const Key('library-list')),
            );
            expect(viewport.contains(tester.getCenter(search)), isTrue);
            expect(tester.getCenter(search).dy, lessThanOrEqualTo(140));
            final textRect = tester.getRect(editable);
            expect(viewport.contains(textRect.topLeft), isTrue);
            expect(
              viewport.contains(textRect.bottomRight - const Offset(.01, .01)),
              isTrue,
            );
            expect(textRect.bottom, lessThanOrEqualTo(140));
            if (demo) {
              final label = find.descendant(
                of: find.byKey(const Key('library-demo-indicator')),
                matching: find.byType(Text),
              );
              final labelRect = tester.getRect(label);
              final available = Rect.fromLTWH(0, 0, width, 140);
              expect(available.contains(labelRect.topLeft), isTrue);
              expect(
                available.contains(
                  labelRect.bottomRight - const Offset(.01, .01),
                ),
                isTrue,
              );
            }

            await tester.enterText(search, 'Moonlit');
            await tester.pumpAndSettle();
            final controller = tester
                .widget<LibraryScreen>(find.byType(LibraryScreen))
                .controller;
            expect(controller.query, 'Moonlit');
            expect(tester.takeException(), isNull);

            await show(0);
            expect(controller.query, 'Moonlit');
            expect(tester.takeException(), isNull);
          },
        );
      }
    }
  }
}
