import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/design/tokens.dart';

void main() {
  testWidgets(
    'white primary control keeps readable content through interaction',
    (tester) async {
      final semantics = tester.ensureSemantics();
      try {
        final focus = FocusNode();
        addTearDown(focus.dispose);
        final previousStrategy = FocusManager.instance.highlightStrategy;
        FocusManager.instance.highlightStrategy =
            FocusHighlightStrategy.alwaysTraditional;
        addTearDown(
          () => FocusManager.instance.highlightStrategy = previousStrategy,
        );
        var enabled = true;
        var activations = 0;
        late StateSetter update;
        const controlKey = Key('primary-control');
        const labelKey = Key('primary-label');
        const iconKey = Key('primary-icon');
        await tester.pumpWidget(
          MaterialApp(
            theme: RyhzeTokens.theme,
            home: Scaffold(
              body: Center(
                child: StatefulBuilder(
                  builder: (context, setState) {
                    update = setState;
                    return RyhzeControl(
                      key: controlKey,
                      label: 'Explore the game',
                      primary: true,
                      focusNode: focus,
                      onPressed: enabled ? () => activations++ : null,
                      child: const Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          Text('Explore the game', key: labelKey),
                          SizedBox(width: 12),
                          Icon(Icons.arrow_forward, key: iconKey),
                        ],
                      ),
                    );
                  },
                ),
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final control = find.byKey(controlKey);
        BoxDecoration decoration() =>
            tester
                    .widget<AnimatedContainer>(
                      find.descendant(
                        of: control,
                        matching: find.byType(AnimatedContainer),
                      ),
                    )
                    .decoration!
                as BoxDecoration;
        Color labelColor() =>
            DefaultTextStyle.of(tester.element(find.byKey(labelKey)))
                .style
                .color!;
        void checkContent(Color background, Color foreground) {
          expect(decoration().color, background);
          expect(labelColor(), foreground);
          expect(
            IconTheme.of(tester.element(find.byKey(iconKey))).color,
            foreground,
          );
          final luminance = [
            background.computeLuminance(),
            foreground.computeLuminance(),
          ]..sort();
          expect(
            (luminance.last + .05) / (luminance.first + .05),
            greaterThanOrEqualTo(4.5),
          );
        }

        checkContent(Colors.white, const Color(0xFF090909));
        final originalBounds = tester.getRect(control);
        final mouse = await tester.createGesture(
          kind: ui.PointerDeviceKind.mouse,
        );
        await mouse.addPointer(location: Offset.zero);
        addTearDown(mouse.removePointer);
        await mouse.moveTo(originalBounds.center);
        await tester.pumpAndSettle();
        checkContent(const Color(0xFFE6E6E6), const Color(0xFF090909));
        focus.requestFocus();
        await tester.pumpAndSettle();
        expect(focus.hasFocus, isTrue);
        expect((decoration().border! as Border).top.color, Colors.white);
        expect(tester.getRect(control), originalBounds);
        await tester.sendKeyEvent(LogicalKeyboardKey.enter);
        await tester.pumpAndSettle();
        expect(activations, 1);
        await tester.tap(control);
        await tester.pumpAndSettle();
        expect(activations, 2);
        update(() => enabled = false);
        await tester.pumpAndSettle();
        expect(decoration().color, isNot(Colors.white));
        checkContent(decoration().color!, labelColor());
        final disabled = tester.getSemantics(control).getSemanticsData();
        expect(disabled.hasAction(ui.SemanticsAction.tap), isFalse);
        await tester.tap(control);
        await tester.sendKeyEvent(LogicalKeyboardKey.enter);
        await tester.pumpAndSettle();
        expect(activations, 2);
        expect(tester.getRect(control), originalBounds);
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    },
  );

  testWidgets(
    'neighboring controls retain independent semantic actions through tooltip updates',
    (tester) async {
      final semantics = tester.ensureSemantics();
      try {
        var searchCount = 0;
        var menuCount = 0;
        var menuOpen = false;
        var searchEnabled = true;
        late StateSetter update;
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: StatefulBuilder(
                builder: (context, setState) {
                  update = setState;
                  return Row(
                    children: [
                      RyhzeControl(
                        key: const Key('search'),
                        label: 'Search titles',
                        iconOnly: true,
                        onPressed: searchEnabled ? () => searchCount++ : null,
                        child: const Icon(Icons.search),
                      ),
                      RyhzeControl(
                        key: const Key('menu'),
                        label: menuOpen ? 'Close menu' : 'Open menu',
                        iconOnly: true,
                        onPressed: () {
                          menuCount++;
                          setState(() => menuOpen = !menuOpen);
                        },
                        child: Icon(menuOpen ? Icons.close : Icons.menu),
                      ),
                    ],
                  );
                },
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();

        void check(
          String key,
          String label,
          String otherLabel, {
          bool enabled = true,
        }) {
          final data = tester
              .getSemantics(find.byKey(Key(key)))
              .getSemanticsData();
          expect(data.label, contains(label));
          expect(data.label, isNot(contains(otherLabel)));
          expect(data.hasAction(ui.SemanticsAction.tap), enabled);
        }

        Future<void> activate(String key) async {
          final node = tester.getSemantics(find.byKey(Key(key)));
          tester.binding.performSemanticsAction(
            ui.SemanticsActionEvent(
              viewId: tester.view.viewId,
              nodeId: node.id,
              type: ui.SemanticsAction.tap,
            ),
          );
          await tester.pumpAndSettle();
        }

        Future<void> showTooltip(String key) async {
          final tooltip = find.descendant(
            of: find.byKey(Key(key)),
            matching: find.byType(Tooltip),
          );
          tester.state<TooltipState>(tooltip).ensureTooltipVisible();
          await tester.pumpAndSettle();
        }

        check('search', 'Search titles', 'Open menu');
        check('menu', 'Open menu', 'Search titles');
        expect(
          tester.getSemantics(find.byKey(const Key('search'))).id,
          isNot(tester.getSemantics(find.byKey(const Key('menu'))).id),
        );
        await showTooltip('search');
        expect(find.text('Search titles'), findsOneWidget);
        check('search', 'Search titles', 'Open menu');
        check('menu', 'Open menu', 'Search titles');
        await activate('search');
        expect(searchCount, 1);
        expect(menuCount, 0);
        Tooltip.dismissAllToolTips();
        await tester.pumpAndSettle();
        expect(find.text('Search titles'), findsNothing);

        await showTooltip('menu');
        expect(find.text('Open menu'), findsOneWidget);
        await activate('menu');
        expect(menuCount, 1);
        check('menu', 'Close menu', 'Search titles');
        check('search', 'Search titles', 'Close menu');
        Tooltip.dismissAllToolTips();
        await tester.pumpAndSettle();
        await showTooltip('menu');
        expect(find.text('Close menu'), findsOneWidget);
        update(() => searchEnabled = false);
        await tester.pumpAndSettle();
        check('search', 'Search titles', 'Close menu', enabled: false);
        check('menu', 'Close menu', 'Search titles');
        Tooltip.dismissAllToolTips();
        await tester.pumpAndSettle();
        await activate('menu');
        expect(menuCount, 2);
        expect(searchCount, 1);
        check('menu', 'Open menu', 'Search titles');
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    },
  );
}
