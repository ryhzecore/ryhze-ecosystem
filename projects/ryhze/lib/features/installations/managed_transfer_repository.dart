import 'dart:convert';

import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../../data/transaction_store.dart';
import '../jobs/job_repository.dart';
import '../session/session_controller.dart';
import 'content_transfer_backend.dart';
import 'managed_content_manifest.dart';
import 'managed_installation.dart';
import 'managed_transfer_record.dart';

class ManagedTransferLease {
  ManagedTransferLease._(this.record, this.sessionGeneration);
  final ManagedTransferRecord record;
  final int sessionGeneration;
}

class ManagedTransferAttempt {
  ManagedTransferAttempt._(this.lease, this.job);
  final ManagedTransferLease lease;
  final JobRecord job;
}

/// Sole durable writer for incoming transfers. No method performs filesystem or
/// network effects. Observations are integrity evidence, never renewed rights.
class ManagedTransferRepository extends ChangeNotifier {
  ManagedTransferRepository(
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

  List<ManagedTransferRecord> readCurrent() => List.unmodifiable(
    _records(store.read(), create: false).values
        .map((raw) => ManagedTransferRecord.decode(raw as Map))
        .where(
          (record) =>
              record.deviceId == deviceId &&
              record.scope.key == session.scope.key,
        ),
  );

  ManagedTransferRecord? findCurrent(String operationId) {
    for (final record in readCurrent()) {
      if (record.operationId == operationId) {
        return record;
      }
    }
    return null;
  }

  ManagedTransferLease? leaseCurrent(String operationId) {
    final record = findCurrent(operationId);
    return record == null
        ? null
        : ManagedTransferLease._(record, session.generation);
  }

  bool canDiscard(ManagedTransferRecord record) {
    if (record.deviceId != deviceId ||
        record.scope.key != session.scope.key ||
        record.state == ManagedTransferState.prepared ||
        record.state == ManagedTransferState.receiving ||
        record.state == ManagedTransferState.discarded) {
      return false;
    }
    final snapshot = store.read();
    if (!supportsManagedInstallations(snapshot['managedInstallations'])) {
      return false;
    }
    return _stageAllowsDiscard(snapshot, record.operationId);
  }

  /// Project eligibility once per UI refresh, not one whole-store clone per row.
  Set<String> discardableCurrent() {
    final snapshot = store.read();
    if (!supportsManagedInstallations(snapshot['managedInstallations'])) {
      return const <String>{};
    }
    final result = <String>{};
    for (final raw in _records(snapshot, create: false).values) {
      final record = ManagedTransferRecord.decode(raw as Map);
      if (record.deviceId == deviceId &&
          record.scope.key == session.scope.key &&
          record.state != ManagedTransferState.prepared &&
          record.state != ManagedTransferState.receiving &&
          record.state != ManagedTransferState.discarded &&
          _stageAllowsDiscard(snapshot, record.operationId)) {
        result.add(record.operationId);
      }
    }
    return Set.unmodifiable(result);
  }

  Future<ManagedTransferAttempt> prepareTransfer(
    ManagedStageIntent intent,
    ManagedManifestProvenance manifest,
    String sourceKey,
    CancellationToken token, {
    required JobRepository jobs,
    required void Function() checkAdmission,
    ContentTransferRequest? initialObservation,
  }) async {
    _checkJobs(jobs);
    final scope = session.scope, generation = session.generation;
    validateManagedScope(scope);
    validateManagedTransferSourceKey(sourceKey);
    if (scope.isLocal) {
      throw const AppFailure(FailureKind.denied);
    }
    final request =
        initialObservation ??
        ContentTransferRequest(
          slot: intent.slot,
          operationId: intent.operationId,
          expectedSize: intent.expectedSize,
          expectedSha256: intent.expectedSha256,
        );
    decodeTransferRequest(encodeTransferRequest(request));
    if (intent.sourceRelative != request.sourceRelative ||
        !transferRequestMatchesIntent(request, intent) ||
        request.attempt != 0 ||
        request.sourceIdentity.isNotEmpty ||
        request.observedBytes != 0 ||
        request.observedSha256.isNotEmpty) {
      throw const AppFailure(FailureKind.invalidData);
    }
    void check() {
      _checkScope(scope, generation, token);
      checkAdmission();
    }

    check();
    late ManagedTransferRecord result;
    late ({JobRecord job, bool inserted}) job;
    await store.transact((draft) {
      check();
      _checkSupportedInventories(draft);
      final records = _records(draft, create: true);
      final existing = records[intent.operationId];
      if (existing != null) {
        result = ManagedTransferRecord.decode(existing as Map);
        if (result.deviceId != deviceId ||
            result.scope.key != scope.key ||
            result.sourceKey != sourceKey ||
            !result.intent.sameRequest(intent) ||
            result.manifest.recordIdentity != manifest.recordIdentity ||
            result.request.rootIdentity != request.rootIdentity ||
            result.request.directoryIdentity != request.directoryIdentity ||
            result.state != ManagedTransferState.prepared ||
            result.attempt != 0) {
          throw const AppFailure(FailureKind.conflict);
        }
      } else {
        if (records.length >= managedInstallationMaximumRecords) {
          throw const AppFailure(FailureKind.conflict);
        }
        final value = <String, Object?>{
          'recordVersion': 1,
          'account': {
            'authority': scope.authority,
            'accountId': scope.accountId,
          },
          'deviceId': deviceId,
          'intent': intent.toJson(),
          'manifest': manifest.toJson(),
          'sourceKey': sourceKey,
          'state': ManagedTransferState.prepared.name,
          'revision': 1,
          'attempt': 0,
          'discardAttempt': 0,
          'request': encodeTransferRequest(request),
          'updatedAt': DateTime.now().toUtc().toIso8601String(),
        };
        result = ManagedTransferRecord.decode(value);
        records[intent.operationId] = value;
      }
      job = jobs.enqueueIn(
        draft,
        scope,
        'managedContentTransfer',
        intent.operationId,
        intentId: result.attemptId,
      );
      if (job.inserted == (existing != null) ||
          job.job.state != JobState.queued ||
          job.job.checkpoint != 0) {
        throw const AppFailure(FailureKind.conflict);
      }
      _validate(draft);
    });
    final attempt = ManagedTransferAttempt._(
      ManagedTransferLease._(result, generation),
      job.job,
    );
    _changed();
    if (job.inserted) {
      jobs.admissionChanged();
    }
    try {
      check();
    } catch (_) {
      await abortPrepared(attempt, jobs: jobs);
      rethrow;
    }
    return attempt;
  }

  Future<ManagedTransferLease> begin(
    ManagedTransferLease lease,
    String attemptId,
    CancellationToken token, {
    required void Function() checkAuthority,
  }) => _update(lease, token, (current, next) {
    if (current.state != ManagedTransferState.prepared ||
        current.attemptId != attemptId) {
      throw const AppFailure(FailureKind.conflict);
    }
    next['state'] = ManagedTransferState.receiving.name;
  }, checkAuthority: checkAuthority);

  Future<ManagedTransferLease> observe(
    ManagedTransferLease lease,
    ContentTransferRequest request,
    CancellationToken token, {
    void Function()? checkAuthority,
    String? strongEtag,
    bool replaceEtag = false,
    bool complete = false,
    JobRepository? jobs,
  }) => _update(
    lease,
    token,
    (current, next) {
      if (jobs != null && !complete) {
        throw const AppFailure(FailureKind.invalidData);
      }
      if (current.state != ManagedTransferState.receiving &&
          current.state != ManagedTransferState.recoveryRequired &&
          current.state != ManagedTransferState.downloaded &&
          current.state != ManagedTransferState.stagingAdmitted &&
          current.state != ManagedTransferState.discarding) {
        throw const AppFailure(FailureKind.conflict);
      }
      _checkObservation(current.request, request, allowUnknownIdentity: true);
      if (complete && !completeTransferRequest(request)) {
        throw const AppFailure(FailureKind.recovery);
      }
      if ((current.state == ManagedTransferState.downloaded ||
              current.state == ManagedTransferState.stagingAdmitted) &&
          (!complete || !completeTransferRequest(request))) {
        throw const AppFailure(FailureKind.recovery);
      }
      _replaceRequest(next, 'request', request);
      if (replaceEtag || strongEtag != null) {
        next['strongEtag'] = strongEtag;
      }
      if (complete &&
          current.state != ManagedTransferState.stagingAdmitted &&
          current.state != ManagedTransferState.discarding) {
        next['state'] = ManagedTransferState.downloaded.name;
      }
    },
    checkAuthority: checkAuthority,
    observedJobs: jobs,
  );

  /// Safety-only settlement after cancellation/account replacement. It may
  /// record exact file observations, but can only downgrade to recovery.
  Future<ManagedTransferLease> requireRecovery(
    ManagedTransferLease lease, {
    ContentTransferRequest? observation,
  }) => _update(lease, null, (current, next) {
    if (current.state == ManagedTransferState.discarded ||
        current.state == ManagedTransferState.stagingAdmitted ||
        current.state == ManagedTransferState.downloaded) {
      throw const AppFailure(FailureKind.conflict);
    }
    if (observation != null) {
      _checkObservation(
        current.request,
        observation,
        allowUnknownIdentity: true,
      );
      _replaceRequest(next, 'request', observation);
    }
    // Preserve deletion intent/job lineage across interruption.
    next['state'] = current.state == ManagedTransferState.discarding
        ? ManagedTransferState.discarding.name
        : ManagedTransferState.recoveryRequired.name;
  }, safetyOnly: true);

  Future<ManagedTransferAttempt> prepareRetry(
    ManagedTransferLease lease,
    ContentTransferRequest observation,
    CancellationToken token, {
    required JobRepository jobs,
    required void Function() checkAdmission,
    String? strongEtag,
  }) => _admit(lease, token, jobs, checkAdmission, (current, next, draft) {
    if (current.state != ManagedTransferState.recoveryRequired ||
        current.attempt >= 999 ||
        current.previous != null) {
      throw const AppFailure(FailureKind.recovery);
    }
    _checkObservation(current.request, observation, allowUnknownIdentity: true);
    final missing =
        observation.sourceIdentity.isEmpty &&
        observation.observedBytes == 0 &&
        observation.observedSha256.isEmpty;
    if (!missing && observation.observedSha256.isEmpty) {
      throw const AppFailure(FailureKind.recovery);
    }
    final request = ContentTransferRequest(
      slot: current.intent.slot,
      operationId: current.operationId,
      expectedSize: current.intent.expectedSize,
      expectedSha256: current.intent.expectedSha256,
      attempt: 1 - current.request.attempt,
      rootIdentity: observation.rootIdentity,
      directoryIdentity: observation.directoryIdentity,
    );
    next['attempt'] = current.attempt + 1;
    next['state'] = ManagedTransferState.prepared.name;
    if (missing) {
      next.remove('previous');
    } else {
      _replaceRequest(next, 'previous', observation);
    }
    _replaceRequest(next, 'request', request);
    next['strongEtag'] = missing ? null : strongEtag;
    final updated = ManagedTransferRecord.decode(next);
    return jobs.admitRetryIn(
      draft,
      current.scope,
      kind: 'managedContentTransfer',
      item: current.operationId,
      previousIntentId: current.attemptId,
      intentId: updated.attemptId,
      checkpoint: 0,
    );
  });

  Future<ManagedTransferLease> clearPrevious(
    ManagedTransferLease lease,
    ContentTransferReceipt receipt,
    CancellationToken token,
  ) => _update(lease, token, (current, next) {
    final previous = current.previous;
    if (previous == null || current.state != ManagedTransferState.discarding) {
      throw const AppFailure(FailureKind.conflict);
    }
    _checkDiscardReceipt(previous, receipt);
    next.remove('previous');
  });

  Future<ManagedTransferAttempt> prepareDiscard(
    ManagedTransferLease lease,
    CancellationToken token, {
    required JobRepository jobs,
    required void Function() checkAdmission,
  }) => _admit(lease, token, jobs, checkAdmission, (current, next, draft) {
    if (current.state == ManagedTransferState.prepared ||
        current.state == ManagedTransferState.receiving ||
        current.state == ManagedTransferState.discarded ||
        current.discardAttempt >= 999) {
      throw const AppFailure(FailureKind.conflict);
    }
    if (!_stageAllowsDiscard(draft, current.operationId)) {
      throw const AppFailure(FailureKind.conflict);
    }
    next['state'] = ManagedTransferState.discarding.name;
    next['discardAttempt'] = current.discardAttempt + 1;
    final updated = ManagedTransferRecord.decode(next);
    return jobs.admitRetryIn(
      draft,
      current.scope,
      kind: 'managedContentTransferDiscard',
      item: current.operationId,
      previousKind: current.discardAttempt == 0
          ? 'managedContentTransfer'
          : 'managedContentTransferDiscard',
      previousIntentId: current.discardAttempt == 0
          ? current.attemptId
          : current.discardAttemptId,
      intentId: updated.discardAttemptId,
      checkpoint: 0,
    );
  });

  Future<ManagedTransferLease> beginDiscard(
    ManagedTransferLease lease,
    CancellationToken token, {
    String? attemptId,
  }) => _update(lease, token, (current, next) {
    if (current.state != ManagedTransferState.discarding ||
        (attemptId != null && current.discardAttemptId != attemptId)) {
      throw const AppFailure(FailureKind.conflict);
    }
  });

  Future<ManagedTransferLease> reconcileDiscard(
    ManagedTransferLease lease,
    ContentTransferReceipt receipt,
    CancellationToken token, {
    JobRepository? jobs,
  }) async {
    if (jobs != null) {
      _checkJobs(jobs);
    }
    final result = await _update(lease, token, (current, next) {
      if (current.state != ManagedTransferState.discarding ||
          current.previous != null) {
        throw const AppFailure(FailureKind.conflict);
      }
      _checkDiscardReceipt(current.request, receipt);
      next['state'] = ManagedTransferState.discarded.name;
    }, observedJobs: jobs);
    return result;
  }

  /// Refresh after the staging repository's atomic transfer/inventory/job commit.
  void stageAdmissionChanged() => _changed();

  /// An abort after durable admission cancels that exact queued job even after
  /// session replacement. Never erases its reservation or incoming observations.
  Future<void> abortPrepared(
    ManagedTransferAttempt attempt, {
    required JobRepository jobs,
  }) async {
    _checkJobs(jobs);
    await store.transact((draft) {
      _checkOpen();
      final records = _records(draft, create: false);
      final raw = records[attempt.lease.record.operationId] as Map?;
      if (raw == null) {
        throw const AppFailure(FailureKind.conflict);
      }
      final current = ManagedTransferRecord.decode(raw);
      _checkLease(attempt.lease, current);
      if (current.state != ManagedTransferState.prepared &&
          current.state != ManagedTransferState.discarding) {
        throw const AppFailure(FailureKind.conflict);
      }
      final expected = current.state == ManagedTransferState.discarding
          ? current.discardAttemptId
          : current.attemptId;
      if (attempt.job.intentId != expected) {
        throw const AppFailure(FailureKind.conflict);
      }
      jobs.cancelQueuedRetryIn(draft, current.scope, attempt.job);
      final next = _next(raw, current);
      if (current.state != ManagedTransferState.discarding) {
        next['state'] = ManagedTransferState.recoveryRequired.name;
      }
      records[current.operationId] = next;
      _validate(draft);
    });
    _changed();
    jobs.retryChanged();
  }

  Future<int> recoverInterrupted() async {
    _checkOpen();
    bool interrupted(ManagedTransferRecord record) =>
        record.deviceId == deviceId &&
        (record.state == ManagedTransferState.prepared ||
            record.state == ManagedTransferState.receiving);
    if (!_records(store.read(), create: false).values.any(
      (raw) => interrupted(ManagedTransferRecord.decode(raw as Map)),
    )) {
      return 0;
    }
    var count = 0;
    await store.transact((draft) {
      final records = _records(draft, create: false);
      for (final key in records.keys.toList()) {
        final raw = records[key] as Map;
        final current = ManagedTransferRecord.decode(raw);
        if (interrupted(current)) {
          records[key] = _next(raw, current)
            ..['state'] = ManagedTransferState.recoveryRequired.name;
          count++;
        }
      }
      _validate(draft);
    });
    if (count != 0) {
      _changed();
    }
    return count;
  }

  Future<ManagedTransferAttempt> _admit(
    ManagedTransferLease lease,
    CancellationToken token,
    JobRepository jobs,
    void Function() checkAdmission,
    JobRecord Function(ManagedTransferRecord, Map<String, Object?>, StoreData)
    change,
  ) async {
    _checkJobs(jobs);
    void check() {
      _checkScope(lease.record.scope, lease.sessionGeneration, token);
      checkAdmission();
    }

    check();
    late ManagedTransferRecord result;
    late JobRecord job;
    await store.transact((draft) {
      check();
      _checkSupportedInventories(draft);
      final records = _records(draft, create: false);
      final raw = records[lease.record.operationId] as Map?;
      if (raw == null) {
        throw const AppFailure(FailureKind.conflict);
      }
      final current = ManagedTransferRecord.decode(raw);
      _checkLease(lease, current);
      final next = _next(raw, current);
      job = change(current, next, draft);
      result = ManagedTransferRecord.decode(next);
      records[current.operationId] = next;
      _validate(draft);
    });
    final attempt = ManagedTransferAttempt._(
      ManagedTransferLease._(result, lease.sessionGeneration),
      job,
    );
    _changed();
    jobs.retryChanged();
    try {
      check();
    } catch (_) {
      await abortPrepared(attempt, jobs: jobs);
      rethrow;
    }
    return attempt;
  }

  Future<ManagedTransferLease> _update(
    ManagedTransferLease lease,
    CancellationToken? token,
    void Function(ManagedTransferRecord, Map<String, Object?>) change, {
    bool safetyOnly = false,
    void Function()? checkAuthority,
    JobRepository? observedJobs,
  }) async {
    if (observedJobs != null) {
      _checkJobs(observedJobs);
    }
    void check() {
      _checkOpen();
      if (lease.record.deviceId != deviceId) {
        throw const AppFailure(FailureKind.conflict);
      }
      if (!safetyOnly) {
        _checkScope(lease.record.scope, lease.sessionGeneration, token!);
      }
      checkAuthority?.call();
    }

    check();
    late ManagedTransferRecord result;
    var jobChanged = false;
    await store.transact((draft) {
      check();
      if (!safetyOnly) {
        _checkSupportedInventories(draft);
      }
      final records = _records(draft, create: false);
      final raw = records[lease.record.operationId] as Map?;
      if (raw == null) {
        throw const AppFailure(FailureKind.conflict);
      }
      final current = ManagedTransferRecord.decode(raw);
      _checkLease(lease, current);
      final next = _next(raw, current);
      change(current, next);
      result = ManagedTransferRecord.decode(next);
      records[current.operationId] = next;
      _validate(draft);
      if (observedJobs != null &&
          result.state == ManagedTransferState.discarded) {
        jobChanged = observedJobs.completeObservedIn(
          draft,
          current.scope,
          kind: 'managedContentTransferDiscard',
          item: current.operationId,
          intentId: current.discardAttemptId,
          checkpoint: current.request.observedBytes,
        );
      } else if (observedJobs != null &&
          (result.state == ManagedTransferState.downloaded ||
              result.state == ManagedTransferState.stagingAdmitted)) {
        jobChanged = observedJobs.completeObservedIn(
          draft,
          current.scope,
          kind: 'managedContentTransfer',
          item: current.operationId,
          intentId: current.attemptId,
          checkpoint: result.request.observedBytes,
        );
      }
    });
    _changed();
    if (jobChanged) {
      observedJobs!.retryChanged();
    }
    final resultLease = ManagedTransferLease._(result, lease.sessionGeneration);
    try {
      check();
    } catch (_) {
      if (!safetyOnly &&
          (result.state == ManagedTransferState.prepared ||
              result.state == ManagedTransferState.receiving)) {
        await requireRecovery(resultLease);
      }
      rethrow;
    }
    return resultLease;
  }

  Map<String, Object?> _next(Map raw, ManagedTransferRecord current) => {
    ...Map<String, Object?>.from(raw),
    'revision': current.revision + 1,
    'updatedAt': DateTime.now().toUtc().toIso8601String(),
  };

  void _checkLease(ManagedTransferLease lease, ManagedTransferRecord current) {
    final old = lease.record;
    if (current.deviceId != deviceId ||
        current.deviceId != old.deviceId ||
        current.scope.key != old.scope.key ||
        current.operationId != old.operationId ||
        current.revision != old.revision ||
        current.attempt != old.attempt ||
        current.discardAttempt != old.discardAttempt ||
        current.state != old.state ||
        current.strongEtag != old.strongEtag ||
        current.sourceKey != old.sourceKey ||
        !current.intent.sameRequest(old.intent) ||
        current.manifest.recordIdentity != old.manifest.recordIdentity ||
        jsonEncode(encodeTransferRequest(current.request)) !=
            jsonEncode(encodeTransferRequest(old.request)) ||
        jsonEncode(
              current.previous == null
                  ? null
                  : encodeTransferRequest(current.previous!),
            ) !=
            jsonEncode(
              old.previous == null
                  ? null
                  : encodeTransferRequest(old.previous!),
            )) {
      throw const AppFailure(FailureKind.conflict);
    }
  }

  void _checkObservation(
    ContentTransferRequest before,
    ContentTransferRequest after, {
    required bool allowUnknownIdentity,
  }) {
    // Construction validates shape; decoding adds identity/observation coherence.
    decodeTransferRequest(encodeTransferRequest(after));
    if (before.slot != after.slot ||
        before.operationId != after.operationId ||
        before.expectedSize != after.expectedSize ||
        before.expectedSha256 != after.expectedSha256 ||
        before.attempt != after.attempt ||
        (before.rootIdentity.isNotEmpty &&
            before.rootIdentity != after.rootIdentity) ||
        (before.directoryIdentity.isNotEmpty &&
            before.directoryIdentity != after.directoryIdentity) ||
        (before.sourceIdentity.isNotEmpty &&
            before.sourceIdentity != after.sourceIdentity) ||
        (!allowUnknownIdentity && after.sourceIdentity.isEmpty)) {
      throw const AppFailure(FailureKind.conflict);
    }
  }

  void _checkDiscardReceipt(
    ContentTransferRequest request,
    ContentTransferReceipt receipt,
  ) {
    if (receipt.state != ContentTransferState.discarded ||
        receipt.sourceRelative != request.sourceRelative ||
        (request.rootIdentity.isNotEmpty &&
            receipt.rootIdentity != request.rootIdentity) ||
        (request.directoryIdentity.isNotEmpty &&
            receipt.directoryIdentity != request.directoryIdentity) ||
        (request.sourceIdentity.isNotEmpty &&
            receipt.sourceIdentity != request.sourceIdentity) ||
        receipt.observedBytes != request.observedBytes ||
        receipt.observedSha256 != request.observedSha256) {
      throw const AppFailure(FailureKind.recovery);
    }
  }

  bool _stageAllowsDiscard(StoreData snapshot, String operationId) {
    final envelope = snapshot['managedInstallations'] as Map?;
    final value = (envelope?['records'] as Map?)?[operationId];
    if (value == null) {
      return true;
    }
    final stage = ManagedInstallationRecord.decode(
      value as Map,
      envelopeVersion: envelope!['version'] as int,
    );
    return stage.state == ManagedInstallationState.staged ||
        stage.state == ManagedInstallationState.cancelled ||
        stage.state == ManagedInstallationState.discarded;
  }

  void _replaceRequest(
    Map<String, Object?> next,
    String key,
    ContentTransferRequest request,
  ) {
    next[key] = {
      if (next[key] is Map) ...Map<String, Object?>.from(next[key] as Map),
      ...encodeTransferRequest(request),
    };
  }

  void _validate(StoreData draft) {
    validateManagedTransfers(draft['managedTransfers']);
    validateManagedContentReservations(draft);
  }

  void _checkSupportedInventories(StoreData data) {
    if (!supportsManagedTransfers(data['managedTransfers']) ||
        !supportsManagedInstallations(data['managedInstallations'])) {
      throw const AppFailure(FailureKind.unsupported);
    }
  }

  Map _records(StoreData data, {required bool create}) {
    _checkOpen();
    final existing = data['managedTransfers'];
    validateManagedTransfers(existing);
    if (!supportsManagedTransfers(existing)) {
      throw const AppFailure(FailureKind.unsupported);
    }
    if (existing == null) {
      if (!create) {
        return <String, Object?>{};
      }
      data['managedTransfers'] = {'version': 1, 'records': <String, Object?>{}};
    }
    return (data['managedTransfers'] as Map)['records'] as Map;
  }

  void _checkJobs(JobRepository jobs) {
    if (!identical(jobs.store, store)) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  void _checkScope(
    AccountScope scope,
    int generation,
    CancellationToken token,
  ) {
    _checkOpen();
    token.check();
    if (scope.key != session.scope.key || generation != session.generation) {
      throw const AppFailure(FailureKind.cancelled);
    }
  }

  void _checkOpen() {
    if (_disposed) {
      throw const AppFailure(FailureKind.cancelled);
    }
  }

  void _changed() {
    if (!_disposed) {
      notifyListeners();
    }
  }

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }
}
