part of 'managed_installation_repository.dart';

// Mutations retain the repository's transaction and post-commit recovery order.
extension _ManagedInstallationMutations on ManagedInstallationRepository {
  Future<ManagedInstallationLease> _update(
    ManagedInstallationLease lease,
    CancellationToken? cancellation,
    (ManagedInstallationState, int, int) Function(ManagedInstallationRecord)
    change, {
    bool safetyOnly = false,
    ManagedStageObservation? verifiedObservation,
    ContentDiscardReceipt? discardObservation,
    JobRepository? observedDiscardJobs,
    void Function()? checkAuthority,
  }) async {
    if (observedDiscardJobs != null &&
        !identical(observedDiscardJobs.store, store)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    void check() {
      _checkOpen();
      checkAuthority?.call();
      if (lease.record.deviceId != deviceId) {
        throw const AppFailure(FailureKind.conflict);
      }
      if (!safetyOnly) {
        _checkScope(lease.record.scope, lease.sessionGeneration, cancellation!);
      }
    }

    check();
    late ManagedInstallationRecord result;
    var settledJob = false;
    await store.transact((draft) {
      check();
      if (!safetyOnly && !supportsManagedTransfers(draft['managedTransfers'])) {
        throw const AppFailure(FailureKind.unsupported);
      }
      final records = _records(draft, create: false);
      final value = records[lease.record.operationId];
      if (value == null) throw const AppFailure(FailureKind.conflict);
      final current = _decode(value as Map, draft);
      _checkLease(lease, current);
      final (state, bytes, chunks) = change(current);
      final acceptedProgress =
          verifiedObservation != null &&
          verifiedObservation.rootIdentity == current.rootIdentity &&
          verifiedObservation.sourceIdentity == current.sourceIdentity;
      final next = _next(
        value,
        current,
        state,
        bytes,
        chunks,
        resumeFloorBytes: acceptedProgress ? bytes : null,
      );
      _applyDiscardObservation(next, value, discardObservation);
      if (state == ManagedInstallationState.discarded &&
          observedDiscardJobs != null) {
        settledJob = observedDiscardJobs.completeObservedIn(
          draft,
          current.scope,
          kind: 'managedContentDiscard',
          item: current.operationId,
          intentId: current.discardAttemptId!,
          checkpoint: discardObservation!.removedBytes,
        );
      }
      records[current.operationId] = next;
      validateManagedInstallations(draft['managedInstallations']);
      result = _decode(next, draft);
    });
    _changed();
    if (settledJob) observedDiscardJobs!.retryChanged();
    final updated = ManagedInstallationLease._(result, lease.sessionGeneration);
    try {
      check();
    } catch (_) {
      if (!safetyOnly) await _recoverCommitted(updated);
      rethrow;
    }
    return updated;
  }

  void _applyDiscardObservation(
    Map<String, Object?> next,
    Map value,
    ContentDiscardReceipt? discardObservation,
  ) {
    if (discardObservation != null) {
      next['discard'] = {
        ...Map<String, Object?>.from(value['discard'] as Map),
        'removedBytes': discardObservation.removedBytes,
        'removedFiles': discardObservation.removedFiles,
      };
    }
  }
}
