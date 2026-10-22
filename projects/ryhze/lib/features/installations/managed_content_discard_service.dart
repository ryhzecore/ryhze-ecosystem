import 'dart:async';
import 'dart:typed_data';

import '../../core/app_failure.dart';
import '../jobs/job_coordinator.dart';
import '../jobs/job_repository.dart';
import '../session/session_controller.dart';
import 'content_discard_backend.dart';
import 'content_staging_backend.dart';
import 'managed_installation.dart';
import 'managed_installation_repository.dart';

/// Private-issued, one-use confirmation. No widget receives deletion paths or a
/// mutable plan. Cancelling/dismissing it never commits an intent or deletes.
class ManagedDiscardConfirmation {
  ManagedDiscardConfirmation._(
    this._lease,
    Uint8List plan,
    this.verifiedBytes,
    this.verifiedChunks,
    this.recovery,
    this.removedBytes,
    this.removedFiles,
  ) : _plan = Uint8List.fromList(plan).asUnmodifiableView();
  final ManagedInstallationLease _lease;
  final Uint8List _plan;
  final int verifiedBytes, verifiedChunks, removedBytes, removedFiles;
  final bool recovery;
  String get operationId => _lease.record.operationId;
  String get packageId => _lease.record.intent.packageId;
  String get version => _lease.record.intent.version;
  int get fileCount => verifiedChunks + 1;
  int get remainingBytes => verifiedBytes - removedBytes;
  int get remainingFileCount => fileCount - removedFiles;
}

/// Deletes only a durably admitted native plan, never incoming source content.
/// Generic queue draining cannot replay deletion: execution needs the one-use
/// selection held by the live explicit confirmation call.
class ManagedContentDiscardService implements JobOperation {
  ManagedContentDiscardService(
    this.repository,
    this.backend,
    this.jobs,
    this.session,
  ) {
    repository.addListener(_invalidate);
    session.addListener(_invalidate);
  }
  static const jobKind = 'managedContentDiscard';
  final ManagedInstallationRepository repository;
  final ContentDiscardBackend backend;
  final JobRepository jobs;
  final SessionController session;
  final Set<CancellationToken> _tokens = {};
  Future<void>? _active;
  Future<void>? _closing;
  Future<void>? _recovering;
  ManagedDiscardConfirmation? _confirmation;
  String? _selectedJobKey;
  bool _closed = false, _suspended = false;

  bool isCurrent(ManagedDiscardConfirmation confirmation) {
    if (_closed ||
        _suspended ||
        _active != null ||
        _recovering != null ||
        !identical(_confirmation, confirmation) ||
        confirmation._lease.sessionGeneration != session.generation) {
      return false;
    }
    final current = repository.findCurrent(confirmation.operationId);
    return current != null &&
        current.revision == confirmation._lease.record.revision &&
        current.scope.key == confirmation._lease.record.scope.key;
  }

  Future<ManagedDiscardConfirmation> prepareRecord(
    String id,
    CancellationToken cancellation,
  ) {
    _admit();
    _invalidate();
    return _track(() => _prepare(id, cancellation));
  }

  Future<ManagedDiscardConfirmation> _prepare(
    String id,
    CancellationToken cancellation,
  ) async {
    final lease = repository.leaseCurrent(id);
    if (lease == null ||
        (lease.record.state != ManagedInstallationState.staged &&
            lease.record.state != ManagedInstallationState.recoveryRequired)) {
      throw const AppFailure(FailureKind.recovery);
    }
    final token = session.beginWork();
    _tokens.add(token);
    final unlink = cancellation.onCancel(token.cancel);
    try {
      token.check();
      final receipt = await backend.planDiscard(_request(lease.record), token);
      await backend.drain();
      _checkLease(lease, token);
      final plan = receipt.plan;
      if ((receipt.state != ContentStagingState.checkpoint &&
              receipt.state != ContentStagingState.staged) ||
          plan == null ||
          plan.isEmpty ||
          plan.length > contentDiscardMaximumPlanBytes ||
          receipt.verifiedBytes < lease.record.resumeFloorBytes) {
        throw const AppFailure(FailureKind.recovery);
      }
      validateManagedCheckpoint(
        lease.record.intent,
        receipt.verifiedBytes,
        receipt.verifiedChunks,
      );
      if (receipt.state == ContentStagingState.staged &&
          receipt.verifiedBytes != lease.record.intent.expectedSize) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final confirmation = ManagedDiscardConfirmation._(
        lease,
        plan,
        receipt.verifiedBytes,
        receipt.verifiedChunks,
        false,
        0,
        0,
      );
      _confirmation = confirmation;
      return confirmation;
    } finally {
      await _finish(token, unlink);
    }
  }

  /// Reconciles a durable cleanup plan without deleting anything. Returns a new
  /// confirmation only for native ready; corrupt/conflicting plans stay blocked.
  Future<ManagedDiscardConfirmation?> inspectRecord(
    String id,
    CancellationToken cancellation,
  ) {
    _admit();
    _invalidate();
    return _track(() => _inspect(id, cancellation, grantConfirmation: true));
  }

  Future<ManagedDiscardConfirmation?> _inspect(
    String id,
    CancellationToken cancellation, {
    required bool grantConfirmation,
  }) async {
    var lease = repository.leaseCurrent(id);
    if (lease == null ||
        lease.record.state !=
            ManagedInstallationState.discardRecoveryRequired) {
      throw const AppFailure(FailureKind.recovery);
    }
    final token = session.beginWork();
    _tokens.add(token);
    final unlink = cancellation.onCancel(token.cancel);
    try {
      token.check();
      final receipt = await backend.inspectDiscard(
        lease.record.discard!.plan,
        token,
      );
      await backend.drain();
      _checkLease(lease, token, allowSuspended: !grantConfirmation);
      lease = await repository.reconcileDiscard(
        lease,
        receipt,
        token,
        observedJobs: jobs,
      );
      if (grantConfirmation && receipt.state == ContentDiscardState.ready) {
        final discard = lease.record.discard!;
        final confirmation = ManagedDiscardConfirmation._(
          lease,
          discard.plan,
          discard.verifiedBytes,
          discard.verifiedChunks,
          true,
          discard.removedBytes,
          discard.removedFiles,
        );
        _confirmation = confirmation;
        return confirmation;
      }
      return null;
    } finally {
      await _finish(token, unlink);
    }
  }

  Future<void> resumeDiscardRecord(
    ManagedDiscardConfirmation confirmation,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
  }) {
    if (!confirmation.recovery) throw const AppFailure(FailureKind.recovery);
    return discardRecord(confirmation, cancellation, coordinator: coordinator);
  }

  Future<void> discardRecord(
    ManagedDiscardConfirmation confirmation,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
  }) {
    _admit();
    if (!isCurrent(confirmation)) throw const AppFailure(FailureKind.conflict);
    _confirmation = null; // Consume synchronously, before any await/admission.
    final admission = coordinator.acquireIdle();
    return _track(() => _discard(confirmation, cancellation, admission));
  }

  Future<void> _discard(
    ManagedDiscardConfirmation confirmation,
    CancellationToken cancellation,
    JobAdmissionLease admission,
  ) async {
    final token = session.beginWork();
    _tokens.add(token);
    final unlink = cancellation.onCancel(token.cancel);
    ManagedDiscardAttempt? attempt;
    void check() {
      _checkLease(confirmation._lease, token);
      admission.check();
    }

    try {
      check();
      final receipt = await backend.inspectDiscard(confirmation._plan, token);
      await backend.drain();
      check();
      if (receipt.state != ContentDiscardState.ready ||
          receipt.removedBytes != confirmation.removedBytes ||
          receipt.removedFiles != confirmation.removedFiles) {
        throw const AppFailure(FailureKind.recovery);
      }
      attempt = await repository.prepareDiscard(
        confirmation._lease,
        confirmation._plan,
        confirmation.verifiedBytes,
        confirmation.verifiedChunks,
        token,
        jobs: jobs,
        checkAdmission: () {
          token.check();
          _checkOpen();
          admission.check();
        },
      );
      token.check();
      _checkOpen();
      admission.check();
      _selectedJobKey = attempt.job.key;
      await admission.run(attempt.lease.record.scope, attempt.job, token);
      token.check();
      final current = repository.findCurrent(confirmation.operationId);
      if (current == null ||
          current.discardAttemptId != attempt.job.intentId ||
          current.state != ManagedInstallationState.discarded) {
        throw const AppFailure(FailureKind.recovery);
      }
    } catch (error) {
      if (error is! AppFailure || error.kind == FailureKind.storage) {
        _failSettlement(admission);
      }
      if (attempt != null) {
        try {
          await repository.abortDiscard(attempt, jobs: jobs);
        } on AppFailure catch (failure) {
          if (failure.kind != FailureKind.conflict) {
            _failSettlement(admission);
            rethrow;
          }
          try {
            await _requireRecovery(attempt.lease);
          } catch (_) {
            _failSettlement(admission);
            rethrow;
          }
        } catch (_) {
          _failSettlement(admission);
          rethrow;
        }
      }
      rethrow;
    } finally {
      _selectedJobKey = null;
      try {
        await _finish(token, unlink);
      } catch (_) {
        _failSettlement(admission);
        rethrow;
      } finally {
        admission.release();
      }
    }
  }

  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken cancellation,
    Future<void> Function(int) saveCheckpoint,
  ) async {
    _checkOpen();
    var lease = _lease(job);
    if (_selectedJobKey != job.key) {
      await _requireRecovery(lease);
      throw const AppFailure(FailureKind.recovery);
    }
    try {
      lease = await repository.beginDiscard(lease, job.intentId!, cancellation);
      final receipt = await backend.discard(
        lease.record.discard!.plan,
        cancellation,
      );
      await backend.drain();
      cancellation.check();
      if (receipt.state == ContentDiscardState.ready) {
        throw const AppFailure(FailureKind.invalidData);
      }
      lease = await repository.reconcileDiscard(lease, receipt, cancellation);
      final bytes = lease.record.discard!.removedBytes;
      await saveCheckpoint(bytes > job.checkpoint ? bytes : job.checkpoint);
      return lease.record.state == ManagedInstallationState.discarded
          ? JobOutcome.completed
          : JobOutcome.unknown;
    } catch (_) {
      if (lease.record.state != ManagedInstallationState.discarded) {
        await _requireRecovery(lease);
      }
      rethrow;
    } finally {
      await backend.drain();
    }
  }

  @override
  Future<JobOutcome> reconcile(
    JobRecord job,
    CancellationToken cancellation,
  ) async {
    _checkOpen();
    var lease = _lease(job);
    if (lease.record.state == ManagedInstallationState.discarded) {
      return JobOutcome.completed;
    }
    lease = await repository.requireDiscardRecovery(lease);
    try {
      final receipt = await backend.inspectDiscard(
        lease.record.discard!.plan,
        cancellation,
      );
      await backend.drain();
      cancellation.check();
      lease = await repository.reconcileDiscard(lease, receipt, cancellation);
      return lease.record.state == ManagedInstallationState.discarded
          ? JobOutcome.completed
          : JobOutcome.unknown;
    } finally {
      await backend.drain();
    }
  }

  ManagedInstallationLease _lease(JobRecord job) {
    final lease = repository.leaseCurrent(job.item);
    if (job.kind != jobKind ||
        lease == null ||
        !isManagedDiscardState(lease.record.state) ||
        lease.record.discardAttemptId != job.intentId) {
      throw const AppFailure(FailureKind.conflict);
    }
    return lease;
  }

  Future<void> _requireRecovery(ManagedInstallationLease lease) async {
    try {
      await repository.requireDiscardRecovery(lease);
    } on AppFailure catch (failure) {
      if (failure.kind != FailureKind.conflict) rethrow;
    }
  }

  void _checkLease(
    ManagedInstallationLease lease,
    CancellationToken token, {
    bool allowSuspended = false,
  }) {
    token.check();
    if (!allowSuspended) _checkOpen();
    if (_closed ||
        session.generation != lease.sessionGeneration ||
        session.scope.key != lease.record.scope.key) {
      throw const AppFailure(FailureKind.cancelled);
    }
    final current = repository.findCurrent(lease.record.operationId);
    if (current == null || current.revision != lease.record.revision) {
      throw const AppFailure(FailureKind.conflict);
    }
  }

  void _checkOpen() {
    if (_closed || _suspended) throw const AppFailure(FailureKind.cancelled);
    if (!backend.discardAvailable) {
      throw const AppFailure(FailureKind.unsupported);
    }
    if (!repository.store.durable) throw const AppFailure(FailureKind.storage);
  }

  void _admit() {
    _checkOpen();
    if (_active != null || _recovering != null) {
      throw const AppFailure(FailureKind.conflict);
    }
  }

  Future<T> _track<T>(Future<T> Function() action) {
    final result = action();
    final settled = result.then<void>((_) {}, onError: (Object _) {});
    _active = settled;
    return result.whenComplete(() {
      if (identical(_active, settled)) _active = null;
    });
  }

  Future<void> _finish(CancellationToken token, void Function() unlink) async {
    try {
      await backend.drain();
    } catch (_) {
      _invalidate();
      _suspended = true;
      rethrow;
    } finally {
      if (token.isCancelled) _invalidate();
      unlink();
      _tokens.remove(token);
      session.finishWork(token);
    }
  }

  void _invalidate() => _confirmation = null;
  void _failSettlement(JobAdmissionLease admission) {
    interrupt();
    admission.suspendAfterSettlementFailure();
  }

  void interrupt() {
    _suspended = true;
    cancelPending();
  }

  void cancelPending() {
    _invalidate();
    for (final token in _tokens.toList()) {
      token.cancel();
    }
    unawaited(backend.cancel().catchError((Object _) {}));
  }

  Future<void> recoverInterrupted() => _recovering ??= _recoverInterrupted()
      .whenComplete(() => _recovering = null);
  Future<void> _recoverInterrupted() async {
    _invalidate();
    await _active;
    if (_closed) throw const AppFailure(FailureKind.cancelled);
    await backend.drain();
    await repository.recoverInterrupted();
    final token = session.beginWork();
    _tokens.add(token);
    try {
      final records = repository
          .readCurrent()
          .where(
            (record) =>
                record.state ==
                    ManagedInstallationState.discardRecoveryRequired ||
                record.state == ManagedInstallationState.discarded,
          )
          .toList();
      for (final record in records) {
        token.check();
        if (record.state == ManagedInstallationState.discarded) {
          final lease = repository.leaseCurrent(record.operationId)!;
          await repository.settleDiscarded(lease, token, jobs: jobs);
        } else {
          await _inspect(record.operationId, token, grantConfirmation: false);
        }
      }
    } finally {
      _tokens.remove(token);
      session.finishWork(token);
    }
  }

  void resume() {
    if (!_closed) _suspended = false;
  }

  Future<void> close() => _closing ??= _close();
  Future<void> _close() async {
    _closed = true;
    repository.removeListener(_invalidate);
    session.removeListener(_invalidate);
    cancelPending();
    await _active;
    await _recovering;
    // The staging owner closes the shared backend after both services settle.
    await backend.drain();
  }

  static ContentStagingRequest _request(ManagedInstallationRecord record) =>
      ContentStagingRequest(
        slot: record.intent.slot,
        operationId: record.operationId,
        manifestSha256: record.intent.manifestSha256,
        sourceRelative: record.intent.sourceRelative,
        size: record.intent.expectedSize,
        sha256: record.intent.expectedSha256,
        rootIdentity: record.rootIdentity,
        sourceIdentity: record.sourceIdentity,
        minimumVerifiedBytes: record.resumeFloorBytes,
      );
}
