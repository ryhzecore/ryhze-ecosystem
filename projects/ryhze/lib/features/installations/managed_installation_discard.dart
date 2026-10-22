part of 'managed_installation_repository.dart';

// Discard transitions use the owning repository's store and notifier.
extension _ManagedInstallationDiscard on ManagedInstallationRepository {
  /// Native planning is read-only; this commits the explicit deletion intent
  /// and exact queue job atomically before any removal can be requested.
  Future<ManagedDiscardAttempt> prepareDiscard(
    ManagedInstallationLease lease,
    Uint8List plan,
    int verifiedBytes,
    int verifiedChunks,
    CancellationToken cancellation, {
    required JobRepository jobs,
    required void Function() checkAdmission,
  }) async {
    if (!identical(jobs.store, store) ||
        plan.isEmpty ||
        plan.length > contentDiscardMaximumPlanBytes) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final encodedPlan = base64Encode(plan);
    void check() {
      _checkScope(lease.record.scope, lease.sessionGeneration, cancellation);
      checkAdmission();
    }

    check();
    late ManagedInstallationRecord result;
    late JobRecord job;
    await store.transact((draft) {
      check();
      final records = _records(draft, create: false);
      final value = records[lease.record.operationId] as Map?;
      if (value == null) throw const AppFailure(FailureKind.conflict);
      final current = _decode(value, draft);
      _checkLease(lease, current);
      final recovery =
          current.state == ManagedInstallationState.discardRecoveryRequired;
      final next = _discardPreparedValue(
        value,
        current,
        recovery,
        encodedPlan,
        verifiedBytes,
        verifiedChunks,
      );
      result = _decode(next, draft);
      job = jobs.admitRetryIn(
        draft,
        current.scope,
        kind: 'managedContentDiscard',
        item: current.operationId,
        previousKind: recovery
            ? 'managedContentDiscard'
            : 'managedContentStage',
        previousIntentId: recovery
            ? current.discardAttemptId!
            : current.attemptId,
        intentId: result.discardAttemptId!,
        checkpoint: result.discard!.removedBytes,
      );
      records[current.operationId] = next;
      validateManagedInstallations(draft['managedInstallations']);
    });
    final attempt = ManagedDiscardAttempt._(
      ManagedInstallationLease._(result, lease.sessionGeneration),
      job,
    );
    _changed();
    jobs.retryChanged();
    try {
      check();
    } catch (_) {
      try {
        await this.abortDiscard(attempt, jobs: jobs);
      } on AppFailure catch (failure) {
        if (failure.kind != FailureKind.conflict) rethrow;
      }
      rethrow;
    }
    return attempt;
  }

  Map<String, Object?> _discardPreparedValue(
    Map value,
    ManagedInstallationRecord current,
    bool recovery,
    String encodedPlan,
    int verifiedBytes,
    int verifiedChunks,
  ) {
    if (!recovery &&
        current.state != ManagedInstallationState.staged &&
        current.state != ManagedInstallationState.recoveryRequired) {
      throw const AppFailure(FailureKind.conflict);
    }
    validateManagedCheckpoint(current.intent, verifiedBytes, verifiedChunks);
    if (verifiedBytes < current.resumeFloorBytes) {
      throw const AppFailure(FailureKind.recovery);
    }
    final previous = current.discard;
    if (recovery &&
        (base64Encode(previous!.plan) != encodedPlan ||
            previous.verifiedBytes != verifiedBytes ||
            previous.verifiedChunks != verifiedChunks ||
            previous.attempt >= 999)) {
      throw const AppFailure(FailureKind.conflict);
    }
    final next = _next(
      value,
      current,
      ManagedInstallationState.discardPrepared,
      current.verifiedBytes,
      current.verifiedChunks,
    );
    next['discard'] = {
      if (recovery) ...Map<String, Object?>.from(value['discard'] as Map),
      'version': 1,
      'plan': encodedPlan,
      'attempt': (previous?.attempt ?? 0) + 1,
      'verifiedBytes': verifiedBytes,
      'verifiedChunks': verifiedChunks,
      'removedBytes': previous?.removedBytes ?? 0,
      'removedFiles': previous?.removedFiles ?? 0,
    };
    return next;
  }

  Future<void> abortDiscard(
    ManagedDiscardAttempt attempt, {
    required JobRepository jobs,
  }) async {
    if (!identical(jobs.store, store)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    await store.transact((draft) {
      final records = _records(draft, create: false);
      final value = records[attempt.lease.record.operationId] as Map?;
      if (value == null) throw const AppFailure(FailureKind.conflict);
      final current = _decode(value, draft);
      _checkLease(attempt.lease, current);
      if (current.state != ManagedInstallationState.discardPrepared ||
          current.discardAttemptId != attempt.job.intentId) {
        throw const AppFailure(FailureKind.conflict);
      }
      jobs.cancelQueuedRetryIn(draft, current.scope, attempt.job);
      records[current.operationId] = _next(
        value,
        current,
        ManagedInstallationState.discardRecoveryRequired,
        current.verifiedBytes,
        current.verifiedChunks,
      );
      validateManagedInstallations(draft['managedInstallations']);
    });
    _changed();
    jobs.retryChanged();
  }

  Future<ManagedInstallationLease> beginDiscard(
    ManagedInstallationLease lease,
    String attemptId,
    CancellationToken token,
  ) => _update(lease, token, (current) {
    if (current.state != ManagedInstallationState.discardPrepared ||
        current.discardAttemptId != attemptId) {
      throw const AppFailure(FailureKind.conflict);
    }
    return (
      ManagedInstallationState.discarding,
      current.verifiedBytes,
      current.verifiedChunks,
    );
  });

  Future<ManagedInstallationLease> requireDiscardRecovery(
    ManagedInstallationLease lease,
  ) => _update(lease, null, (current) {
    if (!isManagedDiscardState(current.state) ||
        current.state == ManagedInstallationState.discarded) {
      throw const AppFailure(FailureKind.conflict);
    }
    return (
      ManagedInstallationState.discardRecoveryRequired,
      current.verifiedBytes,
      current.verifiedChunks,
    );
  }, safetyOnly: true);

  Future<ManagedInstallationLease> reconcileDiscard(
    ManagedInstallationLease lease,
    ContentDiscardReceipt receipt,
    CancellationToken token, {
    JobRepository? observedJobs,
  }) => _update(
    lease,
    token,
    (current) {
      if (current.state != ManagedInstallationState.discarding &&
          current.state != ManagedInstallationState.discardRecoveryRequired) {
        throw const AppFailure(FailureKind.conflict);
      }
      final discard = current.discard!;
      if (receipt.removedBytes < 0 ||
          receipt.removedBytes > discard.verifiedBytes ||
          receipt.removedFiles < 0 ||
          receipt.removedFiles > discard.fileCount ||
          (receipt.state == ContentDiscardState.completed &&
              (receipt.removedBytes != discard.verifiedBytes ||
                  receipt.removedFiles != discard.fileCount))) {
        throw const AppFailure(FailureKind.invalidData);
      }
      return (
        receipt.state == ContentDiscardState.completed
            ? ManagedInstallationState.discarded
            : ManagedInstallationState.discardRecoveryRequired,
        current.verifiedBytes,
        current.verifiedChunks,
      );
    },
    discardObservation: receipt,
    observedDiscardJobs: observedJobs,
  );

  /// Crash recovery after inventory committed but the queue terminal write did
  /// not. Already observed completion is durable; no filesystem effects replay.
  Future<void> settleDiscarded(
    ManagedInstallationLease lease,
    CancellationToken token, {
    required JobRepository jobs,
  }) async {
    if (!identical(jobs.store, store)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    _checkScope(lease.record.scope, lease.sessionGeneration, token);
    if (lease.record.state != ManagedInstallationState.discarded) {
      throw const AppFailure(FailureKind.conflict);
    }
    if (!jobs
        .read(lease.record.scope)
        .any(
          (job) =>
              job.kind == 'managedContentDiscard' &&
              job.item == lease.record.operationId &&
              job.intentId == lease.record.discardAttemptId &&
              job.pending,
        )) {
      return;
    }
    var changed = false;
    await store.transact((draft) {
      _checkScope(lease.record.scope, lease.sessionGeneration, token);
      final records = _records(draft, create: false);
      final value = records[lease.record.operationId] as Map?;
      if (value == null) throw const AppFailure(FailureKind.conflict);
      final current = _decode(value, draft);
      _checkLease(lease, current);
      if (current.state != ManagedInstallationState.discarded) {
        throw const AppFailure(FailureKind.conflict);
      }
      changed = jobs.completeObservedIn(
        draft,
        current.scope,
        kind: 'managedContentDiscard',
        item: current.operationId,
        intentId: current.discardAttemptId!,
        checkpoint: current.discard!.removedBytes,
      );
    });
    if (changed) jobs.retryChanged();
    _checkScope(lease.record.scope, lease.sessionGeneration, token);
  }
}
