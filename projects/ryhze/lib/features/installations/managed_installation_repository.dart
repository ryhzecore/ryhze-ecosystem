import 'dart:convert';

import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../../data/transaction_store.dart';
import '../jobs/job_repository.dart';
import '../session/session_controller.dart';
import 'managed_installation.dart';
import 'managed_content_manifest.dart';
import 'content_discard_backend.dart';
import 'managed_transfer_record.dart';

part 'managed_installation_discard.dart';
part 'managed_installation_mutations.dart';

class ManagedInstallationLease {
  ManagedInstallationLease._(this.record, this.sessionGeneration);
  final ManagedInstallationRecord record;
  final int sessionGeneration;
}

class ManagedStageAttempt {
  ManagedStageAttempt._(this.lease, this.job);
  final ManagedInstallationLease lease;
  final JobRecord job;
}

class ManagedResumeAttempt {
  ManagedResumeAttempt._(this.lease, this.job);
  final ManagedInstallationLease lease;
  final JobRecord job;
}

class ManagedDiscardAttempt {
  ManagedDiscardAttempt._(this.lease, this.job);
  final ManagedInstallationLease lease;
  final JobRecord job;
}

/// Sole metadata writer for inert managed staging. Native effects remain owned
/// by the coordinator. A committed record and lease precede every stage call.
class ManagedInstallationRepository extends ChangeNotifier {
  ManagedInstallationRepository(
    this.store,
    this.session, {
    required this.deviceId,
  }) {
    validateManagedDeviceId(deviceId);
  }

  final TransactionStore store;
  final SessionController session;
  final String deviceId;
  bool _disposed = false;

  List<ManagedInstallationRecord> readCurrent() {
    final snapshot = store.read();
    final records = _records(snapshot, create: false);
    return List.unmodifiable(
      records.values
          .map((value) => _decode(value as Map, snapshot))
          .where(
            (record) =>
                record.deviceId == deviceId &&
                record.scope.key == session.scope.key,
          ),
    );
  }

  ManagedInstallationRecord? findCurrent(String operationId) {
    for (final record in readCurrent()) {
      if (record.operationId == operationId) return record;
    }
    return null;
  }

  /// Reconstructs the CAS token for an explicit current-scope reconciliation.
  /// Obtaining a token never resumes work or makes a stored observation fresh.
  ManagedInstallationLease? leaseCurrent(String operationId) {
    final record = findCurrent(operationId);
    return record == null
        ? null
        : ManagedInstallationLease._(record, session.generation);
  }

  Future<ManagedInstallationLease> prepare(
    ManagedStageIntent intent,
    ManagedStageObservation inspection,
    CancellationToken cancellation,
  ) async {
    final scope = session.scope;
    final generation = session.generation;
    validateManagedScope(scope);
    _checkScope(scope, generation, cancellation);
    late ManagedInstallationRecord result;
    await store.transact((draft) {
      _checkScope(scope, generation, cancellation);
      result = _prepareIn(draft, scope, intent, inspection);
    });
    _changed();
    final lease = ManagedInstallationLease._(result, generation);
    try {
      _checkScope(scope, generation, cancellation);
    } catch (_) {
      await _recoverCommitted(lease);
      rethrow;
    }
    return lease;
  }

  /// Initial inventory and its exact queue job share one durable commit. The
  /// caller holds coordinator admission until settlement and any abort finish.
  Future<ManagedStageAttempt> prepareStage(
    ManagedStageIntent intent,
    ManagedStageObservation inspection,
    CancellationToken cancellation, {
    required JobRepository jobs,
    required void Function() checkAdmission,
    ManagedManifestProvenance? manifest,
    int? transferRevision,
  }) async {
    if (!identical(jobs.store, store)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final scope = session.scope;
    final generation = session.generation;
    validateManagedScope(scope);
    void check() {
      _checkScope(scope, generation, cancellation);
      checkAdmission();
    }

    check();
    late ManagedInstallationRecord result;
    late ({JobRecord job, bool inserted}) admission;
    await store.transact((draft) {
      check();
      final existing = _records(
        draft,
        create: false,
      ).containsKey(intent.operationId);
      result = _prepareIn(draft, scope, intent, inspection, manifest: manifest);
      admission = jobs.enqueueIn(
        draft,
        scope,
        'managedContentStage',
        intent.operationId,
        intentId: intent.operationId,
      );
      if (admission.inserted == existing ||
          admission.job.state != JobState.queued ||
          admission.job.checkpoint != 0) {
        throw const AppFailure(FailureKind.conflict);
      }
      admitManagedTransferStageIn(
        draft,
        result,
        expectedRevision: transferRevision,
      );
    });
    final attempt = ManagedStageAttempt._(
      ManagedInstallationLease._(result, generation),
      admission.job,
    );
    _changed();
    if (admission.inserted) jobs.admissionChanged();
    try {
      check();
    } catch (_) {
      try {
        await abortStage(attempt, jobs: jobs);
      } catch (_) {
        // The caller must keep admission suspended when cleanup did not settle.
        throw const AppFailure(FailureKind.storage);
      }
      rethrow;
    }
    return attempt;
  }

  /// Safety cleanup may target the original account after session invalidation,
  /// but only its exact unstarted operation and matching inventory revision.
  Future<void> abortStage(
    ManagedStageAttempt attempt, {
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
      if (current.state != ManagedInstallationState.prepared ||
          current.attempt != 0 ||
          attempt.job.intentId != current.attemptId) {
        throw const AppFailure(FailureKind.conflict);
      }
      jobs.cancelQueuedRetryIn(draft, current.scope, attempt.job);
      records[current.operationId] = _next(
        value,
        current,
        ManagedInstallationState.recoveryRequired,
        current.verifiedBytes,
        current.verifiedChunks,
      );
      validateManagedInstallations(draft['managedInstallations']);
    });
    _changed();
    jobs.admissionChanged();
  }

  ManagedInstallationRecord _prepareIn(
    StoreData draft,
    AccountScope scope,
    ManagedStageIntent intent,
    ManagedStageObservation inspection, {
    ManagedManifestProvenance? manifest,
  }) {
    if (!supportsManagedTransfers(draft['managedTransfers'])) {
      throw const AppFailure(FailureKind.unsupported);
    }
    if (inspection.operationId != intent.operationId ||
        inspection.state != ManagedStageObservationState.checkpoint ||
        !inspection.hasIdentities ||
        inspection.verifiedBytes != 0 ||
        inspection.verifiedChunks != 0) {
      throw const AppFailure(FailureKind.recovery);
    }
    final records = _records(
      draft,
      create: true,
      promoteManifest: manifest != null,
    );
    final existing = records[intent.operationId];
    if (existing != null) {
      final record = _decode(existing as Map, draft);
      if (record.deviceId != deviceId ||
          record.scope.key != scope.key ||
          !record.intent.sameRequest(intent) ||
          (record.manifest == null) != (manifest == null) ||
          (manifest != null && !record.manifest!.sameArtifact(manifest)) ||
          record.rootIdentity != inspection.rootIdentity ||
          record.sourceIdentity != inspection.sourceIdentity ||
          record.state != ManagedInstallationState.prepared) {
        throw const AppFailure(FailureKind.conflict);
      }
      return record;
    }
    if (records.length >= managedInstallationMaximumRecords) {
      throw const AppFailure(FailureKind.conflict);
    }
    final value = <String, Object?>{
      if (manifest != null) 'recordVersion': 2,
      if (manifest != null) 'managedManifest': manifest.toJson(),
      'account': {'authority': scope.authority, 'accountId': scope.accountId},
      'deviceId': deviceId,
      'owningInstaller': 'ryhze',
      'intent': intent.toJson(),
      'rootIdentity': inspection.rootIdentity,
      'sourceIdentity': inspection.sourceIdentity,
      'state': ManagedInstallationState.prepared.name,
      'revision': 1,
      'verifiedBytes': 0,
      'verifiedChunks': 0,
      'updatedAt': DateTime.now().toUtc().toIso8601String(),
    };
    final result = _decode(value, draft);
    for (final oldValue in records.values) {
      final old = _decode(oldValue as Map, draft);
      if (old.reservesContent &&
          (managedRootReservation(old) == managedRootReservation(result) ||
              managedItemReservation(old) == managedItemReservation(result))) {
        throw const AppFailure(FailureKind.conflict);
      }
    }
    records[intent.operationId] = value;
    validateManagedInstallations(draft['managedInstallations']);
    return result;
  }

  Future<ManagedInstallationLease> beginStage(
    ManagedInstallationLease lease,
    CancellationToken cancellation, {
    String? attemptId,
    void Function()? checkAuthority,
  }) => _update(lease, cancellation, (current) {
    if (current.manifest != null && checkAuthority == null) {
      throw const AppFailure(FailureKind.denied);
    }
    if (current.state != ManagedInstallationState.prepared &&
        current.state != ManagedInstallationState.resumePrepared) {
      throw const AppFailure(FailureKind.recovery);
    }
    if ((current.attempt != 0 || attemptId != null) &&
        attemptId != current.attemptId) {
      throw const AppFailure(FailureKind.conflict);
    }
    return (
      ManagedInstallationState.staging,
      current.verifiedBytes,
      current.verifiedChunks,
    );
  }, checkAuthority: checkAuthority);

  /// Commits both fresh retry identity and queue admission under the caller's
  /// idle coordinator lease. Native operation identity and slot never change.
  Future<ManagedResumeAttempt> prepareResume(
    ManagedInstallationLease lease,
    ManagedStageObservation observation,
    CancellationToken cancellation, {
    required JobRepository jobs,
    required void Function() checkAdmission,
  }) async {
    if (!identical(jobs.store, store)) {
      throw const AppFailure(FailureKind.invalidData);
    }
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
      if (current.state != ManagedInstallationState.recoveryRequired ||
          current.attempt >= 999 ||
          observation.operationId != current.operationId ||
          observation.state != ManagedStageObservationState.checkpoint ||
          observation.rootIdentity != current.rootIdentity ||
          observation.sourceIdentity != current.sourceIdentity ||
          observation.verifiedBytes < current.resumeFloorBytes ||
          observation.verifiedBytes >= current.intent.expectedSize) {
        throw const AppFailure(FailureKind.recovery);
      }
      validateManagedCheckpoint(
        current.intent,
        observation.verifiedBytes,
        observation.verifiedChunks,
      );
      final next = _next(
        value,
        current,
        ManagedInstallationState.resumePrepared,
        observation.verifiedBytes,
        observation.verifiedChunks,
        resumeFloorBytes: observation.verifiedBytes,
      );
      next['attempt'] = current.attempt + 1;
      result = _decode(next, draft);
      job = jobs.admitRetryIn(
        draft,
        current.scope,
        kind: 'managedContentStage',
        item: current.operationId,
        previousIntentId: current.attemptId,
        intentId: result.attemptId,
        checkpoint: result.verifiedBytes,
      );
      records[current.operationId] = next;
      validateManagedInstallations(draft['managedInstallations']);
    });
    final attempt = ManagedResumeAttempt._(
      ManagedInstallationLease._(result, lease.sessionGeneration),
      job,
    );
    _changed();
    jobs.retryChanged();
    try {
      check();
    } catch (_) {
      try {
        await abortResume(attempt, jobs: jobs);
      } on AppFailure catch (failure) {
        if (failure.kind != FailureKind.conflict) rethrow;
      }
      rethrow;
    }
    return attempt;
  }

  /// Cancels only the exact unstarted retry while retaining its bytes/reservation.
  /// Safety cleanup is allowed after account/cancellation invalidates admission.
  Future<void> abortResume(
    ManagedResumeAttempt attempt, {
    required JobRepository jobs,
  }) async {
    if (!identical(jobs.store, store)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    await store.transact((draft) {
      _checkOpen();
      final records = _records(draft, create: false);
      final value = records[attempt.lease.record.operationId] as Map?;
      if (value == null) throw const AppFailure(FailureKind.conflict);
      final current = _decode(value, draft);
      _checkLease(attempt.lease, current);
      if (current.state != ManagedInstallationState.resumePrepared ||
          attempt.job.intentId != current.attemptId) {
        throw const AppFailure(FailureKind.conflict);
      }
      jobs.cancelQueuedRetryIn(draft, current.scope, attempt.job);
      records[current.operationId] = _next(
        value,
        current,
        ManagedInstallationState.recoveryRequired,
        current.verifiedBytes,
        current.verifiedChunks,
      );
      validateManagedInstallations(draft['managedInstallations']);
    });
    _changed();
    jobs.retryChanged();
  }

  /// Native errors/cancellation preserve the reservation and require recovery.
  /// Only exact matching identities and complete verified counts yield staged.
  /// Recovery observations may reduce progress after corruption is discovered.
  Future<ManagedInstallationLease> reconcile(
    ManagedInstallationLease lease,
    ManagedStageObservation observation,
    CancellationToken cancellation,
  ) => _update(lease, cancellation, (current) {
    if (current.state == ManagedInstallationState.cancelled ||
        isManagedDiscardState(current.state) ||
        current.state == ManagedInstallationState.prepared ||
        current.state == ManagedInstallationState.resumePrepared ||
        observation.operationId != current.operationId) {
      throw const AppFailure(FailureKind.conflict);
    }
    final sameIdentity =
        observation.hasIdentities &&
        observation.rootIdentity == current.rootIdentity &&
        observation.sourceIdentity == current.sourceIdentity;
    if (!sameIdentity) {
      return (ManagedInstallationState.recoveryRequired, 0, 0);
    }
    validateManagedCheckpoint(
      current.intent,
      observation.verifiedBytes,
      observation.verifiedChunks,
    );
    if (observation.state == ManagedStageObservationState.staged &&
        observation.verifiedBytes == current.intent.expectedSize &&
        observation.verifiedChunks == current.intent.expectedChunks) {
      return (
        ManagedInstallationState.staged,
        observation.verifiedBytes,
        observation.verifiedChunks,
      );
    }
    // A completed Stage invocation which retained only a checkpoint is not a
    // background worker still running. Resume must be explicit and reinspected.
    return (
      ManagedInstallationState.recoveryRequired,
      observation.verifiedBytes,
      observation.verifiedChunks,
    );
  }, verifiedObservation: observation);

  Future<ManagedInstallationLease> cancelPrepared(
    ManagedInstallationLease lease,
    CancellationToken cancellation,
  ) => _update(lease, cancellation, (current) {
    if (current.state != ManagedInstallationState.prepared) {
      throw const AppFailure(FailureKind.recovery);
    }
    return (ManagedInstallationState.cancelled, 0, 0);
  });

  /// Safety-only downgrade after cancellation/account replacement. It can never
  /// authorize effects or publish success, and still requires the exact CAS.
  Future<ManagedInstallationLease> requireRecovery(
    ManagedInstallationLease lease,
  ) => _update(lease, null, (current) {
    if (current.state != ManagedInstallationState.prepared &&
        current.state != ManagedInstallationState.resumePrepared &&
        current.state != ManagedInstallationState.staging &&
        current.state != ManagedInstallationState.recoveryRequired) {
      throw const AppFailure(FailureKind.conflict);
    }
    return (
      ManagedInstallationState.recoveryRequired,
      current.verifiedBytes,
      current.verifiedChunks,
    );
  }, safetyOnly: true);

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
  }) => _ManagedInstallationDiscard(this).prepareDiscard(
    lease,
    plan,
    verifiedBytes,
    verifiedChunks,
    cancellation,
    jobs: jobs,
    checkAdmission: checkAdmission,
  );

  Future<void> abortDiscard(
    ManagedDiscardAttempt attempt, {
    required JobRepository jobs,
  }) => _ManagedInstallationDiscard(this).abortDiscard(attempt, jobs: jobs);

  Future<ManagedInstallationLease> beginDiscard(
    ManagedInstallationLease lease,
    String attemptId,
    CancellationToken token,
  ) => _ManagedInstallationDiscard(this).beginDiscard(lease, attemptId, token);

  Future<ManagedInstallationLease> requireDiscardRecovery(
    ManagedInstallationLease lease,
  ) => _ManagedInstallationDiscard(this).requireDiscardRecovery(lease);

  Future<ManagedInstallationLease> reconcileDiscard(
    ManagedInstallationLease lease,
    ContentDiscardReceipt receipt,
    CancellationToken token, {
    JobRepository? observedJobs,
  }) =>
      _ManagedInstallationDiscard(this)
          .reconcileDiscard(lease, receipt, token, observedJobs: observedJobs);

  /// Crash recovery after inventory committed but the queue terminal write did
  /// not. Already observed completion is durable; no filesystem effects replay.
  Future<void> settleDiscarded(
    ManagedInstallationLease lease,
    CancellationToken token, {
    required JobRepository jobs,
  }) =>
      _ManagedInstallationDiscard(this)
          .settleDiscarded(lease, token, jobs: jobs);

  /// Explicit bootstrap/interrupt reconciliation, across this device's scopes.
  /// No native work is invoked and no interrupted record is replayed.
  Future<int> recoverInterrupted() async {
    _checkOpen();
    final data = store.read();
    final snapshot = _records(data, create: false);
    if (!snapshot.values.any((value) {
      final record = _decode(value as Map, data);
      return record.deviceId == deviceId &&
          (record.state == ManagedInstallationState.prepared ||
              record.state == ManagedInstallationState.resumePrepared ||
              record.state == ManagedInstallationState.staging ||
              record.state == ManagedInstallationState.discardPrepared ||
              record.state == ManagedInstallationState.discarding);
    })) {
      return 0;
    }
    var recovered = 0;
    await store.transact((draft) {
      _checkOpen();
      final records = _records(draft, create: false);
      for (final key in records.keys.toList()) {
        final value = records[key] as Map;
        final current = _decode(value, draft);
        if (current.deviceId == deviceId &&
            (current.state == ManagedInstallationState.prepared ||
                current.state == ManagedInstallationState.resumePrepared ||
                current.state == ManagedInstallationState.staging ||
                current.state == ManagedInstallationState.discardPrepared ||
                current.state == ManagedInstallationState.discarding)) {
          records[key] = _next(
            value,
            current,
            isManagedDiscardState(current.state)
                ? ManagedInstallationState.discardRecoveryRequired
                : ManagedInstallationState.recoveryRequired,
            current.verifiedBytes,
            current.verifiedChunks,
          );
          recovered++;
        }
      }
      validateManagedInstallations(draft['managedInstallations']);
    });
    if (recovered != 0) _changed();
    return recovered;
  }

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
  }) => _ManagedInstallationMutations(this)._update(
    lease,
    cancellation,
    change,
    safetyOnly: safetyOnly,
    verifiedObservation: verifiedObservation,
    discardObservation: discardObservation,
    observedDiscardJobs: observedDiscardJobs,
    checkAuthority: checkAuthority,
  );

  void _checkLease(
    ManagedInstallationLease lease,
    ManagedInstallationRecord current,
  ) {
    if (current.revision != lease.record.revision ||
        current.deviceId != deviceId ||
        current.deviceId != lease.record.deviceId ||
        current.scope.key != lease.record.scope.key ||
        current.attempt != lease.record.attempt ||
        current.discardAttemptId != lease.record.discardAttemptId ||
        !listEquals(current.discard?.plan, lease.record.discard?.plan) ||
        current.discard?.verifiedBytes != lease.record.discard?.verifiedBytes ||
        current.discard?.verifiedChunks !=
            lease.record.discard?.verifiedChunks ||
        !current.intent.sameRequest(lease.record.intent) ||
        current.manifest?.recordIdentity !=
            lease.record.manifest?.recordIdentity ||
        current.rootIdentity != lease.record.rootIdentity ||
        current.sourceIdentity != lease.record.sourceIdentity) {
      throw const AppFailure(FailureKind.conflict);
    }
  }

  /// A transaction can flush successfully while its originating account/token
  /// is cancelled. The caller has not received the new CAS revision yet, so this
  /// repository must durably downgrade it before rejecting that completion.
  Future<void> _recoverCommitted(ManagedInstallationLease lease) async {
    if (lease.record.state == ManagedInstallationState.discardPrepared ||
        lease.record.state == ManagedInstallationState.discarding) {
      try {
        await requireDiscardRecovery(lease);
      } on AppFailure catch (failure) {
        if (failure.kind != FailureKind.conflict) rethrow;
      }
      return;
    }
    if (lease.record.state != ManagedInstallationState.prepared &&
        lease.record.state != ManagedInstallationState.resumePrepared &&
        lease.record.state != ManagedInstallationState.staging) {
      return;
    }
    try {
      await requireRecovery(lease);
    } on AppFailure catch (failure) {
      // Another writer may already have reconciled this exact revision. Never
      // overwrite that later state or replace the originating cancellation.
      if (failure.kind != FailureKind.conflict) rethrow;
    }
  }

  Map<String, Object?> _next(
    Map value,
    ManagedInstallationRecord current,
    ManagedInstallationState state,
    int bytes,
    int chunks, {
    int? resumeFloorBytes,
  }) => {
    ...Map<String, Object?>.from(value),
    'state': state.name,
    'revision': current.revision + 1,
    'verifiedBytes': bytes,
    'verifiedChunks': chunks,
    'resumeFloorBytes':
        resumeFloorBytes != null && resumeFloorBytes > current.resumeFloorBytes
        ? resumeFloorBytes
        : current.resumeFloorBytes,
    'updatedAt': DateTime.now().toUtc().toIso8601String(),
  };

  ManagedInstallationRecord _decode(Map value, StoreData data) =>
      ManagedInstallationRecord.decode(
        value,
        envelopeVersion:
            (data['managedInstallations'] as Map?)?['version'] as int? ?? 1,
      );

  Map _records(
    StoreData data, {
    required bool create,
    bool promoteManifest = false,
  }) {
    _checkOpen();
    final existing = data['managedInstallations'];
    validateManagedInstallations(existing);
    if (existing == null) {
      if (!create) return <String, Object?>{};
      final envelope = <String, Object?>{
        'version': promoteManifest ? 2 : 1,
        'records': <String, Object?>{},
      };
      data['managedInstallations'] = envelope;
      return envelope['records'] as Map;
    }
    final envelope = existing as Map;
    if (!supportsManagedInstallations(envelope)) {
      throw const AppFailure(FailureKind.unsupported);
    }
    if (promoteManifest && envelope['version'] == 1) {
      final records = envelope['records'] as Map;
      if (records.values.any(
        (raw) =>
            (raw as Map).containsKey('recordVersion') ||
            raw.containsKey('managedManifest'),
      )) {
        // These names may be unknown legacy extensions. Never reinterpret them.
        throw const AppFailure(FailureKind.unsupported);
      }
      envelope['version'] = 2;
    }
    return envelope['records'] as Map;
  }

  void _checkScope(
    AccountScope scope,
    int generation,
    CancellationToken token,
  ) {
    _checkOpen();
    token.check();
    if (session.scope.key != scope.key || session.generation != generation) {
      throw const AppFailure(FailureKind.cancelled);
    }
  }

  void _checkOpen() {
    if (_disposed) throw const AppFailure(FailureKind.cancelled);
  }

  void _changed() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }
}
