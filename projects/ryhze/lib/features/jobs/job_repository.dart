import 'dart:convert';

import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../../domain/activity.dart';
import '../../domain/app_route.dart';
import '../activity/activity_repository.dart';
import '../../data/transaction_store.dart';
import '../session/session_controller.dart';

enum JobState {
  queued,
  running,
  handoffAccepted,
  completed,
  cancelled,
  failed,
  recoveryRequired,
  blocked,
}

class JobRecord {
  const JobRecord({
    required this.key,
    required this.kind,
    required this.item,
    required this.state,
    required this.updatedAt,
    this.checkpoint = 0,
    this.intentId,
    this.failure,
  });
  final String key, kind, item;
  final String? intentId;
  final FailureKind? failure;
  final JobState state;
  final DateTime updatedAt;
  final int checkpoint;
  bool get pending =>
      state == JobState.queued ||
      state == JobState.running ||
      state == JobState.recoveryRequired;
  Map<String, Object?> toJson() => {
    'kind': kind,
    'item': item,
    'state': state.name,
    'updatedAt': updatedAt.toUtc().toIso8601String(),
    'checkpoint': checkpoint,
    if (intentId != null) 'intent': intentId,
    if (failure != null) 'failure': failure!.name,
  };
  static JobRecord decode(String key, Map value) => JobRecord(
    key: key,
    kind: value['kind'] as String,
    item: value['item'] as String,
    state: JobState.values.byName(value['state'] as String),
    updatedAt: DateTime.parse(value['updatedAt'] as String),
    checkpoint: value['checkpoint'] as int,
    intentId: value['intent'] as String?,
    failure: value['failure'] == null
        ? null
        : FailureKind.values.byName(value['failure'] as String),
  );
}

class JobRepository extends ChangeNotifier {
  JobRepository(this.store, {this.maxPending = 32, this.activity});
  final ActivityRepository? activity;
  final TransactionStore store;
  final int maxPending;
  bool _disposed = false;

  void _changed() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }

  List<JobRecord> read(AccountScope scope) {
    final account = (store.read()['accounts'] as Map)[scope.key] as Map?;
    final jobs = account?['jobs'] as Map? ?? {};
    return List.unmodifiable(
      jobs.entries.map(
        (e) => JobRecord.decode(e.key as String, e.value as Map),
      ),
    );
  }

  Future<JobRecord> enqueue(
    AccountScope scope,
    String kind,
    String item, {
    String? intentId,
  }) async {
    late ({JobRecord job, bool inserted}) admission;
    await store.transact((draft) {
      admission = enqueueIn(draft, scope, kind, item, intentId: intentId);
    });
    if (admission.inserted) admissionChanged();
    return admission.job;
  }

  /// Transaction participant for initial admission. The owning repository
  /// validates its operation and session in this same draft before committing.
  /// Notify listeners only after the complete transaction has settled.
  ({JobRecord job, bool inserted}) enqueueIn(
    StoreData draft,
    AccountScope scope,
    String kind,
    String item, {
    String? intentId,
  }) {
    if (kind.isEmpty ||
        kind.length > 80 ||
        item.isEmpty ||
        item.length > 256 ||
        RegExp(r'[\x00-\x1f]').hasMatch(kind + item) ||
        (intentId != null &&
            !RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')
                .hasMatch(intentId))) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final key = jsonEncode([kind, item, ?intentId]);
    final account = (draft['accounts'] as Map).putIfAbsent(
      scope.key,
      () => <String, Object?>{},
    ) as Map;
    final jobs = account.putIfAbsent('jobs', () => <String, Object?>{}) as Map;
    final existing = jobs[key];
    if (existing != null) {
      return (job: JobRecord.decode(key, existing as Map), inserted: false);
    }
    // A different intent is explicit new work, never an automatic retry.
    // Do not admit a second operation while this exact item needs reconciliation.
    if (jobs.entries.any((e) {
      final old = JobRecord.decode(e.key as String, e.value as Map);
      return old.item == item && old.pending;
    })) {
      throw const AppFailure(FailureKind.conflict);
    }
    if (jobs.length >= 1000 ||
        jobs.entries
                .where(
                  (e) =>
                      JobRecord.decode(e.key as String, e.value as Map).pending,
                )
                .length >=
            maxPending) {
      throw const AppFailure(FailureKind.conflict);
    }
    final result = JobRecord(
      key: key,
      intentId: intentId,
      kind: kind,
      item: item,
      state: JobState.queued,
      updatedAt: DateTime.now().toUtc(),
    );
    jobs[key] = result.toJson();
    return (job: result, inserted: true);
  }

  void admissionChanged() => _changed();

  Future<void> transition(
    AccountScope scope,
    JobRecord job,
    JobState next, {
    int? checkpoint,
    FailureKind? failure,
  }) async {
    await store.transact((draft) {
      final account = (draft['accounts'] as Map)[scope.key] as Map?;
      final jobs = account?['jobs'] as Map?;
      final value = jobs?[job.key];
      if (value == null) throw const AppFailure(FailureKind.invalidData);
      final current = JobRecord.decode(job.key, value as Map);
      final allowed = switch (current.state) {
        JobState.queued => {
          JobState.running,
          JobState.cancelled,
          JobState.blocked,
        },
        JobState.running => {
          JobState.running,
          JobState.completed,
          JobState.handoffAccepted,
          JobState.recoveryRequired,
          JobState.failed,
        },
        JobState.recoveryRequired => {
          JobState.completed,
          JobState.handoffAccepted,
          JobState.cancelled,
          JobState.failed,
          JobState.recoveryRequired,
        },
        _ => <JobState>{},
      };
      if (!allowed.contains(next) ||
          (checkpoint != null && checkpoint < current.checkpoint)) {
        throw const AppFailure(FailureKind.conflict);
      }
      final event = switch (next) {
        JobState.completed => ActivityKind.completed,
        JobState.handoffAccepted => ActivityKind.handoff,
        JobState.recoveryRequired => ActivityKind.recovery,
        JobState.failed => ActivityKind.failed,
        JobState.blocked => ActivityKind.blocked,
        _ => null,
      };
      if (event != null) {
        ActivityRepository.recordIn(
          draft,
          scope,
          job.key,
          event,
          DateTime.now().toUtc(),
          route: const AppRoute(AppDestination.activity),
        );
      }
      jobs![job.key] = JobRecord(
        key: job.key,
        intentId: current.intentId,
        failure:
            failure ??
            (next == JobState.blocked ? FailureKind.unsupported : null),
        kind: current.kind,
        item: current.item,
        state: next,
        updatedAt: DateTime.now().toUtc(),
        checkpoint: checkpoint ?? current.checkpoint,
      ).toJson();
    });
    activity?.changed();
    _changed();
  }

  /// Transaction participant for an explicitly reinspected retry. The owning
  /// service must hold an idle coordinator lease and commit its operation CAS
  /// in this same draft. Generic enqueue idempotence/conflict rules are unchanged.
  JobRecord admitRetryIn(
    StoreData draft,
    AccountScope scope, {
    required String kind,
    required String item,
    required String previousIntentId,
    String? previousKind,
    required String intentId,
    required int checkpoint,
  }) {
    final validIntent = RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$');
    final priorKind = previousKind ?? kind;
    if (kind.isEmpty ||
        kind.length > 80 ||
        priorKind.isEmpty ||
        priorKind.length > 80 ||
        item.isEmpty ||
        item.length > 256 ||
        RegExp(r'[\x00-\x1f]').hasMatch(kind + priorKind + item) ||
        !validIntent.hasMatch(previousIntentId) ||
        !validIntent.hasMatch(intentId) ||
        previousIntentId == intentId ||
        checkpoint < 0) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final account = (draft['accounts'] as Map).putIfAbsent(
      scope.key,
      () => <String, Object?>{},
    ) as Map;
    final records =
        account.putIfAbsent('jobs', () => <String, Object?>{}) as Map;
    final previousKey = jsonEncode([priorKind, item, previousIntentId]);
    final key = jsonEncode([kind, item, intentId]);
    if (records.containsKey(key) || records.length >= 1000) {
      throw const AppFailure(FailureKind.conflict);
    }
    for (final entry in records.entries) {
      final job = JobRecord.decode(entry.key as String, entry.value as Map);
      if (job.item == item &&
          job.pending &&
          (job.key != previousKey ||
              job.kind != priorKind ||
              job.intentId != previousIntentId ||
              job.state == JobState.running)) {
        throw const AppFailure(FailureKind.conflict);
      }
    }
    final previous = records[previousKey] as Map?;
    if (previous != null) {
      final job = JobRecord.decode(previousKey, previous);
      if (job.kind != priorKind ||
          job.item != item ||
          job.intentId != previousIntentId ||
          job.state == JobState.running) {
        throw const AppFailure(FailureKind.conflict);
      }
      if (job.state == JobState.queued ||
          job.state == JobState.recoveryRequired) {
        final recovery = job.state == JobState.recoveryRequired;
        final now = DateTime.now().toUtc();
        records[previousKey] = {
          ...Map<String, Object?>.from(previous),
          'state': (recovery ? JobState.failed : JobState.cancelled).name,
          'updatedAt': now.toIso8601String(),
          if (recovery) 'failure': FailureKind.recovery.name,
        };
        if (recovery) {
          ActivityRepository.recordIn(
            draft,
            scope,
            previousKey,
            ActivityKind.failed,
            now,
            route: const AppRoute(AppDestination.activity),
          );
        }
      }
    }
    if (records.entries
            .where(
              (entry) => JobRecord.decode(
                entry.key as String,
                entry.value as Map,
              ).pending,
            )
            .length >=
        maxPending) {
      throw const AppFailure(FailureKind.conflict);
    }
    final result = JobRecord(
      key: key,
      kind: kind,
      item: item,
      intentId: intentId,
      state: JobState.queued,
      updatedAt: DateTime.now().toUtc(),
      checkpoint: checkpoint,
    );
    records[key] = result.toJson();
    return result;
  }

  /// Only an unstarted exact operation can be cancelled during admission cleanup.
  /// The original account is retained even when its session has since changed.
  /// Running and terminal history, and any different attempt, remain untouched.
  Future<bool> cancelUnstarted(AccountScope scope, JobRecord job) async {
    var cancelled = false;
    await store.transact((draft) {
      final records =
          ((draft['accounts'] as Map)[scope.key] as Map?)?['jobs'] as Map?;
      final value = records?[job.key] as Map?;
      if (value == null) return;
      final current = JobRecord.decode(job.key, value);
      if (current.kind != job.kind ||
          current.item != job.item ||
          current.intentId != job.intentId ||
          current.state != JobState.queued) {
        return;
      }
      cancelQueuedRetryIn(draft, scope, job);
      cancelled = true;
    });
    if (cancelled) retryChanged();
    return cancelled;
  }

  /// Transaction participant for exact, still-queued admission cleanup.
  void cancelQueuedRetryIn(StoreData draft, AccountScope scope, JobRecord job) {
    final records =
        ((draft['accounts'] as Map)[scope.key] as Map?)?['jobs'] as Map?;
    final value = records?[job.key] as Map?;
    if (value == null) throw const AppFailure(FailureKind.conflict);
    final current = JobRecord.decode(job.key, value);
    if (current.kind != job.kind ||
        current.item != job.item ||
        current.intentId != job.intentId ||
        current.state != JobState.queued) {
      throw const AppFailure(FailureKind.conflict);
    }
    records![job.key] = {
      ...Map<String, Object?>.from(value),
      'state': JobState.cancelled.name,
      'updatedAt': DateTime.now().toUtc().toIso8601String(),
    };
  }

  /// Transaction participant for verified external completion during recovery.
  /// Its caller must bind the observation to the exact durable operation CAS in
  /// this same snapshot. Normal execution terminalizes through the coordinator.
  /// Preserve terminal history and never retire a different/newer attempt.
  bool completeObservedIn(
    StoreData draft,
    AccountScope scope, {
    required String kind,
    required String item,
    required String intentId,
    required int checkpoint,
  }) {
    if (kind.isEmpty || item.isEmpty || intentId.isEmpty || checkpoint < 0) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final records =
        ((draft['accounts'] as Map)[scope.key] as Map?)?['jobs'] as Map?;
    final key = jsonEncode([kind, item, intentId]);
    final value = records?[key] as Map?;
    if (value == null) return false;
    final current = JobRecord.decode(key, value);
    if (current.kind != kind ||
        current.item != item ||
        current.intentId != intentId) {
      throw const AppFailure(FailureKind.conflict);
    }
    if (!current.pending) return false;
    final now = DateTime.now().toUtc();
    records![key] = {
      ...Map<String, Object?>.from(value),
      'state': JobState.completed.name,
      'updatedAt': now.toIso8601String(),
      'checkpoint': checkpoint > current.checkpoint
          ? checkpoint
          : current.checkpoint,
    };
    (records[key] as Map).remove('failure');
    ActivityRepository.recordIn(
      draft,
      scope,
      key,
      ActivityKind.completed,
      now,
      route: const AppRoute(AppDestination.activity),
    );
    return true;
  }

  /// Called only after a transaction containing the job helpers has committed.
  void retryChanged() {
    activity?.changed();
    _changed();
  }
}
