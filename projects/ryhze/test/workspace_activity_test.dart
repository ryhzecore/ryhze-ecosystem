import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/domain/app_route.dart';
import 'package:ryhze/features/activity/activity_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/features/workspace/workspace_panel.dart';

Future<void> seed(AppServices services, int count) async {
  await services.storage.transact((draft) {
    for (var i = 0; i < count; i++) {
      ActivityRepository.recordIn(
        draft,
        services.session.scope,
        'event-$i',
        ActivityKind.completed,
        DateTime.utc(2026, 1, 1).add(Duration(seconds: i)),
        route: i == 0 ? const AppRoute(AppDestination.filmsStore) : null,
      );
    }
  });
  services.activity.changed();
}

Finder control(String label) => find.byWidgetPredicate(
  (widget) => widget is RyhzeControl && widget.label == label,
);
Future<void> press(WidgetTester tester, String label) async {
  final target = control(label);
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
  await tester.pumpAndSettle();
}

Future<void> panel(WidgetTester tester, AppServices services) async {
  await tester.pumpWidget(
    MaterialApp(
      home: WorkspacePanel(
        controller: services.workspace,
        providers: services.providers,
        jobs: const [],
        diagnostics: services.diagnostics,
        durable: false,
        integrations: services.integrations,
      ),
    ),
  );
  await press(tester, 'Activity');
}

void main() {
  testWidgets('all 200 events reachable; pages clamp and reset by account', (
    tester,
  ) async {
    final services = AppServices.memory();
    await seed(services, 200);
    final events = services.workspace.events;
    await panel(tester, services);
    for (var page = 0; page < 4; page++) {
      expect(find.text('Page ${page + 1} of 4'), findsOneWidget);
      expect(
        find.byKey(ValueKey('activity-${events[page * 50].id}')),
        findsOneWidget,
      );
      expect(
        find.byKey(ValueKey('activity-${events[page * 50 + 49].id}')),
        findsOneWidget,
      );
      expect(
        tester
                .widget<RyhzeControl>(control('Previous activity page'))
                .onPressed ==
            null,
        page == 0,
      );
      expect(
        tester.widget<RyhzeControl>(control('Next activity page')).onPressed ==
            null,
        page == 3,
      );
      if (page < 3) await press(tester, 'Next activity page');
    }
    await services.storage.transact((draft) {
      final account =
          (draft['accounts'] as Map)[services.session.scope.key] as Map;
      (account['activity'] as Map).removeWhere(
        (key, _) => key != events.first.id,
      );
    });
    services.activity.changed();
    await tester.pumpAndSettle();
    expect(find.byKey(ValueKey('activity-${events.first.id}')), findsOneWidget);
    expect(control('Next activity page'), findsNothing);
    await seed(services, 51);
    await tester.pumpAndSettle();
    await press(tester, 'Next activity page');
    expect(find.text('Page 2 of 2'), findsOneWidget);
    services.session.replace(const AccountScope.verified('test', 'next'));
    await seed(services, 51);
    await tester.pumpAndSettle();
    expect(find.text('Page 1 of 2'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox());
    await services.close();
  });

  testWidgets('read-only activity keeps acknowledgement disabled', (
    tester,
  ) async {
    final services = AppServices(
      storage: MemoryTransactionStore(),
      storageFailure: const AppFailure(FailureKind.storage),
    );
    await seed(services, 1);
    await panel(tester, services);
    expect(
      tester.widget<RyhzeControl>(control('Acknowledge activity')).onPressed,
      isNull,
    );
    expect(
      tester.widget<RyhzeControl>(control('Open activity item')).onPressed,
      isNotNull,
    );
    expect(services.workspace.events.single.acknowledged, isFalse);
    await tester.pumpWidget(const SizedBox());
    await services.close();
  });

  testWidgets('older event acknowledgement and navigation use existing flow', (
    tester,
  ) async {
    final services = AppServices.memory();
    await seed(services, 51);
    final oldest = services.workspace.events.last.id;
    await tester.pumpWidget(RyhzeApp(services: services));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('menu-trigger')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('menu-workspace')));
    await tester.pumpAndSettle();
    await press(tester, 'Activity');
    await press(tester, 'Next activity page');
    await press(tester, 'Acknowledge activity');
    expect(
      services.workspace.events.singleWhere((e) => e.id == oldest).acknowledged,
      isTrue,
    );
    expect(
      ActivityRepository(services.storage)
          .read(services.session.scope)
          .last
          .acknowledged,
      isTrue,
    );
    await press(tester, 'Open activity item');
    expect(find.byType(WorkspacePanel), findsNothing);
    expect(services.navigation.destination, AppDestination.filmsStore);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox());
    await services.close();
  });
}
