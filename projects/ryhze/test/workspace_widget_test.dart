import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/features/workspace/workspace_panel.dart';

void main() {
  testWidgets(
    'saved manual titles stay outside Library and vanish on account switch',
    (tester) async {
      final services = AppServices.memory();
      await tester.pumpWidget(RyhzeApp(services: services));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('menu-trigger')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('menu-workspace')));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), 'A manually saved title');
      await tester.tap(find.text('Save title'));
      await tester.pumpAndSettle();
      expect(find.text('A manually saved title'), findsOneWidget);
      expect(services.library.entries, isEmpty);
      services.session.replace(const AccountScope.verified('test', 'other'));
      await tester.pumpAndSettle();
      expect(find.byType(WorkspacePanel), findsNothing);
      expect(services.collections.read(services.session.scope), isEmpty);
      expect(services.navigation.query, isEmpty);
      await tester.pumpWidget(const SizedBox());
      await services.close();
    },
  );
  testWidgets(
    'large text workspace keyboard dismissal and settings remain usable',
    (tester) async {
      tester.view.physicalSize = const Size(320, 640);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final services = AppServices.memory();
      await tester.pumpWidget(
        MediaQuery(
          data: const MediaQueryData(
            size: Size(320, 640),
            textScaler: TextScaler.linear(2),
          ),
          child: RyhzeApp(services: services),
        ),
      );
      await tester.pumpAndSettle();
      await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
      await tester.sendKeyEvent(LogicalKeyboardKey.comma);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
      await tester.pumpAndSettle();
      expect(find.byType(WorkspacePanel), findsOneWidget);
      await tester.ensureVisible(find.text('Settings'));
      await tester.tap(find.text('Settings'));
      await tester.pumpAndSettle();
      await tester.ensureVisible(find.text('Reduce motion'));
      await tester.tap(find.text('Reduce motion'));
      await tester.pumpAndSettle();
      expect(services.preferences.read().reduceMotion, isTrue);
      expect(tester.takeException(), isNull);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(find.byType(WorkspacePanel), findsNothing);
      await tester.pumpWidget(const SizedBox());
      await services.close();
    },
  );
  testWidgets(
    'keyboard modal command dismisses an open menu and deduplicates',
    (tester) async {
      await tester.pumpWidget(const RyhzeApp());
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('menu-trigger')));
      await tester.pumpAndSettle();
      expect(find.byType(MenuItemButton), findsNWidgets(4));
      for (var i = 0; i < 2; i++) {
        await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
        await tester.sendKeyEvent(LogicalKeyboardKey.keyK);
        await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
      }
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('search-input')), findsOneWidget);
      expect(
        tester
            .widget<EditableText>(find.byType(EditableText))
            .focusNode
            .hasFocus,
        isTrue,
      );
      expect(find.byType(MenuItemButton), findsNothing);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'Search trigger');
      expect(tester.takeException(), isNull);
    },
  );
}
