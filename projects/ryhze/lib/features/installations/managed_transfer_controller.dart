import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../jobs/job_coordinator.dart';
import '../jobs/job_repository.dart';
import '../session/session_controller.dart';
import 'managed_content_service.dart';
import 'managed_installation.dart';
import 'managed_transfer_record.dart';
import 'managed_transfer_service.dart';

class ManagedTransferController extends ChangeNotifier {
  ManagedTransferController({
    required this.service,
    required this.coordinator,
    required this.session,
    this.staging,
  }) {
    service.repository.addListener(_repositoryChanged);
    staging?.repository.addListener(_stagingChanged);
    service.addListener(_changed);
    session.addListener(_scopeChanged);
  }
  final ManagedTransferService service;
  final JobCoordinator coordinator;
  final SessionController session;
  final ManagedContentService? staging;
  CancellationToken? _request;
  bool _disposed = false, cancellationRequested = false;
  int? _generation;
  AppFailure? failure;
  ({String operationId, int revision, int generation})? _removal;
  String? get removalId => _removal?.operationId;
  bool get busy => _request != null || service.busy;
  bool get activeInCurrentScope =>
      _generation == session.generation && _request != null;
  List<ManagedTransferRecord>? _records;
  Set<String>? _discardable;
  int _projectionGeneration = -1;
  void _ensureProjectionScope() {
    if (_projectionGeneration != session.generation) {
      _projectionGeneration = session.generation;
      _records = null;
      _discardable = null;
    }
  }

  List<ManagedTransferRecord> get records {
    _ensureProjectionScope();
    return _records ??= service.repository.readCurrent();
  }

  Set<String> get discardableIds {
    _ensureProjectionScope();
    return _discardable ??= service.repository.discardableCurrent();
  }

  int _page = 0;
  int get pageCount => ((records.length + 19) ~/ 20).clamp(1, 7);
  int get page => _page.clamp(0, pageCount - 1);
  List<ManagedTransferRecord> get visibleRecords =>
      List.unmodifiable(records.skip(page * 20).take(20));
  void changePage(int delta) {
    _page = (page + delta).clamp(0, pageCount - 1);
    _changed();
  }

  void _changed() {
    if (!_disposed) notifyListeners();
  }

  void _repositoryChanged() {
    _records = null;
    _discardable = null;
    _changed();
  }

  void _stagingChanged() {
    _records = null;
    _discardable = null;
    _changed();
  }

  void _scopeChanged() {
    _ensureProjectionScope();
    cancel();
    _removal = null;
    failure = null;
    _page = 0;
    _changed();
  }

  void dismissRemoval() {
    _removal = null;
    _changed();
  }

  void cancel() {
    cancellationRequested = true;
    _request?.cancel();
    _changed();
  }

  Future<void> _run(Future<void> Function(CancellationToken) work) async {
    if (_disposed || busy) return;
    final token = _request = session.beginWork();
    final generation = _generation = session.generation;
    failure = null;
    cancellationRequested = false;
    _changed();
    try {
      await work(token);
    } catch (error) {
      if (!_disposed && generation == session.generation) {
        failure = error is AppFailure
            ? error
            : const AppFailure(FailureKind.recovery);
      }
    } finally {
      session.finishWork(token);
      _request = null;
      if (!_disposed) _changed();
    }
  }

  Future<void> _runJob(
    AccountScope scope,
    JobRecord job,
    CancellationToken token,
  ) async {
    JobAdmissionLease? reservation;
    try {
      token.check();
      reservation = coordinator.acquireIdle();
      await reservation.run(scope, job, token);
    } catch (_) {
      // Preparation has already committed. A cancelled request or a lost idle
      // lease must not leave its exact queued attempt available to a later drain.
      try {
        await coordinator.repository.cancelUnstarted(scope, job);
      } catch (_, stack) {
        try {
          coordinator.suspend();
        } finally {
          Error.throwWithStackTrace(const JobSettlementFailure(), stack);
        }
      }
      rethrow;
    } finally {
      reservation?.release();
    }
  }

  Future<void> inspect(String id) => _run((token) async {
    _removal = null;
    await service.inspect(id, token, coordinator: coordinator);
  });

  /// Called only by a configured provider action with an admitted manifest.
  /// There is no local UI for inventing source URLs or verified ownership.
  Future<void> download(ManagedStageIntent intent) => _run((token) async {
    final scope = session.scope;
    _removal = null;
    final job = await service.prepare(intent, token, coordinator: coordinator);
    await _runJob(scope, job, token);
  });
  Future<void> retry(String id) => _run((token) async {
    final scope = session.scope;
    _removal = null;
    final job = await service.prepareRetry(id, token, coordinator: coordinator);
    await _runJob(scope, job, token);
  });
  Future<void> stage(String id) => _run((token) async {
    final scope = session.scope;
    if (staging == null) throw const AppFailure(FailureKind.unsupported);
    _removal = null;
    final job = await service.prepareStage(
      id,
      token,
      coordinator: coordinator,
      staging: staging!,
    );
    await _runJob(scope, job, token);
  });
  Future<void> reviewRemoval(String id) => _run((token) async {
    _removal = null;
    await service.inspect(id, token, coordinator: coordinator);
    token.check();
    final record = service.repository.findCurrent(id);
    if (record == null) {
      throw const AppFailure(FailureKind.recovery);
    }
    _removal = (
      operationId: id,
      revision: record.revision,
      generation: session.generation,
    );
  });
  Future<void> confirmRemoval() => _run((token) async {
    final scope = session.scope;
    final confirmation = _removal;
    _removal = null;
    if (confirmation == null ||
        confirmation.generation != session.generation ||
        service.repository.findCurrent(confirmation.operationId)?.revision !=
            confirmation.revision) {
      throw const AppFailure(FailureKind.conflict);
    }
    final job = await service.prepareDiscard(
      confirmation.operationId,
      token,
      coordinator: coordinator,
    );
    await _runJob(scope, job, token);
  });
  @override
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _request?.cancel();
    service.repository.removeListener(_repositoryChanged);
    staging?.repository.removeListener(_stagingChanged);
    service.removeListener(_changed);
    session.removeListener(_scopeChanged);
    super.dispose();
  }
}
