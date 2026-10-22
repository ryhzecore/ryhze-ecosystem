import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/design/controls.dart';

import '../test_driver/journey_focus.dart';

void main() {
  testWidgets('menu focus evidence uses identity without debug labels', (
    tester,
  ) async {
    final trigger = FocusNode();
    final decoy = FocusNode(debugLabel: 'Menu trigger');
    addTearDown(trigger.dispose);
    addTearDown(decoy.dispose);
    await tester.pumpWidget(
      MaterialApp(
        home: Column(
          children: [
            RyhzeControl(
              key: const Key('menu-trigger'),
              focusNode: trigger,
              label: 'Menu',
              onPressed: () {},
              child: const Text('Menu'),
            ),
            Focus(focusNode: decoy, child: const Text('Different control')),
          ],
        ),
      ),
    );

    decoy.requestFocus();
    await tester.pump();
    expect(FocusManager.instance.primaryFocus, same(decoy));
    expect(menuTriggerHasPrimaryFocus(), isFalse);

    trigger.requestFocus();
    await tester.pump();
    expect(trigger.debugLabel, isNull);
    expect(menuTriggerHasPrimaryFocus(), isTrue);

    await tester.pumpWidget(const SizedBox());
    expect(menuTriggerHasPrimaryFocus(), isFalse);
  });
}
