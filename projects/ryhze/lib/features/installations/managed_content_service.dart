import 'dart:async';

import '../../core/app_failure.dart';
import '../jobs/job_coordinator.dart';
import '../jobs/job_repository.dart';
import '../session/session_controller.dart';
import 'content_staging_backend.dart';
import 'managed_installation.dart';
import 'managed_installation_repository.dart';
import 'managed_content_admission.dart';
import 'managed_content_manifest.dart';

/// Coordinates local staging through the durable queue. Manifested requests
/// additionally require a composition-trusted authority adapter. No production
/// authority, download origin, archive extraction or launch is supplied here.
/// A provider must establish its separate authorization before offering content.
class ManagedContentService implements JobOperation {
  ManagedContentService(
    this.repository,
    this.backend,
    this.jobs,
    this.session, {
    ManagedContentAuthority authority =
        const UnavailableManagedContentAuthority(),
    DateTime Function()? now,
  }) {
    admissions = ManagedContentAdmissionService(
      session: session,
      deviceId: backend.deviceId,
      source: authority,
      now: now,
    );
    repository.addListener(_invalidateTickets);
    session.addListener(_invalidateTickets);
  }
  static const jobKind = 'managedContentStage';
  final ManagedInstallationRepository repository;
  final ContentStagingBackend backend;
  final JobRepository jobs;
  final SessionController session;
  late final ManagedContentAdmissionService admissions;
  final Set<CancellationToken> _tokens = {};
  Future<JobRecord>? _preparing;
  Future<void>? _inspecting;
  Future<void>? _resuming;
  final Map<String, ManagedInstallationLease> _resumeTickets = {};
  Future<void>? _closing;
  bool _closed = false;
  bool _suspended = false;

  /// Preflight is read-only. Commit intent and queue admission atomically before
  /// any copy; preparation never automatically runs the admitted job.
  /// No UI/provider calls this until an approved content source is configured.
  Future<JobRecord> prepareLocalStage(
    ManagedStageIntent intent,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
  }) => _prepareAdmission(intent, cancellation, coordinator, false);

  /// Disabled with the production default authority. This remains local staging,
  /// with manifest provenance; it does not download, extract, install or launch.
  Future<JobRecord> prepareAuthorizedLocalStage(
    ManagedStageIntent intent,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
    int? transferRevision,
  }) => _prepareAdmission(
    intent,
    cancellation,
    coordinator,
    true,
    transferRevision: transferRevision,
  );

  Future<JobRecord> _prepareAdmission(
    ManagedStageIntent intent,
    CancellationToken cancellation,
    JobCoordinator coordinator,
    bool authorized, {
    int? transferRevision,
  }) {
    _checkAdmission();
    if (_preparing != null || _inspecting != null || _resuming != null) {
      throw const AppFailure(FailureKind.conflict);
    }
    if (!identical(coordinator.repository, jobs) ||
        !identical(coordinator.operations[jobKind], this)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final admission = coordinator.acquireIdle();
    final completion = Completer<JobRecord>();
    _preparing = completion.future;
    unawaited(
      _settle(
        completion,
        () => _prepare(
          intent,
          cancellation,
          admission,
          authorized,
          transferRevision: transferRevision,
        ),
        () => _preparing = null,
      ),
    );
    return completion.future;
  }

  Future<JobRecord> _prepare(
    ManagedStageIntent intent,
    CancellationToken cancellation,
    JobAdmissionLease admission,
    bool authorized, {
    int? transferRevision,
  }) async {
    final token = session.beginWork();
    _tokens.add(token);
    final remove = cancellation.onCancel(token.cancel);
    final scope = session.scope;
    final generation = session.generation;
    ManagedStageAttempt? attempt;
    ManagedContentAdmissionGuard? authorityGuard;
    void check() {
      authorityGuard?.check();
      token.check();
      _checkAdmission();
      admission.check();
      if (session.generation != generation || session.scope.key != scope.key) {
        throw const AppFailure(FailureKind.cancelled);
      }
    }

    try {
      check();
      if (authorized) authorityGuard = await _authorize(intent, token);
      check();
      final workToken = authorityGuard?.cancellation ?? token;
      late ContentStagingReceipt observation;
      try {
        observation = await backend.inspect(_request(intent), workToken);
      } finally {
        // Keep the queue held until the actual read-only native worker settles.
        try {
          await backend.drain();
        } catch (_) {
          _suspendAfterFailure(admission);
          rethrow;
        }
      }
      check();
      attempt = await repository.prepareStage(
        intent,
        _observation(intent, observation),
        workToken,
        jobs: jobs,
        checkAdmission: check,
        manifest: authorityGuard?.provenance,
        transferRevision: transferRevision,
      );
      check();
      return attempt.job;
    } catch (error) {
      if (error is! AppFailure || error.kind == FailureKind.storage) {
        // A failed flush may have committed. Recovery, not another drain, owns it.
        _suspendAfterFailure(admission);
      }
      if (attempt != null) {
        try {
          await repository.abortStage(attempt, jobs: jobs);
        } catch (_) {
          _suspendAfterFailure(admission);
          rethrow;
        }
      }
      rethrow;
    } finally {
      try {
        authorityGuard?.close();
      } finally {
        admission.release();
        remove();
        _tokens.remove(token);
        session.finishWork(token);
      }
    }
  }

  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken cancellation,
    Future<void> Function(int) saveCheckpoint,
  ) async {
    _checkAdmission();
    var lease = _lease(job);
    ManagedContentAdmissionGuard? authorityGuard;
    try {
      if (lease.record.manifest != null) {
        authorityGuard = await _authorize(
          lease.record.intent,
          cancellation,
          expected: lease.record.manifest,
        );
      }
      _checkAdmission();
      authorityGuard?.check();
      final workToken = authorityGuard?.cancellation ?? cancellation;
      // This write must succeed before invoking native effects.
      lease = await repository.beginStage(
        lease,
        workToken,
        attemptId: job.intentId,
        checkAuthority: authorityGuard?.check,
      );
      _checkAdmission();
      authorityGuard?.check();
      workToken.check();
      final receipt = await backend.stage(
        _request(
          lease.record.intent,
          record: lease.record,
          minimumVerifiedBytes: lease.record.resumeFloorBytes,
        ),
        workToken,
      );
      await _drainOperation();
      workToken.check();
      authorityGuard?.check();
      lease = await repository.reconcile(
        lease,
        _observation(lease.record.intent, receipt),
        workToken,
      );
      // Inventory is durable before the queue can report completion. A crash
      // between these writes leaves a recoverable job, never false inventory.
      await saveCheckpoint(lease.record.verifiedBytes);
      workToken.check();
      authorityGuard?.check();
      return lease.record.state == ManagedInstallationState.staged
          ? JobOutcome.completed
          : JobOutcome.unknown;
    } catch (_) {
      if (lease.record.state != ManagedInstallationState.staged) {
        await _requireRecovery(lease);
      }
      rethrow;
    } finally {
      try {
        await _drainOperation();
      } finally {
        authorityGuard?.close();
      }
    }
  }

  @override
  Future<JobOutcome> reconcile(
    JobRecord job,
    CancellationToken cancellation,
  ) async {
    _checkAdmission();
    var lease = _lease(job);
    // A queued orphan may still be prepared. Reconciliation only downgrades it
    // before inspecting; it never replays a copy, even after source reconnect.
    if (lease.record.state == ManagedInstallationState.prepared ||
        lease.record.state == ManagedInstallationState.resumePrepared ||
        lease.record.state == ManagedInstallationState.staging) {
      lease = await repository.requireRecovery(lease);
    }
    try {
      final receipt = await backend.inspect(
        _request(lease.record.intent, record: lease.record),
        cancellation,
      );
      await _drainOperation();
      cancellation.check();
      lease = await repository.reconcile(
        lease,
        _observation(lease.record.intent, receipt),
        cancellation,
      );
      return lease.record.state == ManagedInstallationState.staged
          ? JobOutcome.completed
          : JobOutcome.unknown;
    } finally {
      await _drainOperation();
    }
  }

  ManagedInstallationLease _lease(JobRecord job) {
    if (job.kind != jobKind) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final lease = repository.leaseCurrent(job.item);
    if (lease == null) throw const AppFailure(FailureKind.recovery);
    if (isManagedDiscardState(lease.record.state)) {
      throw const AppFailure(FailureKind.conflict);
    }
    if (job.intentId != lease.record.attemptId) {
      throw const AppFailure(FailureKind.conflict);
    }
    return lease;
  }

  /// User-requested reinspection of retained bytes never resumes a copy. The
  /// existing job remains historical; inventory reflects the newest inspection.
  Future<void> inspectRecord(
    String operationId,
    CancellationToken cancellation,
  ) {
    _checkAdmission();
    if (_preparing != null || _inspecting != null || _resuming != null) {
      throw const AppFailure(FailureKind.conflict);
    }
    return _inspecting = _inspectRecord(
      operationId,
      cancellation,
    ).whenComplete(() => _inspecting = null);
  }

  Future<void> _inspectRecord(
    String operationId,
    CancellationToken cancellation,
  ) async {
    var lease = repository.leaseCurrent(operationId);
    _resumeTickets.remove(operationId);
    if (lease == null ||
        (lease.record.state != ManagedInstallationState.staged &&
            lease.record.state != ManagedInstallationState.recoveryRequired)) {
      throw const AppFailure(FailureKind.recovery);
    }
    final token = session.beginWork();
    _tokens.add(token);
    final remove = cancellation.onCancel(token.cancel);
    try {
      final receipt = await backend.inspect(
        _request(lease.record.intent, record: lease.record),
        token,
      );
      await backend.drain();
      token.check();
      final updated = await repository.reconcile(
        lease,
        _observation(lease.record.intent, receipt),
        token,
      );
      if (_resumable(lease.record, receipt) && !token.isCancelled) {
        _resumeTickets[operationId] = updated;
      }
    } finally {
      try {
        await backend.drain();
      } catch (_) {
        _resumeTickets.remove(operationId);
        rethrow;
      } finally {
        if (token.isCancelled) _resumeTickets.remove(operationId);
        remove();
        _tokens.remove(token);
        session.finishWork(token);
      }
    }
  }

  /// Eligibility exists only after an explicit inspection in this live scope.
  /// Any repository notification, cancellation or lifecycle boundary removes it.
  bool canResume(String operationId) {
    final ticket = _resumeTickets[operationId];
    return !_closed &&
        !_suspended &&
        _preparing == null &&
        _inspecting == null &&
        _resuming == null &&
        ticket != null &&
        ticket.sessionGeneration == session.generation &&
        ticket.record.scope.key == session.scope.key &&
        ticket.record.state == ManagedInstallationState.recoveryRequired;
  }

  /// User-explicit retry of one retained operation. This waits for the selected
  /// queue attempt and native settlement, and never drains unrelated jobs.
  Future<void> resumeRecord(
    String operationId,
    CancellationToken cancellation, {
    required JobCoordinator coordinator,
  }) {
    _checkAdmission();
    if (_preparing != null || _inspecting != null || _resuming != null) {
      throw const AppFailure(FailureKind.conflict);
    }
    if (!canResume(operationId)) throw const AppFailure(FailureKind.recovery);
    final ticket = _resumeTickets.remove(operationId)!;
    final admission = coordinator.acquireIdle();
    final completion = Completer<void>();
    _resuming = completion.future;
    unawaited(
      _settle(
        completion,
        () => _resumeRecord(ticket, cancellation, admission),
        () => _resuming = null,
      ),
    );
    return completion.future;
  }

  Future<void> _resumeRecord(
    ManagedInstallationLease ticket,
    CancellationToken cancellation,
    JobAdmissionLease admission,
  ) async {
    final token = session.beginWork();
    _tokens.add(token);
    final remove = cancellation.onCancel(token.cancel);
    ManagedResumeAttempt? attempt;
    ManagedContentAdmissionGuard? authorityGuard;
    void check() {
      authorityGuard?.check();
      token.check();
      _checkAdmission();
      admission.check();
      if (session.generation != ticket.sessionGeneration ||
          session.scope.key != ticket.record.scope.key) {
        throw const AppFailure(FailureKind.cancelled);
      }
    }

    try {
      check();
      if (ticket.record.manifest != null) {
        authorityGuard = await _authorize(
          ticket.record.intent,
          token,
          expected: ticket.record.manifest,
        );
      }
      check();
      final workToken = authorityGuard?.cancellation ?? token;
      final receipt = await backend.inspect(
        _request(ticket.record.intent, record: ticket.record),
        workToken,
      );
      await backend.drain();
      check();
      if (!_resumable(ticket.record, receipt) ||
          receipt.verifiedBytes != ticket.record.verifiedBytes ||
          receipt.verifiedChunks != ticket.record.verifiedChunks) {
        await repository.reconcile(
          ticket,
          _observation(ticket.record.intent, receipt),
          token,
        );
        throw const AppFailure(FailureKind.recovery);
      }
      attempt = await repository.prepareResume(
        ticket,
        _observation(ticket.record.intent, receipt),
        workToken,
        jobs: jobs,
        checkAdmission: check,
      );
      check();
      // Execution obtains a fresh proof for the exact now-persisted attempt.
      authorityGuard?.close();
      authorityGuard = null;
      await admission.run(ticket.record.scope, attempt.job, token);
      token.check();
      final record = repository.findCurrent(ticket.record.operationId);
      if (record == null ||
          record.attemptId != attempt.job.intentId ||
          record.state != ManagedInstallationState.staged) {
        throw const AppFailure(FailureKind.recovery);
      }
    } catch (error) {
      // A failed transaction can have committed before reporting its failure.
      // Recovery must inspect that durable state before ordinary queue work.
      if (error is! AppFailure || error.kind == FailureKind.storage) {
        _suspendAfterFailure(admission);
      }
      if (attempt != null) {
        try {
          await repository.abortResume(attempt, jobs: jobs);
        } on AppFailure catch (failure) {
          if (failure.kind != FailureKind.conflict) {
            _suspendAfterFailure(admission);
            rethrow;
          }
          await _requireRecovery(attempt.lease);
        } catch (_) {
          _suspendAfterFailure(admission);
          rethrow;
        }
      }
      rethrow;
    } finally {
      // Keep admission held and UI busy until the real worker has settled.
      try {
        await backend.drain();
      } catch (_) {
        _suspendAfterFailure(admission);
        rethrow;
      } finally {
        try {
          authorityGuard?.close();
        } finally {
          admission.release();
          remove();
          _tokens.remove(token);
          session.finishWork(token);
        }
      }
    }
  }

  Future<ManagedContentAdmissionGuard> _authorize(
    ManagedStageIntent intent,
    CancellationToken token, {
    ManagedManifestProvenance? expected,
  }) async {
    final ticket = await admissions.admit(intent, token);
    try {
      if (expected != null && !expected.sameArtifact(ticket.provenance)) {
        throw const AppFailure(FailureKind.conflict);
      }
      return ticket.claim(
        intent: intent,
        session: session,
        deviceId: backend.deviceId,
      );
    } catch (_) {
      ticket.close();
      rethrow;
    }
  }

  bool _resumable(
    ManagedInstallationRecord record,
    ContentStagingReceipt receipt,
  ) =>
      receipt.state == ContentStagingState.checkpoint &&
      receipt.rootIdentity == record.rootIdentity &&
      receipt.sourceIdentity == record.sourceIdentity &&
      receipt.verifiedBytes >= record.resumeFloorBytes &&
      receipt.verifiedBytes < record.intent.expectedSize;

  void _invalidateTickets() => _resumeTickets.clear();

  void _checkAdmission() {
    if (_closed || _suspended) throw const AppFailure(FailureKind.cancelled);
    if (!repository.store.durable) throw const AppFailure(FailureKind.storage);
  }

  Future<void> _drainOperation() async {
    try {
      await backend.drain();
    } catch (_, stack) {
      try {
        interrupt();
      } catch (_) {
        // Preserve the failed settlement; cancellation must not reopen admission.
      }
      Error.throwWithStackTrace(const JobSettlementFailure(), stack);
    }
  }

  Future<void> _requireRecovery(ManagedInstallationLease lease) async {
    try {
      await repository.requireRecovery(lease);
    } on AppFailure catch (error) {
      // Another safety downgrade can legitimately invalidate this CAS. Keep
      // the original failure; never promote or replay a stale completion.
      if (error.kind != FailureKind.conflict) rethrow;
    }
  }

  // Called only while propagating an earlier operation/settlement failure.
  // Callback failures must not mask that first cause or leave the queue open.
  void _suspendAfterFailure(JobAdmissionLease admission) {
    try {
      interrupt();
    } catch (_) {
      /* The originating failure remains the caller's result. */
    }
    try {
      admission.suspendAfterSettlementFailure();
    } catch (_) {
      /* The queue marks itself suspended before invoking cancellation. */
    } finally {
      admission.release();
    }
  }

  void interrupt() {
    _suspended = true;
    _cancelPending(interrupt: true);
  }

  void cancelPending() => _cancelPending(interrupt: false);

  void _cancelPending({required bool interrupt}) {
    (Object, StackTrace)? failure;
    void cancel(void Function() action) {
      try {
        action();
      } catch (error, stack) {
        failure ??= (error, stack);
      }
    }

    cancel(interrupt ? admissions.interrupt : admissions.cancelPending);
    _invalidateTickets();
    for (final token in _tokens.toList()) {
      cancel(token.cancel);
    }
    cancel(() {
      unawaited(
        backend.cancel().catchError((Object _) {
          // Only drain establishes settlement; its failures reach the owner.
        }),
      );
    });
    final retained = failure;
    if (retained != null) Error.throwWithStackTrace(retained.$1, retained.$2);
  }

  Future<void> recoverInterrupted() async {
    try {
      await _preparing;
    } catch (_) {
      /* Retain durable recovery state. */
    }
    try {
      await _inspecting;
    } catch (_) {
      /* Retain the previous observation. */
    }
    try {
      await _resuming;
    } catch (_) {
      /* Retain durable attempt history and recovery state. */
    }
    await admissions.drain();
    await backend.drain();
    await repository.recoverInterrupted();
  }

  void resume() {
    if (!_closed) {
      admissions.resume();
      _suspended = false;
    }
  }

  Future<void> close() {
    final existing = _closing;
    if (existing != null) return existing;
    final completion = Completer<void>();
    _closing = completion.future;
    unawaited(_settle(completion, _close, () {}));
    return completion.future;
  }

  Future<void> _close() async {
    _closed = true;
    repository.removeListener(_invalidateTickets);
    session.removeListener(_invalidateTickets);
    (Object, StackTrace)? failure;
    try {
      cancelPending();
    } catch (error, stack) {
      failure = (error, stack);
    }
    try {
      await _preparing;
    } catch (_) {
      /* Intent is preserved for recovery. */
    }
    try {
      await _inspecting;
    } catch (_) {
      /* Preserve the last durable record. */
    }
    try {
      await _resuming;
    } catch (_) {
      /* Cancellation may have followed a durable attempt commit. */
    }
    try {
      await admissions.close();
    } catch (error, stack) {
      failure ??= (error, stack);
    }
    try {
      await backend.close();
    } catch (error, stack) {
      failure ??= (error, stack);
    }
    // All owners have been asked to settle even if cancellation or one close
    // fails. Preserve the first failure without reporting early completion.
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }

  Future<void> _settle<T>(
    Completer<T> completion,
    Future<T> Function() work,
    void Function() release,
  ) async {
    try {
      completion.complete(await work());
    } catch (error, stack) {
      completion.completeError(error, stack);
    } finally {
      release();
    }
  }

  static ContentStagingRequest _request(
    ManagedStageIntent intent, {
    ManagedInstallationRecord? record,
    int minimumVerifiedBytes = 0,
  }) => ContentStagingRequest(
    slot: intent.slot,
    operationId: intent.operationId,
    manifestSha256: intent.manifestSha256,
    sourceRelative: intent.sourceRelative,
    size: intent.expectedSize,
    sha256: intent.expectedSha256,
    rootIdentity: record?.rootIdentity ?? '',
    sourceIdentity: record?.sourceIdentity ?? '',
    minimumVerifiedBytes: minimumVerifiedBytes,
  );

  static ManagedStageObservation _observation(
    ManagedStageIntent intent,
    ContentStagingReceipt receipt,
  ) => ManagedStageObservation(
    operationId: intent.operationId,
    state: ManagedStageObservationState.values.byName(receipt.state.name),
    verifiedBytes: receipt.verifiedBytes,
    verifiedChunks: receipt.verifiedChunks,
    rootIdentity: receipt.rootIdentity,
    sourceIdentity: receipt.sourceIdentity,
  );
}
