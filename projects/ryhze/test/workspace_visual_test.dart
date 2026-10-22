import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/features/workspace/workspace_panel.dart';

import 'support/golden_diagnostics.dart';

class GuardedWorkspaceComparator extends LocalFileComparator {
  GuardedWorkspaceComparator(super.testFile);
  @override
  Future<void> update(Uri golden, Uint8List bytes) async {
    if (golden.path.contains('v060-continuation/')) {
      await super.update(golden, bytes);
    } else if (!await compare(bytes, golden)) {
      throw StateError('Historical workspace reference changed: $golden');
    }
  }
}

void main() {
  final originalComparator = goldenFileComparator;
  setUpAll(() {
    goldenFileComparator = GuardedWorkspaceComparator(
      Uri.file('${Directory.current.path}/test/workspace_visual_test.dart'),
    );
  });
  tearDownAll(() {
    goldenFileComparator = originalComparator;
  });
  installHostGoldens();
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final font in ['Inter', 'SpaceGrotesk']) {
      await (FontLoader(
        font,
      )..addFont(rootBundle.load('assets/fonts/$font.ttf'))).load();
    }
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  for (final layout in [
    ('desktop', const Size(1280, 900), 1.0),
    ('narrow-large', const Size(320, 800), 2.0),
  ]) {
    testWidgets('${layout.$1} workspace evidence', (tester) async {
      tester.view.physicalSize = layout.$2;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final services = AppServices.memory();
      await services.workspace.addManual(
        'A manually saved title — no ownership verified',
      );
      await tester.pumpWidget(
        MediaQuery(
          data: MediaQueryData(
            size: layout.$2,
            textScaler: TextScaler.linear(layout.$3),
            disableAnimations: true,
          ),
          child: RyhzeApp(services: services),
        ),
      );
      await tester.pumpAndSettle();
      await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
      await tester.sendKeyEvent(LogicalKeyboardKey.comma);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
      await tester.pumpAndSettle();
      for (final section in [
        'Saved',
        'Connections',
        'Activity',
        'Settings',
        'Support',
      ]) {
        final button = find
            .descendant(
              of: find.byType(WorkspacePanel),
              matching: find.text(section),
            )
            .first;
        await tester.ensureVisible(button);
        await tester.tap(button);
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
        await expectGoldenWithDiagnostics(
          tester,
          find.byType(WorkspacePanel),
          'goldens/monochrome-20261011/${section == 'Support' ? 'v060-continuation' : 'v060-workspace'}/${layout.$1}-${section.toLowerCase()}.png',
        );
      }
      await tester.pumpWidget(const SizedBox());
      await services.close();
    }, variant: TargetPlatformVariant.only(TargetPlatform.linux));
  }
}
