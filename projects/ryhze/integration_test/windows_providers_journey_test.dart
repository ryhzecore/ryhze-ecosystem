import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/store/artwork_maintenance.dart';
import 'package:ryhze/features/store/catalog_maintenance.dart';
import 'package:ryhze/features/workspace/workspace_panel.dart';
import 'package:ryhze/providers/ryhze_catalog_repository.dart';

import '../test_driver/journey_deadline.dart';
import '../test_driver/journey_focus.dart';

// Live public metadata and read-only native inventory, with an isolated journal.
// No injected successful provider response, account connection, notification
// enablement, registry write, installation, launch or private-path reporting.
void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('Windows live provider, cache maintenance and queued activation', (
    tester,
  ) async {
    expect(Platform.isWindows, isTrue);
    final report = <String, Object?>{
      'source': const String.fromEnvironment(
        'RYHZE_SOURCE',
        defaultValue: 'unrecorded',
      ),
      'workload':
          'production-anonymous-catalogue-readonly-steam-isolated-journal',
      'acceptance_revision': 2,
      'status': 'running',
      'cleanup_completed': false,
      'completed_phases': <String>[],
      'limits': [
        'No ownership, sign-in, download, executable launch or provider credentials.',
        'Cancellation requests are exercised; an actual socket abort is not instrumented.',
        'Steam inventory is compatibility observation, not an exhaustive supported provider API.',
        'Direct same-executable IPC does not test installed protocol registration or toast delivery.',
        'No physical power, screen reader, installer or image-golden acceptance.',
      ],
    };
    binding.reportData ??= <String, dynamic>{};
    binding.reportData!['providers_journey'] = report;
    // 115 s body + 15 s widget disposal + 15 s owner close < 150 s backstop.
    final deadline = JourneyBodyDeadline(budget: const Duration(seconds: 115));
    AppServices? services;
    var bootstrapPending = false;
    var cleanupStarted = false;
    var bodyCompleted = false;
    Future<T> bounded<T>(
      Future<T> Function() operation,
      String label, {
      Duration timeout = const Duration(seconds: 15),
    }) async {
      report['pending_operation'] = label;
      try {
        final result = cleanupStarted
            ? await Future<T>.sync(operation).timeout(timeout)
            : await deadline.run(operation, timeout: timeout);
        report.remove('pending_operation');
        return result;
      } catch (_) {
        report.putIfAbsent('failed_operation', () => label);
        rethrow;
      }
    }

    Future<void> pump([Duration? duration]) async {
      if (cleanupStarted) throw StateError('Journey cleanup already started.');
      await bounded(() => tester.pump(duration), 'render frame');
    }

    Future<void> until(
      bool Function() ready,
      String phase, {
      Duration timeout = const Duration(seconds: 35),
    }) async {
      final elapsed = Stopwatch()..start();
      while (!ready() && elapsed.elapsed < timeout) {
        await pump(const Duration(milliseconds: 100));
      }
      expect(ready(), isTrue, reason: 'Bounded phase did not finish: $phase');
      await pump();
    }

    Future<void> tap(Finder target) async {
      if (cleanupStarted) throw StateError('Journey cleanup already started.');
      await bounded(() => tester.ensureVisible(target), 'reveal control');
      if (cleanupStarted) throw StateError('Journey cleanup already started.');
      await bounded(() => tester.tap(target), 'tap control');
      await pump(const Duration(milliseconds: 250));
    }

    Future<void> escape() async {
      if (cleanupStarted) throw StateError('Journey cleanup already started.');
      await bounded(
        () => tester.sendKeyEvent(
          LogicalKeyboardKey.escape,
          physicalKey: PhysicalKeyboardKey.escape,
        ),
        'dismiss route',
      );
      await pump(const Duration(milliseconds: 350));
    }

    void done(String phase) {
      (report['completed_phases'] as List<String>).add(phase);
      report.remove('pending_operation');
      debugPrint('Provider journey completed phase: $phase');
    }

    try {
      final directory = await bounded(
        () => Directory.systemTemp.createTemp('ryhze-providers-journey-'),
        'create isolated journal',
      );
      Future<AppServices> bootstrap() async {
        bootstrapPending = true;
        try {
          final app = await AppServices.bootstrap(
            openStore: () => JournalStore.open(directory),
            // Notification enablement/history is outside this workload.
            connectNotifications: () async => null,
          );
          if (cleanupStarted) {
            await app.close().timeout(const Duration(seconds: 15));
            throw StateError('Bootstrap completed after cleanup started.');
          }
          services = app;
          return app;
        } finally {
          bootstrapPending = false;
        }
      }

      final app = await bounded(
        bootstrap,
        'bootstrap services',
        timeout: const Duration(seconds: 35),
      );
      expect(app.storageFailure, isNull);
      expect(app.storage.durable, isTrue);
      expect(app.catalog, isA<RyhzeCatalogRepository>());
      final maintenance = app.catalog as CatalogMaintenance;
      await bounded(
        () => tester.pumpWidget(RyhzeApp(services: app)),
        'mount application',
      );
      await until(
        () => !app.discovery.state(EntertainmentCategory.games).loading,
        'initial public catalogue',
      );
      void recordCatalogue(String label) {
        final state = app.discovery.state(EntertainmentCategory.games);
        final read = state.read;
        if (read == null) {
          expect(state.failure, isNotNull);
          report[label] = {
            'quality': 'unavailable',
            'failure': state.failure?.kind.name,
            'public_metadata_count': null,
            'cache_state': maintenance.cacheHealth().state.name,
          };
          return;
        }
        if (read.quality == ReadQuality.complete) {
          expect(read.failure, isNull);
          expect(
            read.values.every(
              (item) =>
                  item.availability ==
                  RyhzeCatalogRepository.metadataAvailability,
            ),
            isTrue,
          );
        } else {
          expect(read.failure ?? state.failure, isNotNull);
        }
        report[label] = {
          'quality': read.quality.name,
          'failure': (read.failure ?? state.failure)?.kind.name,
          // Bundled fallback counts must not be reported as live provider data.
          'public_metadata_count':
              read.quality == ReadQuality.complete ||
                  read.quality == ReadQuality.stale
              ? read.values
                    .where(
                      (item) =>
                          item.availability ==
                          RyhzeCatalogRepository.metadataAvailability,
                    )
                    .length
              : null,
          'observed_at_utc': read.observedAt.toUtc().toIso8601String(),
          'cache_state': maintenance.cacheHealth().state.name,
        };
      }

      recordCatalogue('initial_catalogue');
      done('initial truthful catalogue quality');

      await tap(find.byKey(const Key('destination-library')));
      expect(app.navigation.libraryDestination, isTrue);
      final libraryCount = app.library.entries.length;
      await tap(find.byKey(const Key('destination-store')));
      done('Library and Store navigation');

      await tap(find.byKey(const Key('menu-trigger')));
      await tap(find.byKey(const Key('menu-workspace')));
      report['workspace_open_focus'] = _workspaceFocus(app, tester);
      await tap(find.text('Settings').first);
      expect(find.text('Reduce motion'), findsOneWidget);
      final preferencesBefore = app.storage.read()['preferences'];
      await tap(find.text('Support').first);
      expect(find.text('Public catalogue cache'), findsOneWidget);
      expect(find.text('Public artwork cache'), findsOneWidget);
      final artwork = app.artwork;
      expect(
        artwork,
        isNotNull,
        reason: 'Real application artwork owner required.',
      );
      late PublicArtworkHealth beforeArtwork;
      final waiting = Stopwatch()..start();
      do {
        beforeArtwork = await bounded(
          artwork!.health,
          'artwork health before UI clear',
        );
        if (beforeArtwork.active == 0 && beforeArtwork.queued == 0) break;
        await pump(const Duration(milliseconds: 100));
      } while (waiting.elapsed < const Duration(seconds: 25));
      expect(beforeArtwork.active, 0);
      expect(beforeArtwork.queued, 0);
      expect(beforeArtwork.diskFailure, isNull);
      expect(
        beforeArtwork.disk,
        isNotNull,
        reason: 'Actual native artwork cache required.',
      );
      final stateBeforeArtwork = jsonEncode(app.storage.read());
      final libraryBeforeArtwork = List.of(app.library.entries);
      final titlesBeforeArtwork = List.of(
        app.discovery.state(EntertainmentCategory.games).titles,
      );
      final imageWidgetsBefore = find
          .byKey(const Key('public-artwork-image'))
          .evaluate()
          .length;
      await tap(find.text('Clear artwork cache').first);
      final artworkMessage =
          'Artwork cache cleared. ${beforeArtwork.disk!.entries} saved images removed; '
          '${beforeArtwork.disk!.preserved} unrecognized files preserved.';
      await until(
        () => find.text(artworkMessage).evaluate().isNotEmpty,
        'visible native artwork cache clear',
        timeout: const Duration(seconds: 25),
      );
      await pump(const Duration(milliseconds: 300));
      final afterArtwork = await bounded(
        artwork.health,
        'artwork health after UI clear',
      );
      expect(afterArtwork.diskFailure, isNull);
      expect(afterArtwork.disk, isNotNull);
      expect(afterArtwork.disk!.entries, 0);
      expect(afterArtwork.disk!.bytes, 0);
      expect(afterArtwork.disk!.preserved, beforeArtwork.disk!.preserved);
      expect(afterArtwork.memoryEntries, 0);
      expect(afterArtwork.memoryBytes, 0);
      expect(afterArtwork.active, 0);
      expect(afterArtwork.queued, 0);
      expect(jsonEncode(app.storage.read()), stateBeforeArtwork);
      expect(app.library.entries, libraryBeforeArtwork);
      expect(
        app.discovery.state(EntertainmentCategory.games).titles,
        titlesBeforeArtwork,
      );
      expect(find.byKey(const Key('public-artwork-image')), findsNothing);
      report['artwork_support_clear'] = {
        'native_cache_available': true,
        'before': _artworkHealth(beforeArtwork),
        'after': _artworkHealth(afterArtwork),
        'ui_confirmation_matched': true,
        'reported_removed': beforeArtwork.disk!.entries,
        'reported_preserved': beforeArtwork.disk!.preserved,
        'isolated_user_state_unchanged': true,
        'catalogue_unchanged': true,
        'library_unchanged': true,
        'bound_image_widgets_before': imageWidgetsBefore,
        'bound_image_widgets_after': 0,
      };
      report['workspace_after_artwork_clear_focus'] = _workspaceFocus(
        app,
        tester,
      );
      done('Support artwork clear preserves catalogue and user state');
      report['cache_before_clear'] = maintenance.cacheHealth().state.name;
      await tap(find.text('Clear catalogue cache').first);
      await until(
        () => find.text('Catalogue cache cleared.').evaluate().isNotEmpty,
        'visible cache clear',
        timeout: const Duration(seconds: 15),
      );
      expect(maintenance.cacheHealth().state, CatalogCacheState.empty);
      expect(app.storage.read()['preferences'], preferencesBefore);
      done('Settings and cache health/clear UI');

      report['steam_adapter_available'] = app.installations.available;
      if (app.installations.available) {
        await tap(find.text('Installations').first);
        await tap(find.text('Inspect Steam installations').first);
        await until(
          () => !app.installations.busy,
          'read-only Steam inspection',
          timeout: const Duration(seconds: 25),
        );
        final inventory = app.installations.read;
        report['steam_inspection'] = {
          'quality': inventory?.quality.name,
          'failure':
              (inventory?.failure ?? app.installations.failure)?.kind.name,
          'observation_count': inventory?.values.length,
          'paths_titles_and_ids': 'not included',
        };
        expect(inventory != null || app.installations.failure != null, isTrue);
        expect(app.library.entries.length, libraryCount);
        done('on-demand native Steam observation');
      } else {
        report['steam_inspection'] = {
          'state': 'adapter unavailable; not exercised',
        };
      }
      final dismissal = <String, Object?>{
        'before': _workspaceFocus(app, tester),
        'escape_dispatched': false,
      };
      report['workspace_dismissal'] = dismissal;
      try {
        expect(
          (dismissal['before'] as Map)['focused_route'],
          'workspace',
          reason: '$dismissal',
        );
        await bounded(
          () => tester.sendKeyEvent(
            LogicalKeyboardKey.escape,
            physicalKey: PhysicalKeyboardKey.escape,
          ),
          'send Workspace Escape',
        );
        dismissal['escape_dispatched'] = true;
        await until(
          () =>
              find.byType(WorkspacePanel).evaluate().isEmpty &&
              !app.overlays.isOpen &&
              menuTriggerHasPrimaryFocus(),
          'workspace route disposal and menu focus',
          timeout: const Duration(seconds: 15),
        );
      } finally {
        dismissal['after'] = _workspaceFocus(app, tester);
      }
      expect(find.byType(WorkspacePanel), findsNothing, reason: '$dismissal');
      expect(app.overlays.isOpen, isFalse, reason: '$dismissal');
      expect(
        (dismissal['after'] as Map)['menu_focus_restored'],
        isTrue,
        reason: '$dismissal',
      );
      done('Workspace Escape and menu focus restoration');

      // Clear left the Store empty. Start then cancel a real repository request;
      // no injected failure or successful fallback is used to claim recovery.
      await bounded(
        () async {
          final cancelled = app.discovery.load(EntertainmentCategory.games);
          app.discovery.cancel();
          await cancelled;
        },
        'cancel catalogue request',
        timeout: const Duration(seconds: 20),
      );
      expect(app.discovery.state(EntertainmentCategory.games).loading, isFalse);
      done('explicit catalogue request cancellation');
      await bounded(
        () async {
          final recovery = app.discovery.load(EntertainmentCategory.games);
          await until(
            () => !app.discovery.state(EntertainmentCategory.games).loading,
            'catalogue recovery',
          );
          await recovery;
        },
        'catalogue recovery',
        timeout: const Duration(seconds: 35),
      );
      recordCatalogue('recovery_catalogue');
      await bounded(
        () async {
          final refresh = app.discovery.load(EntertainmentCategory.games);
          await until(
            () => !app.discovery.state(EntertainmentCategory.games).loading,
            'repeat catalogue refresh',
          );
          await refresh;
        },
        'repeat catalogue refresh',
        timeout: const Duration(seconds: 35),
      );
      recordCatalogue('repeat_catalogue');
      report['repeat_refresh_note'] =
          'Fresh TTL may satisfy repeat refresh from validated cache.';
      done('truthful recovery and repeat refresh');

      report['native_activation_available'] = app.windowsActivationAvailable;
      if (app.windowsActivationAvailable) {
        await tap(find.byKey(const Key('search-trigger')));
        expect(find.byKey(const Key('search-input')), findsOneWidget);
        expect(app.overlays.isOpen, isTrue);
        final forwarded = await bounded(
          () => Process.run(Platform.resolvedExecutable, [
            'ryhze://app/activity',
          ], runInShell: false),
          'forward same-executable activity',
          timeout: const Duration(seconds: 20),
        );
        report['activity_forward_exit_code'] = forwarded.exitCode;
        expect(forwarded.exitCode, 0);
        await pump(const Duration(milliseconds: 300));
        expect(find.byKey(const Key('search-input')), findsOneWidget);
        expect(app.overlays.isOpen, isTrue);
        await escape();
        await until(
          () =>
              find.text('Downloads and activity').evaluate().isNotEmpty &&
              find.byKey(const Key('search-input')).evaluate().isEmpty,
          'queued Activity presentation and Search route disposal',
          timeout: const Duration(seconds: 15),
        );
        expect(find.byKey(const Key('search-input')), findsNothing);
        done(
          'same-executable forwarding queued behind Search then Activity drain',
        );
        await escape();
        await until(
          () => !app.overlays.isOpen,
          'Activity route disposal',
          timeout: const Duration(seconds: 15),
        );
      } else {
        report['activity_forwarding'] =
            'native activation unavailable; not exercised';
      }
      expect(tester.takeException(), isNull);
      bodyCompleted = true;
    } finally {
      // A framework timeout can otherwise report success while this test is
      // still awaiting a frame. Completion requires both the body and teardown.
      report['status'] = 'failed';
      cleanupStarted = true;
      deadline.stop();
      binding.reportData!['providers_journey'] = report;
      var widgetDisposed = false;
      try {
        await bounded(
          () => tester.pumpWidget(const SizedBox()),
          'dispose application',
        );
        widgetDisposed = true;
      } finally {
        await bounded(
          () => services?.close() ?? Future<void>.value(),
          'close services',
        );
        report['cleanup_completed'] = widgetDisposed && !bootstrapPending;
        if (bodyCompleted && widgetDisposed && !bootstrapPending) {
          report['status'] = 'completed';
          report.remove('pending_operation');
        }
      }
      // Keep only the isolated journal as optional failure evidence. Its absolute
      // path and any native installation records are excluded from the report.
    }
  }, timeout: const Timeout(Duration(minutes: 2, seconds: 30)));
}

Map<String, Object?> _artworkHealth(PublicArtworkHealth health) => {
  'memory_entries': health.memoryEntries,
  'memory_bytes': health.memoryBytes,
  'active': health.active,
  'queued': health.queued,
  'entries': health.disk!.entries,
  'bytes': health.disk!.bytes,
  'preserved': health.disk!.preserved,
};

// Closed labels only: diagnostics must not retain focus text or native paths.
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
