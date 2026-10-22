import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/features/activity/activity_repository.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/store/catalog_repository.dart';
import 'package:ryhze/features/workspace/workspace_panel.dart';

import '../test_driver/journey_deadline.dart';
import '../test_driver/journey_focus.dart';
import 'windows_process_probe.dart';

// An isolated synthetic profile exercises the real Windows runner, engine,
// native channels and journal. It never opens the person's application profile.
// Bundled metadata makes repeated profile runs independent of network latency.
void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('Windows navigation, retained Activity and durable restart', (
    tester,
  ) async {
    expect(Platform.isWindows, isTrue);
    final report = <String, Object?>{
      'source': const String.fromEnvironment(
        'RYHZE_SOURCE',
        defaultValue: 'unrecorded',
      ),
      'fixture': 'bundled-catalogue-51-synthetic-activity-isolated-journal',
      'acceptance_revision': 2,
      'status': 'running',
      'cleanup_completed': false,
      'physical_power_screen_reader_installer': 'not exercised by this test',
    };
    binding.reportData ??= <String, dynamic>{};
    binding.reportData!['journey'] = report;
    final owners = <AppServices>[];
    // 100 s body + 15 s widget disposal + two 15 s owners < 150 s backstop.
    final deadline = JourneyBodyDeadline(budget: const Duration(seconds: 100));
    var bodyCompleted = false;
    var cleanupStarted = false;
    var pendingBootstraps = 0;

    Future<T> bounded<T>(
      Future<T> Function() operation,
      String phase, {
      Duration timeout = const Duration(seconds: 15),
    }) async {
      report['pending_operation'] = phase;
      try {
        final value = cleanupStarted
            ? await Future<T>.sync(operation).timeout(timeout)
            : await deadline.run(operation, timeout: timeout);
        report.remove('pending_operation');
        return value;
      } catch (_) {
        report.putIfAbsent('failed_operation', () => phase);
        rethrow;
      }
    }

    Future<AppServices> bootstrap(Directory directory) async {
      Future<AppServices> create() async {
        pendingBootstraps++;
        try {
          final owner = await AppServices.bootstrap(
            openStore: () => JournalStore.open(directory),
            catalogRepository: LocalCatalogRepository(),
          );
          if (cleanupStarted) {
            await owner.close().timeout(const Duration(seconds: 15));
            throw StateError('Bootstrap completed after cleanup started.');
          }
          owners.add(owner);
          return owner;
        } finally {
          pendingBootstraps--;
        }
      }

      return bounded(
        create,
        'bootstrap services',
        timeout: const Duration(seconds: 35),
      );
    }

    Future<void> settle(String phase) async {
      if (cleanupStarted) throw StateError('Journey cleanup already started.');
      await bounded(
        () => tester.pumpAndSettle(
          const Duration(milliseconds: 100),
          EnginePhase.sendSemanticsUpdate,
          const Duration(seconds: 10),
        ),
        phase,
      );
    }

    Future<void> tap(Finder target, String phase, {bool reveal = false}) async {
      if (cleanupStarted) throw StateError('Journey cleanup already started.');
      if (reveal) {
        await bounded(() => tester.ensureVisible(target), 'reveal $phase');
      }
      if (cleanupStarted) throw StateError('Journey cleanup already started.');
      await bounded(() => tester.tap(target), 'tap $phase');
      await settle(phase);
    }

    Future<void> escape(String phase) async {
      if (cleanupStarted) throw StateError('Journey cleanup already started.');
      await bounded(
        () => tester.sendKeyEvent(
          LogicalKeyboardKey.escape,
          physicalKey: PhysicalKeyboardKey.escape,
        ),
        'send Escape: $phase',
      );
      await settle(phase);
    }

    try {
      final directory = await bounded(
        () => Directory.systemTemp.createTemp('ryhze-journey-'),
        'create isolated journal',
      );
      final startup = Stopwatch()..start();
      var services = await bootstrap(directory);
      expect(services.storageFailure, isNull);
      expect(services.storage.durable, isTrue);
      await bounded(
        () => tester.pumpWidget(RyhzeApp(services: services)),
        'mount application',
      );
      await settle('initial settled frame');
      startup.stop();
      final rssBefore = ProcessInfo.currentRss;
      final countersBefore = processCounters();
      final latencies = <int>[];
      await bounded(
        () => binding.watchPerformance(() async {
          for (var i = 0; i < 12; i++) {
            final navigation = Stopwatch()..start();
            await tap(find.byKey(const Key('hero-explore')), 'open detail');
            expect(find.byKey(const Key('detail-back')), findsOneWidget);
            await escape('close detail');
            expect(find.byKey(const Key('detail-back')), findsNothing);
            navigation.stop();
            latencies.add(navigation.elapsedMicroseconds);
          }
        }, reportKey: 'repeated_navigation'),
        'profile repeated navigation',
        timeout: const Duration(seconds: 60),
      );
      await tap(find.text('Favourites').first, 'save favourite');
      expect(services.store.favourite(Catalog.larcenous.id).value, isTrue);
      await tap(find.byKey(const Key('search-trigger')), 'open search');
      await bounded(
        () => tester.enterText(
          find.byKey(const Key('search-input')),
          'larcenous',
        ),
        'enter search query',
      );
      await settle('search results');
      expect(find.text('Larcenous Driftscape').last, findsOneWidget);
      await escape('close search');
      expect(FocusManager.instance.primaryFocus?.hasFocus, isTrue);

      // Test records are explicitly synthetic; they are not provider outcomes.
      await bounded(
        () => services.storage.transact((draft) {
          for (var i = 0; i < 51; i++) {
            ActivityRepository.recordIn(
              draft,
              services.session.scope,
              'synthetic-journey-$i',
              ActivityKind.failed,
              DateTime.utc(2026, 10, 10, 0, i),
            );
          }
        }),
        'record synthetic Activity',
      );
      services.activity.changed();
      await tap(find.byKey(const Key('menu-trigger')), 'open menu');
      await tap(find.byKey(const Key('menu-workspace')), 'open Workspace');
      report['workspace_open_focus'] = _workspaceFocus(services, tester);
      await tap(find.text('Activity'), 'open Activity');
      await tap(find.text('Next'), 'next Activity page', reveal: true);
      expect(find.text('Page 2 of 2'), findsOneWidget);
      await tap(find.text('Acknowledge'), 'acknowledge Activity', reveal: true);
      expect(
        services.activity
            .read(services.session.scope)
            .where((record) => record.acknowledged),
        hasLength(1),
      );
      final dismissal = <String, Object?>{
        'before': _workspaceFocus(services, tester),
        'escape_dispatched': false,
      };
      report['workspace_dismissal'] = dismissal;
      try {
        expect((dismissal['before'] as Map)['focused_route'], 'workspace');
        await bounded(
          () => tester.sendKeyEvent(
            LogicalKeyboardKey.escape,
            physicalKey: PhysicalKeyboardKey.escape,
          ),
          'send Workspace Escape',
        );
        dismissal['escape_dispatched'] = true;
        await settle('Workspace Escape disposal');
      } finally {
        dismissal['after'] = _workspaceFocus(services, tester);
      }
      expect(find.byType(WorkspacePanel), findsNothing, reason: '$dismissal');
      expect(services.overlays.isOpen, isFalse, reason: '$dismissal');
      expect(
        (dismissal['after'] as Map)['menu_focus_restored'],
        isTrue,
        reason: '$dismissal',
      );

      // This is software lifecycle coverage, not a physical power transition.
      await bounded(() async {
        services.interruptLifecycle();
        await services.recoverAfterResume();
      }, 'software resume recovery');
      expect(services.lifecycleFailure, isNull);
      await bounded(
        () => tester.pumpWidget(const SizedBox()),
        'dispose before restart',
      );
      await bounded(services.close, 'close before restart');
      services = await bootstrap(directory);
      await bounded(
        () => tester.pumpWidget(RyhzeApp(services: services)),
        'mount restarted application',
      );
      await settle('restarted settled frame');
      expect(services.store.favourite(Catalog.larcenous.id).value, isTrue);
      final retained = services.activity.read(services.session.scope);
      expect(retained, hasLength(51));
      expect(retained.where((record) => record.acknowledged), hasLength(1));
      expect(tester.takeException(), isNull);
      report.addAll({
        'bootstrap_to_settled_frame_us': startup.elapsedMicroseconds,
        'navigation_round_trip_us': latencies,
        'rss_before_bytes': rssBefore,
        'process_counters_before': countersBefore,
        'process_counters_after': processCounters(),
        'rss_after_bytes': ProcessInfo.currentRss,
        'max_rss_bytes': ProcessInfo.maxRss,
        'profile_directory': directory.path,
      });
      bodyCompleted = true;
    } finally {
      // A failed assertion anywhere in the workload still attempts all service
      // owners. Framework success alone never establishes a settled workload.
      report['status'] = 'failed';
      cleanupStarted = true;
      deadline.stop();
      var widgetDisposed = false;
      try {
        await bounded(
          () => tester.pumpWidget(const SizedBox()),
          'dispose final application',
        );
        widgetDisposed = true;
      } finally {
        var ownersClosed = pendingBootstraps == 0;
        for (final owner in List<AppServices>.of(owners)) {
          try {
            await bounded(owner.close, 'close final services');
          } catch (_) {
            ownersClosed = false;
          }
        }
        report['cleanup_completed'] = widgetDisposed && ownersClosed;
        if (bodyCompleted && widgetDisposed && ownersClosed) {
          report['status'] = 'completed';
          report.remove('pending_operation');
        }
      }
    }
  }, timeout: const Timeout(Duration(minutes: 2, seconds: 30)));
}

// Closed labels only: no focus debug strings, native records or user paths.
Map<String, Object?> _workspaceFocus(
  AppServices services,
  WidgetTester tester,
) {
  final finder = find.byType(WorkspacePanel);
  final workspace = finder.evaluate().isEmpty
      ? null
      : ModalRoute.of(tester.element(finder));
  final primary = FocusManager.instance.primaryFocus;
  final focused = primary?.context == null
      ? null
      : ModalRoute.of(primary!.context!);
  return {
    'workspace_present': finder.evaluate().isNotEmpty,
    'workspace_current': workspace?.isCurrent ?? false,
    'overlay_open': services.overlays.isOpen,
    'focused_route': primary == null
        ? 'none'
        : workspace != null && identical(workspace, focused)
        ? 'workspace'
        : 'other',
    'menu_focus_restored': menuTriggerHasPrimaryFocus(),
    'lifecycle': WidgetsBinding.instance.lifecycleState?.name ?? 'unknown',
  };
}
