import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/installation_repository.dart';
import 'package:ryhze/features/store/catalog_maintenance.dart';
import 'package:ryhze/features/workspace/workspace_panel.dart';
import 'package:ryhze/providers/public_artwork_cache.dart';
import 'package:ryhze/providers/public_artwork_repository.dart';
import 'package:ryhze/providers/public_catalog_cache.dart';
import 'package:ryhze/providers/ryhze_catalog_repository.dart';

import 'artwork_fixture.dart';
import '../test_driver/journey_focus.dart';
import 'installation_controller_test.dart' as installations;
import 'public_artwork_repository_test.dart' as artwork;
import 'ryhze_catalog_repository_test.dart' as catalogue;

class _Inspection implements InstallationRepository {
  final pending = Completer<RepositoryRead<InstallationObservation>>();
  int calls = 0;

  @override
  Future<RepositoryRead<InstallationObservation>> inspect(
    CancellationToken token,
  ) {
    token.check();
    calls++;
    return pending.future;
  }
}

// Only fixed widget/focus labels and route state are included. Native records,
// account data and device paths must not become keyboard failure diagnostics.
String _routeFocus(AppServices services, WidgetTester tester) {
  final primary = FocusManager.instance.primaryFocus;
  final focusContext = primary?.context;
  final focusedRoute = focusContext == null
      ? null
      : ModalRoute.of(focusContext);
  final workspace = find.byType(WorkspacePanel);
  final workspaceRoute = workspace.evaluate().isEmpty
      ? null
      : ModalRoute.of(tester.element(workspace));
  return 'focus=${primary?.debugLabel}; '
      'scope=${primary?.enclosingScope?.debugLabel}; '
      'focusedRoute=${focusedRoute?.runtimeType}; '
      'focusedRouteCurrent=${focusedRoute?.isCurrent}; '
      'workspaceRoute=${workspaceRoute?.runtimeType}; '
      'workspaceRouteCurrent=${workspaceRoute?.isCurrent}; '
      'focusOwnsWorkspace=${workspaceRoute != null && identical(focusedRoute, workspaceRoute)}; '
      'overlayOpen=${services.overlays.isOpen}';
}

void main() {
  testWidgets(
    'menu Workspace retains Escape after both cache clears and inspection',
    (tester) async {
      tester.view.physicalSize = const Size(1280, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final now = DateTime.utc(2026, 10, 11);
      final storage = MemoryTransactionStore();
      final metadataTransport = catalogue.Transport([
        (_) => catalogue.encode([catalogue.entry('journey')]),
      ]);
      final metadata = RyhzeCatalogRepository(
        cache: PublicCatalogCache(storage),
        transport: metadataTransport,
        clock: () => now,
      );
      final imageCache = artwork.Cache()
        ..values[artworkRef().key] = PublicArtworkCached(artworkPng(), now);
      final images = PublicArtworkRepository(
        cache: imageCache,
        transport: artwork.Transport(),
        clock: () => now,
      );
      final inspection = _Inspection();
      final services = AppServices(
        storage: storage,
        catalogRepository: metadata,
        artwork: images,
      );
      services.installations.attach(inspection);
      final focusHistory = <String>[];

      Future<void> settle(String phase) async {
        await tester.pumpAndSettle(
          const Duration(milliseconds: 100),
          EnginePhase.sendSemanticsUpdate,
          const Duration(seconds: 5),
        );
        focusHistory.add('$phase: ${_routeFocus(services, tester)}');
      }

      Future<void> tap(Finder target, String phase) async {
        await tester.ensureVisible(target);
        await tester.tap(target);
        await settle(phase);
      }

      try {
        await tester.pumpWidget(RyhzeApp(services: services));
        await settle('mounted');
        expect(metadata.cacheHealth().state, CatalogCacheState.fresh);
        expect(metadataTransport.calls, 1);
        final preferencesBefore = storage.read()['preferences'];
        final libraryBefore = services.library.entries.length;
        final menuNode = tester
            .widget<RyhzeControl>(find.byKey(const Key('menu-trigger')))
            .focusNode!;
        // Profile and Release omit all debug labels. Restoration must still
        // return to this exact control after the full Workspace lifecycle.
        menuNode.debugLabel = null;

        await tap(find.byKey(const Key('menu-trigger')), 'menu opened');
        await tap(find.byKey(const Key('menu-workspace')), 'Workspace opened');
        expect(find.byType(WorkspacePanel), findsOneWidget);
        expect(services.overlays.isOpen, isTrue);
        await tap(find.text('Settings').first, 'Settings selected');
        expect(find.text('Reduce motion'), findsOneWidget);
        await tap(find.text('Support').first, 'Support selected');
        expect(find.text('Public catalogue cache'), findsOneWidget);
        expect(find.text('Public artwork cache'), findsOneWidget);

        await tap(find.text('Clear catalogue cache'), 'metadata cleared');
        expect(find.text('Catalogue cache cleared.'), findsOneWidget);
        expect(metadata.cacheHealth().state, CatalogCacheState.empty);
        expect(imageCache.values, hasLength(1));
        await tap(find.text('Clear artwork cache'), 'artwork cleared');
        expect(find.textContaining('Artwork cache cleared.'), findsOneWidget);
        expect(imageCache.clears, 1);
        expect(imageCache.values, isEmpty);
        expect(metadata.cacheHealth().state, CatalogCacheState.empty);
        expect(metadataTransport.calls, 1);
        expect(storage.read()['preferences'], preferencesBefore);

        await tap(find.text('Installations').first, 'Installations selected');
        await tap(
          find.text('Inspect Steam installations'),
          'inspection pending',
        );
        expect(inspection.calls, 1);
        expect(services.installations.busy, isTrue);
        expect(find.text('Cancel'), findsOneWidget);
        inspection.pending.complete(installations.result());
        await settle('inspection completed');
        expect(services.installations.busy, isFalse);
        expect(find.text('Local inspection completed.'), findsOneWidget);
        expect(services.library.entries.length, libraryBefore);

        await tester.sendKeyEvent(
          LogicalKeyboardKey.escape,
          physicalKey: PhysicalKeyboardKey.escape,
        );
        await settle('Escape dispatched');
        final diagnostics = focusHistory.join('\n');
        expect(find.byType(WorkspacePanel), findsNothing, reason: diagnostics);
        expect(services.overlays.isOpen, isFalse, reason: diagnostics);
        expect(
          FocusManager.instance.primaryFocus,
          same(menuNode),
          reason: diagnostics,
        );
        expect(menuTriggerHasPrimaryFocus(), isTrue, reason: diagnostics);
        expect(tester.takeException(), isNull);
      } finally {
        try {
          await tester.pumpWidget(const SizedBox());
        } finally {
          await services.close();
        }
      }
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
}
