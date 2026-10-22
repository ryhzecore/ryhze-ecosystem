import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../store/artwork_maintenance.dart';
import '../../domain/activity.dart';
import '../activity/activity_repository.dart';
import '../jobs/job_repository.dart';
import '../collections/collection_repository.dart';
import '../session/session_controller.dart';
import '../store/store_controller.dart';
import '../installations/installation_controller.dart';
import '../installations/managed_content_controller.dart';
import '../installations/managed_transfer_controller.dart';
import '../store/catalog_maintenance.dart';

class WorkspaceController extends ChangeNotifier {
  WorkspaceController({
    required this.activity,
    required this.jobs,
    required this.collections,
    required this.preferences,
    required this.session,
    required this.store,
    required this.savePreferences,
    required this.writable,
    this.notificationsAvailable,
    this.installations,
    this.managedContent,
    this.transfers,
    this.catalogMaintenance,
    this.clearCatalog,
    this.artwork,
  }) {
    activity.addListener(_changed);
    jobs.addListener(_changed);
    session.addListener(_changed);
    store.addListener(_changed);
    installations?.addListener(_changed);
    managedContent?.addListener(_changed);
    transfers?.addListener(_changed);
  }
  final ActivityRepository activity;
  final JobRepository jobs;
  List<ActivityRecord> get events => activity.read(session.scope);
  List<JobRecord> get jobRecords => jobs.read(session.scope);
  Future<void> acknowledge(ActivityRecord record) =>
      _run(() => activity.acknowledge(session.scope, record.id));
  final CollectionRepository collections;
  final PreferenceRepository preferences;
  final SessionController session;
  final StoreController store;
  final Future<void> Function(Preferences) savePreferences;
  final bool writable;
  final bool Function()? notificationsAvailable;
  final InstallationController? installations;
  final ManagedContentController? managedContent;
  final ManagedTransferController? transfers;
  final CatalogMaintenance? catalogMaintenance;
  final Future<void> Function()? clearCatalog;
  final ArtworkMaintenance? artwork;
  bool _busy = false;
  bool get busy => _busy;
  AppFailure? get failure => _failure;
  bool _disposed = false;
  AppFailure? _failure;
  List<SavedTitle> get saved => collections.read(session.scope);
  Preferences get settings => preferences.read();
  void _changed() {
    if (!_disposed) {
      notifyListeners();
    }
  }

  Future<void> _run(Future<void> Function() operation) async {
    if (_busy || _disposed) return;
    if (!writable) {
      _failure = const AppFailure(FailureKind.storage);
      notifyListeners();
      return;
    }
    _busy = true;
    _failure = null;
    final generation = session.generation;
    notifyListeners();
    try {
      if (session.generation != generation) return;
      await operation();
    } catch (error) {
      if (!_disposed && session.generation == generation) {
        _failure = error is AppFailure
            ? error
            : const AppFailure(FailureKind.storage);
      }
    } finally {
      if (!_disposed) {
        _busy = false;
        if (session.generation == generation) store.resetScope();
        notifyListeners();
      }
    }
  }

  Future<void> addManual(String title) => _run(
    () => collections.setSaved(
      session.scope,
      SavedTitle(
        'manual:${DateTime.now().microsecondsSinceEpoch}',
        title.trim(),
      ),
      true,
    ),
  );
  Future<void> remove(SavedTitle title) =>
      _run(() => collections.setSaved(session.scope, title, false));
  Future<void> updateSettings(Preferences value) =>
      _run(() => savePreferences(value));
  @override
  void dispose() {
    _disposed = true;
    activity.removeListener(_changed);
    jobs.removeListener(_changed);
    session.removeListener(_changed);
    store.removeListener(_changed);
    installations?.removeListener(_changed);
    managedContent?.removeListener(_changed);
    transfers?.removeListener(_changed);
    super.dispose();
  }
}
