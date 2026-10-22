import 'dart:async';

import 'package:flutter/foundation.dart';

import '../core/app_failure.dart';
import '../features/activity/activity_repository.dart';
import '../features/installations/installation_controller.dart';
import '../features/installations/installation_repository.dart';
import '../features/installations/content_staging_backend.dart';
import '../features/installations/content_discard_backend.dart';
import '../features/installations/managed_content_discard_service.dart';
import '../features/installations/managed_installation_repository.dart';
import '../features/installations/managed_installation.dart';
import '../features/installations/managed_content_service.dart';
import '../features/installations/managed_content_controller.dart';
import '../features/installations/content_transfer_backend.dart';
import '../features/installations/managed_transfer_record.dart';
import '../features/installations/managed_transfer_repository.dart';
import '../features/installations/managed_transfer_service.dart';
import '../features/installations/managed_transfer_source.dart';
import '../features/installations/managed_transfer_controller.dart';
import '../core/diagnostics.dart';
import '../features/workspace/workspace_controller.dart';
import '../data/transaction_store.dart';
import '../features/collections/collection_repository.dart';
import '../features/jobs/job_coordinator.dart';
import '../features/jobs/job_repository.dart';
import '../features/library/data/library_repository.dart';
import '../features/library/data/library_preferences.dart';
import '../features/library/library_controller.dart';
import '../features/session/session_controller.dart';
import '../features/store/store_controller.dart';
import '../features/store/catalog_repository.dart';
import '../features/store/catalog_browse_controller.dart';
import '../features/store/catalog_maintenance.dart';
import '../platform/desktop_contracts.dart';
import '../platform/windows/windows_lifecycle.dart';
import '../platform/windows/windows_lifecycle_host.dart';
import '../platform/windows/windows_activation.dart';
import '../platform/windows/windows_security.dart';
import '../platform/windows/windows_updates.dart';
import '../platform/windows/windows_taskbar.dart';
import '../platform/windows/windows_notifications.dart';
import '../platform/windows/windows_services_host.dart';
import '../providers/provider_registry.dart';
import '../providers/public_artwork_repository.dart';
import 'bootstrap_storage.dart';
import 'navigation_controller.dart';
import 'route_activation_controller.dart';

/// Composition root owns repositories and long-lived controllers. Widgets own
/// only focus, scroll and text resources. Optional failures do not block browsing.
class AppServices extends ChangeNotifier {
  AppServices({
    required this.storage,
    LibraryRepository? libraryRepository,
    this.storageFailure,
    Map<String, JobOperation> jobOperations = const {},
    CatalogRepository? catalogRepository,
    ContentStagingBackend? contentStaging,
    ContentTransferBackend? contentTransfer,
    ManagedContentTransferAuthority transferAuthority =
        const UnavailableManagedContentTransferAuthority(),
    this.artwork,
  }) : catalog = catalogRepository ?? LocalCatalogRepository() {
    discovery = CatalogBrowseController(catalog);
    collections = CollectionRepository(storage);
    preferences = PreferenceRepository(storage);
    activity = ActivityRepository(storage);
    jobs = JobRepository(storage, activity: activity);
    if (contentStaging != null && storageFailure == null) {
      managedInstallations = ManagedInstallationRepository(
        storage,
        session,
        deviceId: contentStaging.deviceId,
      );
      managedContent = ManagedContentService(
        managedInstallations!,
        contentStaging,
        jobs,
        session,
        authority: transferAuthority,
      );
      final discardBackend = contentStaging is ContentDiscardBackend
          ? contentStaging as ContentDiscardBackend
          : null;
      if (discardBackend != null && discardBackend.discardAvailable) {
        managedDiscard = ManagedContentDiscardService(
          managedInstallations!,
          discardBackend,
          jobs,
          session,
        );
      }
    }
    if (contentTransfer != null && storageFailure == null) {
      transfers = ManagedTransferRepository(
        storage,
        session,
        deviceId: contentTransfer.deviceId,
      );
      managedTransfer = ManagedTransferService(
        repository: transfers!,
        backend: contentTransfer,
        jobs: jobs,
        session: session,
        authority: transferAuthority,
      );
    }
    coordinator = JobCoordinator(jobs, {
      ...jobOperations,
      ManagedContentService.jobKind: ?managedContent,
      ManagedContentDiscardService.jobKind: ?managedDiscard,
      ManagedTransferService.jobKind: ?managedTransfer,
      ManagedTransferService.discardKind: ?managedTransfer,
    });
    if (managedContent != null) {
      managedContentController = ManagedContentController(
        repository: managedInstallations!,
        service: managedContent!,
        session: session,
        coordinator: coordinator,
        discardService: managedDiscard,
      );
    }
    if (managedTransfer != null) {
      transferController = ManagedTransferController(
        service: managedTransfer!,
        coordinator: coordinator,
        session: session,
        staging: managedContent,
      );
    }
    store = StoreController(
      repository: collections,
      session: session,
      writable: storageFailure == null,
    );
    library = LibraryController(
      repository: libraryRepository,
      preferences: storageFailure == null
          ? LibraryPreferences(storage, session.scope)
          : null,
    );
    routes = RouteActivationController(
      catalog: catalog,
      navigation: navigation,
      session: session,
      library: library,
      overlays: overlays,
    );
    session.addListener(_replaceScope);
    workspace = WorkspaceController(
      activity: activity,
      jobs: jobs,
      collections: collections,
      preferences: preferences,
      session: session,
      store: store,
      savePreferences: savePreferences,
      writable: storageFailure == null,
      notificationsAvailable: () => windowsNotificationsAvailable,
      installations: installations,
      managedContent: managedContentController,
      transfers: transferController,
      catalogMaintenance: catalog is CatalogMaintenance
          ? catalog as CatalogMaintenance
          : null,
      clearCatalog: clearPublicCatalog,
      artwork: artwork,
    );
    diagnostics.record(
      storageFailure == null
          ? DiagnosticEvent.startup
          : DiagnosticEvent.storageUnavailable,
    );
    library.load();
  }
  factory AppServices.memory({LibraryRepository? libraryRepository}) =>
      AppServices(
        storage: MemoryTransactionStore(),
        libraryRepository: libraryRepository,
      );
  static Future<AppServices> bootstrap({
    Future<TransactionStore> Function()? openStore,
    Future<WindowsLifecycleClient?> Function()? connectLifecycle,
    Future<WindowsSecurityClient?> Function()? connectSecurity,
    Future<WindowsActivationClient?> Function()? connectActivation,
    Future<WindowsUpdateClient?> Function()? connectUpdates,
    Future<WindowsTaskbarClient?> Function()? connectTaskbar,
    Future<WindowsNotificationsClient?> Function()? connectNotifications,
    Future<InstallationRepository?> Function()? connectInstallations,
    Future<ContentStagingBackend?> Function()? connectContentStaging,
    Future<ContentTransferBackend?> Function()? connectContentTransfer,
    Future<PublicArtworkRepository?> Function()? connectArtwork,
    CatalogRepository? catalogRepository,
  }) async {
    TransactionStore? opened;
    AppServices? candidate;
    ContentStagingBackend? contentStaging;
    ContentTransferBackend? contentTransfer;
    PublicArtworkRepository? artwork;
    try {
      opened = await (openStore ?? openApplicationStore)();
      try {
        final managed = opened.read()['managedInstallations'];
        // Preserve future optional metadata without disabling unrelated data.
        if (supportsManagedInstallations(managed)) {
          contentStaging =
              await (connectContentStaging ?? connectWindowsContentStaging)();
        }
      } catch (_) {
        /* Inert local staging is optional and establishes no provider rights. */
      }
      try {
        if (supportsManagedTransfers(opened.read()['managedTransfers'])) {
          contentTransfer =
              await (connectContentTransfer ?? connectWindowsContentTransfer)();
        }
      } catch (_) {
        // Transfer is optional; platform presence establishes no content rights.
      }
      try {
        artwork = await (connectArtwork ?? connectWindowsArtwork)();
      } catch (_) {
        // Optional public images never block catalogue or user-data startup.
      }
      candidate = AppServices(
        storage: opened,
        catalogRepository: catalogRepository ?? createWindowsCatalog(opened),
        contentStaging: contentStaging,
        contentTransfer: contentTransfer,
        artwork: artwork,
      );
      await candidate.managedContent?.recoverInterrupted();
      await candidate.managedDiscard?.recoverInterrupted();
      await candidate.managedTransfer?.recoverInterrupted();
      await candidate.coordinator.recover(candidate.session.scope);
      // Optional native capability failure must not discard a healthy store.
      try {
        candidate.attachWindowsLifecycle(
          await (connectLifecycle ?? connectWindowsLifecycle)(),
        );
      } catch (_) {
        // The normal browsing/storage path remains available.
      }
      try {
        candidate.windowsSecurity =
            await (connectSecurity ?? connectWindowsSecurity)();
        if (candidate.windowsSecurity?.secureCredentials ?? false) {
          candidate.windowsCredentials = WindowsCredentials(
            candidate.session,
            candidate.windowsSecurity!,
          );
        }
      } catch (_) {
        /* Optional capability; no authentication is inferred. */
      }
      try {
        candidate.attachWindowsActivation(
          await (connectActivation ?? connectWindowsActivation)(),
        );
      } catch (_) {
        /* Browsing does not depend on external activation. */
      }
      try {
        candidate.windowsUpdates =
            await (connectUpdates ?? connectWindowsUpdates)();
      } catch (_) {
        /* Signed publisher policy is an independent dependency. */
      }
      try {
        final taskbar = await (connectTaskbar ?? connectWindowsTaskbar)();
        if (taskbar != null && taskbar.available) {
          candidate.windowsTaskbar = WindowsTaskbarBinding(
            taskbar,
            candidate.jobs,
            candidate.activity,
            candidate.session,
          );
        }
      } catch (_) {
        /* Native status is optional; it cannot block browsing or storage. */
      }
      try {
        final notifications =
            await (connectNotifications ?? connectWindowsNotifications)();
        if (notifications != null) {
          candidate.windowsNotificationsAvailable = notifications.available;
          final settings = candidate.preferences.read();
          candidate.windowsNotifications = WindowsNotificationsBinding(
            notifications,
            candidate.activity,
            candidate.session,
            enabled: settings.notifications,
            privatePreviews: settings.privatePreviews,
          );
        }
      } catch (_) {
        /* The in-app Activity record remains authoritative when delivery fails. */
      }
      try {
        final inventory =
            await (connectInstallations ?? connectWindowsInstallations)();
        if (inventory != null) candidate.installations.attach(inventory);
      } catch (_) {
        /* Device inventory is optional and confers no entitlement. */
      }
      return candidate;
    } catch (_) {
      try {
        if (candidate != null) {
          await candidate.close();
        } else {
          final failures = _SettlementFailures();
          await Future.wait<void>([
            failures.wait(() => contentStaging?.close()),
            failures.wait(() => contentTransfer?.close()),
            failures.wait(() => artwork?.close()),
          ]);
          await failures.wait(() => opened?.close());
          failures.rethrowFirst();
        }
      } catch (_) {
        // Cleanup failure must not suppress the safe read-only fallback.
      }
      return AppServices(
        storage: MemoryTransactionStore(),
        storageFailure: const AppFailure(FailureKind.recovery),
      );
    }
  }

  final TransactionStore storage;
  final AppFailure? storageFailure;
  final session = SessionController();
  final diagnostics = Diagnostics();
  final installations = InstallationController();
  ManagedInstallationRepository? managedInstallations;
  ManagedContentService? managedContent;
  ManagedTransferRepository? transfers;
  ManagedTransferService? managedTransfer;
  ManagedTransferController? transferController;
  ManagedContentDiscardService? managedDiscard;
  Future<void>? _managedClosing;
  ManagedContentController? managedContentController;
  late final WorkspaceController workspace;
  final navigation = NavigationController();
  final overlays = OverlayCoordinator();
  final providers = ProviderRegistry();
  final CatalogRepository catalog;
  final PublicArtworkRepository? artwork;
  bool windowsNotificationsAvailable = false;
  WindowsNotificationsBinding? windowsNotifications;
  late final CatalogBrowseController discovery;
  late final RouteActivationController routes;
  final DesktopAdapter desktop = const UnavailableDesktopAdapter();
  final integrations = const [
    OptionalIntegration('Gideon'),
    OptionalIntegration('RACE'),
  ];
  late final CollectionRepository collections;
  late final PreferenceRepository preferences;
  late final ActivityRepository activity;
  late final JobRepository jobs;
  late final JobCoordinator coordinator;
  late final StoreController store;
  late final LibraryController library;
  bool _disposed = false;
  WindowsLifecycleClient? _windowsLifecycle;
  WindowsSecurityClient? windowsSecurity;
  WindowsCredentials? windowsCredentials;
  WindowsUpdateClient? windowsUpdates;
  WindowsTaskbarBinding? windowsTaskbar;
  WindowsActivationClient? _windowsActivation;
  bool _drainingActivation = false;
  bool get windowsActivationAvailable => _windowsActivation != null;
  void attachWindowsActivation(WindowsActivationClient? client) {
    if (identical(client, _windowsActivation)) return;
    _windowsActivation?.removeListener(_scheduleActivationDrain);
    _windowsActivation?.dispose();
    overlays.removeListener(_scheduleActivationDrain);
    routes.removeListener(_scheduleActivationDrain);
    _windowsActivation = null;
    if (_disposed) {
      client?.dispose();
      return;
    }
    _windowsActivation = client;
    client?.addListener(_scheduleActivationDrain);
    if (client != null) {
      overlays.addListener(_scheduleActivationDrain);
      routes.addListener(_scheduleActivationDrain);
      _scheduleActivationDrain();
    }
  }

  void _scheduleActivationDrain() => scheduleMicrotask(_drainActivations);

  void _drainActivations() {
    if (_disposed ||
        _drainingActivation ||
        overlays.isOpen ||
        routes.value.hasPendingPresentation ||
        _windowsActivation == null) {
      return;
    }
    final client = _windowsActivation!;
    _drainingActivation = true;
    unawaited(() async {
      try {
        while (!_disposed &&
            identical(client, _windowsActivation) &&
            !overlays.isOpen &&
            !routes.value.hasPendingPresentation &&
            client.pending.isNotEmpty) {
          final event = client.pending.first;
          if (event.route != null) await routes.activate(event.route!);
          if (_disposed) return;
          await client.acknowledge(event);
        }
      } catch (_) {
        // An unacknowledged event remains in the bounded native queue. Do not
        // spin or discard it on a channel failure.
      } finally {
        _drainingActivation = false;
        if (!_disposed && !identical(client, _windowsActivation)) {
          _scheduleActivationDrain();
        }
      }
    }());
  }

  bool _lifecycleInterrupted = false;
  bool get windowsPowerEventsAvailable => _windowsLifecycle != null;
  Future<void>? _closing;
  Future<void>? _lifecycleRecovery;
  int? _lifecycleRecoveryGeneration;
  int _lifecycleInterruption = 0;
  int? _lifecycleRecoveryInterruption;
  AppFailure? _lifecycleFailure;
  AppFailure? get lifecycleFailure => _lifecycleFailure;

  /// The composition root owns the bridge and its listener, including disposal.
  void attachWindowsLifecycle(WindowsLifecycleClient? client) {
    if (identical(client, _windowsLifecycle)) return;
    _windowsLifecycle?.removeListener(_handleWindowsLifecycle);
    _windowsLifecycle?.dispose();
    _windowsLifecycle = null;
    if (_disposed) {
      client?.dispose();
      return;
    }
    _windowsLifecycle = client;
    client?.addListener(_handleWindowsLifecycle);
    if (client != null) _handleWindowsLifecycle();
  }

  void _handleWindowsLifecycle() {
    if (_disposed) return;
    switch (_windowsLifecycle!.value.state) {
      case WindowsLifecycleState.suspended:
      case WindowsLifecycleState.ending:
        _lifecycleInterrupted = true;
        interruptLifecycle();
      case WindowsLifecycleState.active:
        if (_lifecycleInterrupted) {
          _lifecycleInterrupted = false;
          unawaited(recoverAfterResume());
        }
      case WindowsLifecycleState.resumed:
        // Critical resume may arrive without a prior suspend notification.
        _lifecycleInterrupted = false;
        unawaited(recoverAfterResume());
    }
  }

  void interruptLifecycle() {
    if (_disposed) return;
    // Close admission before invoking cancellation callbacks. Each owner must
    // receive the interruption even if an earlier owner's callback throws.
    _lifecycleInterrupted = true;
    _lifecycleInterruption++;
    final failures = _SettlementFailures();
    failures.run(coordinator.suspend);
    failures.run(installations.interrupt);
    failures.run(() => artwork?.interrupt());
    failures.run(() => managedContentController?.dismissRemoval());
    failures.run(() => transferController?.dismissRemoval());
    failures.run(() => managedContent?.interrupt());
    failures.run(() => managedDiscard?.interrupt());
    failures.run(() => managedTransfer?.interrupt());
    failures.run(() => windowsUpdates?.interrupt());
    failures.run(() => windowsTaskbar?.setSuspended(true));
    failures.run(discovery.cancel);
    failures.rethrowFirst();
  }

  void _replaceScope() {
    final failures = _SettlementFailures();
    failures.run(() => installations.cancel(clear: true));
    failures.run(discovery.cancel);
    final recoverNewScope =
        !_lifecycleInterrupted &&
        (_lifecycleRecovery != null || _lifecycleFailure != null);
    failures.run(coordinator.cancelActive);
    failures.run(() => managedContent?.cancelPending());
    failures.run(() => managedDiscard?.cancelPending());
    failures.run(() => managedTransfer?.cancelPending());
    _lifecycleFailure = null;
    diagnostics.record(DiagnosticEvent.scopeReplaced);
    failures.run(navigation.resetScope);
    failures.run(store.resetScope);
    failures.run(() {
      library.setRepository(
        const UnavailableLibraryRepository(),
        preferences: storageFailure == null
            ? LibraryPreferences(storage, session.scope)
            : null,
      );
    });
    if (recoverNewScope) {
      unawaited(recoverAfterResume());
    }
    notifyListeners();
    failures.rethrowFirst();
  }

  Future<void> savePreferences(Preferences value) async {
    if (storageFailure != null) throw storageFailure!;
    await preferences.save(value);
    windowsNotifications?.configure(
      enabled: value.notifications,
      privatePreviews: value.privatePreviews,
    );
    diagnostics.record(DiagnosticEvent.preferenceSaved);
    if (!_disposed) notifyListeners();
  }

  Future<void> clearPublicCatalog() async {
    if (_disposed || storageFailure != null || catalog is! CatalogMaintenance) {
      throw const AppFailure(FailureKind.unsupported);
    }
    await (catalog as CatalogMaintenance).clearCache();
    if (!_disposed) {
      routes.invalidate();
      discovery.reset();
    }
  }

  /// Reconcile durable status after a platform interruption, without rerunning
  /// external actions. Account replacement invalidates completion and UI state.
  Future<void> recoverAfterResume() {
    if (_disposed) return Future.value();
    _lifecycleInterrupted = false;
    final generation = session.generation;
    final interruption = _lifecycleInterruption;
    final scope = session.scope;
    if (_lifecycleRecoveryGeneration == generation &&
        _lifecycleRecoveryInterruption == interruption &&
        _lifecycleRecovery != null) {
      return _lifecycleRecovery!;
    }
    final previous = _lifecycleRecovery;
    late final Future<void> recovery;
    recovery = () async {
      try {
        await previous;
        if (_disposed ||
            generation != session.generation ||
            interruption != _lifecycleInterruption) {
          return;
        }
        await coordinator.quiesce();
        if (_disposed ||
            generation != session.generation ||
            interruption != _lifecycleInterruption) {
          return;
        }
        await managedContent?.recoverInterrupted();
        if (_disposed ||
            generation != session.generation ||
            interruption != _lifecycleInterruption) {
          return;
        }
        await managedDiscard?.recoverInterrupted();
        if (_disposed ||
            generation != session.generation ||
            interruption != _lifecycleInterruption) {
          return;
        }
        await windowsUpdates?.recoverAfterResume();
        await managedTransfer?.recoverInterrupted();
        if (_disposed ||
            generation != session.generation ||
            interruption != _lifecycleInterruption) {
          return;
        }
        await coordinator.resume(scope);
        if (_disposed ||
            generation != session.generation ||
            interruption != _lifecycleInterruption) {
          return;
        }
        _lifecycleFailure = null;
        managedContent?.resume();
        managedDiscard?.resume();
        managedTransfer?.resume();
        windowsTaskbar?.setSuspended(false);
      } catch (_) {
        if (_disposed ||
            generation != session.generation ||
            interruption != _lifecycleInterruption) {
          return;
        }
        _lifecycleFailure = const AppFailure(FailureKind.recovery);
      } finally {
        if (identical(_lifecycleRecovery, recovery)) {
          _lifecycleRecovery = null;
          _lifecycleRecoveryGeneration = null;
          _lifecycleRecoveryInterruption = null;
        }
        if (!_disposed &&
            generation == session.generation &&
            interruption == _lifecycleInterruption) {
          notifyListeners();
        }
      }
    }();
    _lifecycleRecovery = recovery;
    _lifecycleRecoveryGeneration = generation;
    _lifecycleRecoveryInterruption = interruption;
    return recovery;
  }

  Future<void> close() {
    if (_closing != null) return _closing!;
    // Cancellation callbacks can call close again before _close first yields.
    // Publish ownership first so they join the same shutdown and store close.
    final completion = Completer<void>();
    _closing = completion.future;
    unawaited(
      _close().then(completion.complete, onError: completion.completeError),
    );
    return completion.future;
  }

  Future<void> _close() async {
    final failures = _SettlementFailures();
    failures.run(dispose);
    // Invoke and join every owner even after synchronous cancellation failed.
    // The durable store stays open until all settlement futures have completed.
    await Future.wait<void>([
      failures.wait(coordinator.close),
      failures.wait(() => _lifecycleRecovery),
      failures.wait(() => windowsCredentials?.settled),
      failures.wait(() => windowsTaskbar?.close()),
      failures.wait(() => windowsNotifications?.close()),
      failures.wait(() => windowsUpdates?.close()),
      failures.wait(_closeManagedContent),
      failures.wait(() => managedTransfer?.close()),
      failures.wait(() => artwork?.close()),
    ]);
    failures.run(() => managedInstallations?.dispose());
    failures.run(() => transfers?.dispose());
    failures.run(() => managedTransfer?.dispose());
    await failures.wait(storage.close);
    failures.rethrowFirst();
  }

  Future<void> _closeManagedContent() => _managedClosing ??= () async {
    // Discard shares the staging channel. Join its owner before staging closes
    // the channel, including when settlement reports an error.
    try {
      await managedDiscard?.close();
    } finally {
      await managedContent?.close();
    }
  }();

  @override
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    final failures = _SettlementFailures();
    failures.run(coordinator.suspend);
    failures.run(() => unawaited(artwork?.close().catchError((Object _) {})));
    failures.run(
      () => unawaited(windowsTaskbar?.close().catchError((Object _) {})),
    );
    failures.run(
      () => unawaited(windowsNotifications?.close().catchError((Object _) {})),
    );
    failures.run(
      () => unawaited(_closeManagedContent().catchError((Object _) {})),
    );
    failures.run(
      () => unawaited(managedTransfer?.close().catchError((Object _) {})),
    );
    failures.run(
      () => unawaited(windowsUpdates?.close().catchError((Object _) {})),
    );
    failures.run(() => windowsCredentials?.dispose());
    failures.run(
      () => _windowsLifecycle?.removeListener(_handleWindowsLifecycle),
    );
    failures.run(() => _windowsLifecycle?.dispose());
    _windowsLifecycle = null;
    failures.run(
      () => _windowsActivation?.removeListener(_scheduleActivationDrain),
    );
    failures.run(() => _windowsActivation?.dispose());
    _windowsActivation = null;
    failures.run(() => overlays.removeListener(_scheduleActivationDrain));
    failures.run(() => routes.removeListener(_scheduleActivationDrain));
    failures.run(routes.dispose);
    failures.run(discovery.dispose);
    failures.run(installations.dispose);
    failures.run(() => managedContentController?.dispose());
    failures.run(() => transferController?.dispose());
    failures.run(workspace.dispose);
    failures.run(jobs.dispose);
    failures.run(activity.dispose);
    failures.run(() => session.removeListener(_replaceScope));
    failures.run(session.dispose);
    failures.run(navigation.dispose);
    failures.run(store.dispose);
    failures.run(library.dispose);
    failures.run(overlays.dispose);
    failures.run(super.dispose);
    failures.rethrowFirst();
  }
}

/// Keep the original failure observable without abandoning other owned work.
class _SettlementFailures {
  (Object, StackTrace)? _first;

  void run(void Function() action) {
    try {
      action();
    } catch (error, stack) {
      _first ??= (error, stack);
    }
  }

  Future<void> wait(Future<void>? Function() action) async {
    try {
      await action();
    } catch (error, stack) {
      _first ??= (error, stack);
    }
  }

  void rethrowFirst() {
    final failure = _first;
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }
}
