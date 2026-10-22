import 'dart:async';

import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../jobs/job_coordinator.dart';
import '../jobs/job_repository.dart';
import '../session/session_controller.dart';
import 'content_transfer_backend.dart';
import 'managed_content_admission.dart';
import 'managed_content_manifest.dart';
import 'managed_content_service.dart';
import 'managed_installation.dart';
import 'managed_transfer_record.dart';
import 'managed_transfer_repository.dart';
import 'managed_transfer_source.dart';

/// A single coordinator owns transfer and incoming-file removal. Byte completion
/// never establishes installation or execution rights. Network destinations and
/// current access are obtained again for every explicit transfer attempt.
class ManagedTransferService extends ChangeNotifier implements JobOperation {
  ManagedTransferService({
    required this.repository,
    required this.backend,
    required this.jobs,
    required this.session,
    this.authority = const UnavailableManagedContentTransferAuthority(),
    DateTime Function()? now,
  }) : admissions = ManagedContentAdmissionService(
         session: session,
         deviceId: backend.deviceId,
         source: authority,
         now: now,
       );

  static const jobKind = 'managedContentTransfer';
  static const discardKind = 'managedContentTransferDiscard';
  final ManagedTransferRepository repository;
  final ContentTransferBackend backend;
  final JobRepository jobs;
  final SessionController session;
  final ManagedContentTransferAuthority authority;
  final ManagedContentAdmissionService admissions;
  Future<void>? _active, _closing;
  final Set<CancellationToken> _tokens = {};
  bool _closed = false, _suspended = false, _disposed = false;
  String? activeOperation;
  String? activeScope;
  int receivedBytes = 0;
  bool get busy => _active != null;
  bool get available => !_closed && !_suspended && authority.available;

  void _check() {
    if (_closed || _suspended) throw const AppFailure(FailureKind.cancelled);
    if (!repository.store.durable) throw const AppFailure(FailureKind.storage);
  }

  Future<T> _owned<T>(
    CancellationToken cancellation,
    Future<T> Function(CancellationToken) work,
  ) {
    _check();
    if (_active != null) throw const AppFailure(FailureKind.conflict);
    final settled = Completer<void>();
    final result = Completer<T>();
    _active = settled.future;
    final token = session.beginWork();
    _tokens.add(token);
    final remove = cancellation.onCancel(token.cancel);
    unawaited(() async {
      try {
        token.check();
        result.complete(await work(token));
      } catch (error, stack) {
        result.completeError(error, stack);
      } finally {
        remove();
        _tokens.remove(token);
        session.finishWork(token);
        activeOperation = null;
        activeScope = null;
        _active = null;
        settled.complete();
        _changed();
      }
    }());
    return result.future;
  }

  Future<_TransferAccess> _authorize(
    ManagedStageIntent intent,
    CancellationToken token, {
    ManagedManifestProvenance? expected,
    String? sourceKey,
  }) async {
    final ticket = await admissions.admit(intent, token);
    ManagedContentAdmissionGuard? guard;
    try {
      guard = ticket.claim(
        intent: intent,
        session: session,
        deviceId: backend.deviceId,
      );
      if (expected != null && !guard.provenance.sameArtifact(expected)) {
        throw const AppFailure(FailureKind.denied);
      }
      final source = await authority.resolveTransfer(
        ticket.request,
        guard.cancellation,
      );
      source.check(ticket.request, guard, expectedSourceKey: sourceKey);
      return _TransferAccess(guard, source, ticket.request);
    } catch (_) {
      ticket.close();
      rethrow;
    }
  }

  Future<JobRecord> prepare(
    ManagedStageIntent intent,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
  }) => _owned(cancellation, (token) async {
    _checkCoordinator(coordinator);
    final reservation = coordinator.acquireIdle();
    _TransferAccess? access;
    try {
      access = await _authorize(intent, token);
      void check() {
        _check();
        reservation.check();
        access!.check();
      }

      check();
      final request = ContentTransferRequest(
        slot: intent.slot,
        operationId: intent.operationId,
        expectedSize: intent.expectedSize,
        expectedSha256: intent.expectedSha256,
      );
      final inspection = await backend.inspect(
        request,
        access.guard.cancellation,
      );
      await _drainAdmission(reservation);
      check();
      if (inspection.state != ContentTransferState.missing ||
          inspection.sourceIdentity.isNotEmpty ||
          inspection.observedBytes != 0) {
        throw const AppFailure(FailureKind.conflict);
      }
      final attempt = await repository.prepareTransfer(
        intent,
        access.guard.provenance,
        access.source.sourceKey,
        token,
        jobs: jobs,
        checkAdmission: check,
        initialObservation: _observed(request, inspection),
      );
      check();
      return attempt.job;
    } catch (error) {
      if (error is JobSettlementFailure ||
          error is! AppFailure ||
          error.kind == FailureKind.storage) {
        reservation.suspendAfterSettlementFailure();
      }
      rethrow;
    } finally {
      try {
        await _drainAdmission(reservation);
      } finally {
        try {
          access?.close();
        } finally {
          reservation.release();
        }
      }
    }
  });

  void _checkCoordinator(JobCoordinator coordinator) {
    if (!identical(coordinator.repository, jobs) ||
        !identical(coordinator.operations[jobKind], this) ||
        !identical(coordinator.operations[discardKind], this)) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  ManagedTransferLease _lease(JobRecord job) {
    final lease = repository.leaseCurrent(job.item);
    if (lease == null ||
        job.intentId !=
            (job.kind == discardKind
                ? lease.record.discardAttemptId
                : lease.record.attemptId) ||
        (job.kind != jobKind && job.kind != discardKind)) {
      throw const AppFailure(FailureKind.conflict);
    }
    return lease;
  }

  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken cancellation,
    Future<void> Function(int) saveCheckpoint,
  ) => _owned(cancellation, (token) async {
    var lease = _lease(job);
    activeOperation = lease.record.operationId;
    activeScope = lease.record.scope.key;
    receivedBytes = lease.record.request.observedBytes;
    _changed();
    if (job.kind == discardKind) {
      return _discard(lease, token, saveCheckpoint);
    }
    _TransferAccess? access;
    try {
      access = await _authorize(
        lease.record.intent,
        token,
        expected: lease.record.manifest,
        sourceKey: lease.record.sourceKey,
      );
      final current = access;
      void check() {
        _check();
        token.check();
        current.check();
      }

      check();
      lease = await repository.begin(
        lease,
        job.intentId!,
        token,
        checkAuthority: check,
      );
      final receipt = await backend.begin(
        lease.record.request,
        current.guard.cancellation,
        resume: lease.record.strongEtag == null ? null : lease.record.previous,
      );
      check();
      var writer = _observed(lease.record.request, receipt);
      lease = await repository.observe(
        lease,
        writer,
        token,
        checkAuthority: check,
      );
      final offset = lease.record.request.observedBytes;
      if (offset < lease.record.intent.expectedSize) {
        await current.source.transport.transfer(
          expectedBytes: lease.record.intent.expectedSize,
          offset: offset,
          strongEtag: offset == 0 ? null : lease.record.strongEtag,
          cancellation: current.guard.cancellation,
          onResponse: (etag) async {
            check();
            lease = await repository.observe(
              lease,
              lease.record.request,
              token,
              checkAuthority: check,
              strongEtag: etag,
              replaceEtag: true,
            );
            check();
          },
          onChunk: (bytes) async {
            check();
            final receipt = await backend.append(
              writer,
              bytes,
              current.guard.cancellation,
            );
            check();
            writer = _observed(writer, receipt);
            receivedBytes = receipt.observedBytes;
            _changed();
          },
        );
      }
      check();
      final complete = await backend.finish(writer, current.guard.cancellation);
      await _drain();
      check();
      if (complete.state != ContentTransferState.complete) {
        throw const AppFailure(FailureKind.recovery);
      }
      lease = await repository.observe(
        lease,
        _observed(lease.record.request, complete),
        token,
        checkAuthority: check,
        complete: true,
      );
      await saveCheckpoint(complete.observedBytes);
      check();
      return JobOutcome.completed;
    } catch (_) {
      await _recover(lease);
      rethrow;
    } finally {
      try {
        await _drain();
      } finally {
        access?.close();
      }
    }
  });

  Future<void> _recover(ManagedTransferLease lease) async {
    try {
      await repository.requireRecovery(lease);
    } on AppFailure catch (error) {
      if (error.kind != FailureKind.conflict) rethrow;
    }
  }

  Future<void> _drain() async {
    try {
      await backend.drain();
    } catch (_, stack) {
      _suspended = true;
      Error.throwWithStackTrace(const JobSettlementFailure(), stack);
    }
  }

  Future<void> _drainAdmission(JobAdmissionLease reservation) async {
    try {
      await _drain();
    } catch (_) {
      // A failed drain leaves actual native settlement unknown. Retain the
      // shared queue suspension even when this idle reservation is released.
      reservation.suspendAfterSettlementFailure();
      rethrow;
    }
  }

  @override
  Future<JobOutcome> reconcile(JobRecord job, CancellationToken cancellation) =>
      _owned(cancellation, (token) async {
        var lease = _lease(job);
        // Restart never repeats network writes, removal or staging admission.
        if (lease.record.state == ManagedTransferState.prepared ||
            lease.record.state == ManagedTransferState.receiving) {
          lease = await repository.requireRecovery(lease);
        }
        try {
          final receipt = await backend.inspect(lease.record.request, token);
          await _drain();
          token.check();
          if (receipt.state == ContentTransferState.missing) {
            return JobOutcome.unknown;
          }
          lease = await repository.observe(
            lease,
            _observed(lease.record.request, receipt),
            token,
            complete: receipt.state == ContentTransferState.complete,
          );
          return receipt.state == ContentTransferState.complete &&
                  job.kind == jobKind
              ? JobOutcome.completed
              : JobOutcome.unknown;
        } finally {
          await _drain();
        }
      });

  Future<void> inspect(
    String operationId,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
  }) => _owned(cancellation, (token) async {
    _checkCoordinator(coordinator);
    final reservation = coordinator.acquireIdle();
    try {
      var lease = repository.leaseCurrent(operationId);
      if (lease == null) throw const AppFailure(FailureKind.invalidData);
      if (lease.record.state == ManagedTransferState.prepared) {
        lease = await repository.requireRecovery(lease);
      }
      final receipt = await backend.inspect(lease.record.request, token);
      await _drainAdmission(reservation);
      token.check();
      reservation.check();
      if (receipt.state != ContentTransferState.missing ||
          lease.record.request.sourceIdentity.isEmpty) {
        await repository.observe(
          lease,
          _observed(lease.record.request, receipt),
          token,
          complete: receipt.state == ContentTransferState.complete,
          jobs: receipt.state == ContentTransferState.complete ? jobs : null,
        );
      }
    } finally {
      try {
        await _drainAdmission(reservation);
      } finally {
        reservation.release();
      }
    }
  });

  Future<JobRecord> prepareRetry(
    String operationId,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
  }) => _owned(cancellation, (token) async {
    _checkCoordinator(coordinator);
    final reservation = coordinator.acquireIdle();
    _TransferAccess? access;
    try {
      var lease = repository.leaseCurrent(operationId);
      if (lease == null) throw const AppFailure(FailureKind.invalidData);
      access = await _authorize(
        lease.record.intent,
        token,
        expected: lease.record.manifest,
        sourceKey: lease.record.sourceKey,
      );
      void check() {
        _check();
        reservation.check();
        access!.check();
      }

      final receipt = await backend.inspect(lease.record.request, token);
      await _drainAdmission(reservation);
      check();
      if (receipt.state != ContentTransferState.checkpoint &&
          receipt.state != ContentTransferState.missing) {
        throw const AppFailure(FailureKind.recovery);
      }
      if (receipt.state == ContentTransferState.missing &&
          lease.record.request.sourceIdentity.isNotEmpty) {
        // Keep the old ownership/removal plan. Explicitly reconcile its
        // absence through reviewed removal before admitting a fresh download.
        throw const AppFailure(FailureKind.recovery);
      }
      final observed = _observed(lease.record.request, receipt);
      if (receipt.state != ContentTransferState.missing) {
        lease = await repository.observe(lease, observed, token);
      }
      final attempt = await repository.prepareRetry(
        lease,
        observed,
        token,
        jobs: jobs,
        checkAdmission: check,
        strongEtag: lease.record.strongEtag,
      );
      return attempt.job;
    } catch (error) {
      if (error is JobSettlementFailure ||
          error is! AppFailure ||
          error.kind == FailureKind.storage) {
        // Admission may have committed before its acknowledgment failed.
        // Recovery owns that queued attempt; a later drain cannot replay it.
        reservation.suspendAfterSettlementFailure();
      }
      rethrow;
    } finally {
      try {
        await _drainAdmission(reservation);
      } finally {
        try {
          access?.close();
        } finally {
          reservation.release();
        }
      }
    }
  });

  Future<JobRecord> prepareDiscard(
    String operationId,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
  }) => _owned(cancellation, (token) async {
    _checkCoordinator(coordinator);
    final reservation = coordinator.acquireIdle();
    try {
      final lease = repository.leaseCurrent(operationId);
      if (lease == null) throw const AppFailure(FailureKind.invalidData);
      // UI requires an explicit inspect/review before this destructive intent.
      final attempt = await repository.prepareDiscard(
        lease,
        token,
        jobs: jobs,
        checkAdmission: reservation.check,
      );
      return attempt.job;
    } catch (error) {
      if (error is JobSettlementFailure ||
          error is! AppFailure ||
          error.kind == FailureKind.storage) {
        // A failed flush cannot establish that removal was never queued.
        reservation.suspendAfterSettlementFailure();
      }
      rethrow;
    } finally {
      reservation.release();
    }
  });

  Future<JobOutcome> _discard(
    ManagedTransferLease initial,
    CancellationToken token,
    Future<void> Function(int) saveCheckpoint,
  ) async {
    var lease = initial;
    try {
      lease = await repository.beginDiscard(
        lease,
        token,
        attemptId: lease.record.discardAttemptId,
      );
      if (lease.record.previous != null) {
        final receipt = await backend.discard(lease.record.previous!, token);
        await _drain();
        token.check();
        if (receipt.state != ContentTransferState.discarded) {
          throw const AppFailure(FailureKind.recovery);
        }
        lease = await repository.clearPrevious(lease, receipt, token);
      }
      final receipt = await backend.discard(lease.record.request, token);
      await _drain();
      token.check();
      if (receipt.state != ContentTransferState.discarded) {
        throw const AppFailure(FailureKind.recovery);
      }
      await repository.reconcileDiscard(lease, receipt, token);
      return JobOutcome.completed;
    } catch (_) {
      await _recover(lease);
      rethrow;
    } finally {
      await _drain();
    }
  }

  /// Explicit handoff into the existing independently authorized staging path.
  Future<JobRecord> prepareStage(
    String operationId,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
    required ManagedContentService staging,
  }) => _owned(cancellation, (token) async {
    final lease = repository.leaseCurrent(operationId);
    if (lease == null ||
        lease.record.state != ManagedTransferState.downloaded ||
        !identical(staging.repository.store, repository.store) ||
        !identical(staging.session, session) ||
        staging.backend.deviceId != backend.deviceId) {
      throw const AppFailure(FailureKind.conflict);
    }
    try {
      return await staging.prepareAuthorizedLocalStage(
        lease.record.stagingIntent,
        token,
        coordinator: coordinator,
        transferRevision: lease.record.revision,
      );
    } finally {
      // The staging transaction owns its inventory, exact queued job and this
      // transfer's handoff marker together, including invalidation cleanup.
      repository.stageAdmissionChanged();
    }
  });

  static ContentTransferRequest _observed(
    ContentTransferRequest request,
    ContentTransferReceipt receipt,
  ) {
    if (receipt.sourceRelative != request.sourceRelative ||
        receipt.observedBytes < 0 ||
        receipt.observedBytes > request.expectedSize ||
        (request.rootIdentity.isNotEmpty &&
            receipt.rootIdentity != request.rootIdentity) ||
        (request.directoryIdentity.isNotEmpty &&
            receipt.directoryIdentity != request.directoryIdentity) ||
        (request.sourceIdentity.isNotEmpty &&
            receipt.sourceIdentity != request.sourceIdentity)) {
      throw const AppFailure(FailureKind.recovery);
    }
    return ContentTransferRequest(
      slot: request.slot,
      operationId: request.operationId,
      expectedSize: request.expectedSize,
      expectedSha256: request.expectedSha256,
      attempt: request.attempt,
      rootIdentity: receipt.rootIdentity,
      directoryIdentity: receipt.directoryIdentity,
      sourceIdentity: receipt.sourceIdentity,
      observedBytes: receipt.observedBytes,
      observedSha256: receipt.observedSha256,
    );
  }

  void _changed() {
    if (!_disposed) notifyListeners();
  }

  void interrupt() {
    _suspended = true;
    _cancel(interrupting: true);
  }

  void cancelPending() => _cancel(interrupting: false);
  void _cancel({required bool interrupting}) {
    (Object, StackTrace)? failure;
    void attempt(void Function() callback) {
      try {
        callback();
      } catch (error, stack) {
        failure ??= (error, stack);
      }
    }

    attempt(interrupting ? admissions.interrupt : admissions.cancelPending);
    for (final token in _tokens.toList()) {
      attempt(token.cancel);
    }
    attempt(() => unawaited(backend.cancel().catchError((Object _) {})));
    if (failure != null) Error.throwWithStackTrace(failure!.$1, failure!.$2);
  }

  Future<void> recoverInterrupted() async {
    await _active;
    await admissions.drain();
    await _drain();
    await repository.recoverInterrupted();
  }

  void resume() {
    if (!_closed) {
      admissions.resume();
      _suspended = false;
    }
  }

  Future<void> close() {
    if (_closing != null) return _closing!;
    final completion = Completer<void>();
    _closing = completion.future;
    _closed = true;
    unawaited(() async {
      (Object, StackTrace)? failure;
      try {
        cancelPending();
      } catch (error, stack) {
        failure = (error, stack);
      }
      for (final work in <Future<void> Function()>[
        () async {
          await _active;
        },
        admissions.close,
        backend.close,
      ]) {
        try {
          await work();
        } catch (error, stack) {
          failure ??= (error, stack);
        }
      }
      if (failure == null) {
        completion.complete();
      } else {
        completion.completeError(failure.$1, failure.$2);
      }
    }());
    return completion.future;
  }

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }
}

class _TransferAccess {
  _TransferAccess(this.guard, this.source, this.request);
  final ManagedContentAdmissionGuard guard;
  final ManagedTransferSource source;
  final ManagedContentAccessRequest request;
  void check() => source.check(request, guard);
  void close() => guard.close();
}
