import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/activity/activity_repository.dart';
import 'package:ryhze/features/installations/content_discard_backend.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/managed_content_discard_service.dart';
import 'package:ryhze/features/installations/managed_content_service.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_installation_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

final _id = 'a' * 64;
final _root = '${'1' * 16}:${'2' * 32}';
final _source = '${'1' * 16}:${'3' * 32}';
const _bytes = managedStageChunkBytes + 7;
final _plan = Uint8List.fromList([1, 3, 5, 7]);
Matcher _failure(FailureKind kind) =>
    isA<AppFailure>().having((e) => e.kind, 'kind', kind);

ManagedStageIntent _intent([String? id]) => ManagedStageIntent(
  operationId: id ?? _id,
  item: ProviderItemRef('fixture', 'item', namespace: 'exact'),
  packageId: 'fixture-package',
  version: '1',
  manifestSha256: 'b' * 64,
  sourceRelative: 'fixture.bin',
  expectedSize: _bytes,
  expectedSha256: 'c' * 64,
  slot: 0,
);

// Opaque bytes and state ordering are synthetic here. Native tests separately
// establish plan parsing, exact ownership, verified handles and deletion safety.
class _Backend implements ContentDiscardBackend, ContentStagingBackend {
  @override
  bool discardAvailable = true;
  @override
  String get deviceId => _root;
  ContentStagingState planState = ContentStagingState.staged;
  ContentDiscardState inspectionState = ContentDiscardState.ready;
  ContentDiscardState deletionState = ContentDiscardState.completed;
  Uint8List? plan = _plan;
  int plannedBytes = _bytes, plannedChunks = 2;
  ContentStagingRequest? lastRequest;
  int plans = 0, deletions = 0, inspections = 0, drains = 0;
  int removedBytes = 0, removedFiles = 0;
  bool completed = false;
  Completer<void>? hold;
  final started = Completer<void>();
  Future<void> Function(int)? onDrain;
  void Function()? beforeDelete;
  @override
  Future<ContentDiscardPlanReceipt> planDiscard(
    ContentStagingRequest request,
    CancellationToken token,
  ) async {
    plans++;
    lastRequest = request;
    return ContentDiscardPlanReceipt(
      state: planState,
      plan: plan,
      verifiedBytes: plannedBytes,
      verifiedChunks: plannedChunks,
    );
  }

  @override
  Future<ContentDiscardReceipt> inspectDiscard(
    Uint8List plan,
    CancellationToken token,
  ) async {
    inspections++;
    expect(plan, _plan);
    return ContentDiscardReceipt(
      state: completed ? ContentDiscardState.completed : inspectionState,
      removedBytes: completed ? _bytes : removedBytes,
      removedFiles: completed ? 3 : removedFiles,
    );
  }

  @override
  Future<ContentDiscardReceipt> discard(
    Uint8List plan,
    CancellationToken token,
  ) async {
    expect(plan, _plan);
    beforeDelete?.call();
    deletions++;
    if (!started.isCompleted) started.complete();
    await hold?.future;
    completed = deletionState == ContentDiscardState.completed;
    return ContentDiscardReceipt(
      state: deletionState,
      removedBytes: completed ? _bytes : removedBytes,
      removedFiles: completed ? 3 : removedFiles,
    );
  }

  @override
  Future<void> drain() async {
    await onDrain?.call(++drains);
    if (deletions > 0) await hold?.future;
  }

  @override
  Future<void> cancel() async {}
  @override
  Future<void> close() => drain();
  @override
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken token,
  ) => throw StateError('Staging must not inspect a cleanup record.');
  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken token,
  ) => throw StateError('Staging must not execute a cleanup record.');
}

class _Store extends MemoryTransactionStore {
  @override
  bool get durable => true;
  bool failPrepare = false, failCleanup = false;
  void Function()? afterPrepare;
  @override
  Future<void> transact(void Function(StoreData) change) async {
    var prepared = false;
    await super.transact((draft) {
      final before = _state(draft);
      change(draft);
      prepared =
          before != 'discardPrepared' && _state(draft) == 'discardPrepared';
      if ((failPrepare && prepared) ||
          (failCleanup &&
              before == 'discardPrepared' &&
              _state(draft) != before)) {
        throw const AppFailure(FailureKind.storage);
      }
    });
    if (prepared) {
      final callback = afterPrepare;
      afterPrepare = null;
      callback?.call();
    }
  }

  String? _state(StoreData draft) =>
      ((draft['managedInstallations'] as Map?)?['records']
              as Map?)?[_id]?['state']
          as String?;
}

class _Fixture {
  _Fixture() {
    activity = ActivityRepository(store);
    jobs = JobRepository(store, activity: activity);
    repository = ManagedInstallationRepository(store, session, deviceId: _root);
    staging = ManagedContentService(repository, backend, jobs, session);
    discard = ManagedContentDiscardService(repository, backend, jobs, session);
    coordinator = JobCoordinator(jobs, {
      ManagedContentService.jobKind: staging,
      ManagedContentDiscardService.jobKind: discard,
    });
    session.addListener(coordinator.cancelActive);
  }
  final store = _Store();
  final session = SessionController();
  final backend = _Backend();
  late final ManagedInstallationRepository repository;
  late final ActivityRepository activity;
  late final JobRepository jobs;
  late final ManagedContentService staging;
  late final ManagedContentDiscardService discard;
  late final JobCoordinator coordinator;
  ManagedInstallationRecord get record => repository.findCurrent(_id)!;
  List<JobRecord> get records => jobs.read(session.scope);
  ManagedStageObservation observation(int bytes) => ManagedStageObservation(
    operationId: _id,
    state: bytes == _bytes
        ? ManagedStageObservationState.staged
        : ManagedStageObservationState.checkpoint,
    verifiedBytes: bytes,
    verifiedChunks: bytes == 0 ? 0 : 2,
    rootIdentity: _root,
    sourceIdentity: _source,
  );
  Future<void> seed({JobState? oldState = JobState.completed}) async {
    var lease = await repository.prepare(
      _intent(),
      observation(0),
      CancellationToken(),
    );
    JobRecord? old;
    if (oldState != null) {
      old = await jobs.enqueue(
        session.scope,
        ManagedContentService.jobKind,
        _id,
        intentId: _id,
      );
    }
    if (oldState == JobState.queued || oldState == null) {
      await repository.requireRecovery(lease);
      return;
    }
    await jobs.transition(session.scope, old!, JobState.running);
    lease = await repository.beginStage(lease, CancellationToken());
    lease = await repository.reconcile(
      lease,
      observation(_bytes),
      CancellationToken(),
    );
    if (oldState != JobState.running) {
      await jobs.transition(session.scope, old, oldState);
    }
  }

  Future<ManagedDiscardConfirmation> prepare([CancellationToken? token]) =>
      discard.prepareRecord(_id, token ?? CancellationToken());
  Future<void> confirm(
    ManagedDiscardConfirmation confirmation, [
    CancellationToken? token,
  ]) => discard.discardRecord(
    confirmation,
    token ?? CancellationToken(),
    coordinator: coordinator,
  );
  Future<ManagedDiscardConfirmation?> inspect() =>
      discard.inspectRecord(_id, CancellationToken());
  Future<ManagedDiscardAttempt> admit() async {
    final admission = coordinator.acquireIdle();
    try {
      return await repository.prepareDiscard(
        repository.leaseCurrent(_id)!,
        _plan,
        _bytes,
        2,
        CancellationToken(),
        jobs: jobs,
        checkAdmission: admission.check,
      );
    } finally {
      admission.release();
    }
  }

  Future<void> recover() async {
    await discard.recoverInterrupted();
    discard.resume();
    await coordinator.resume(session.scope);
  }

  Future<void> close() async {
    await coordinator.close();
    await discard.close();
    await staging.close();
    repository.dispose();
    jobs.dispose();
    activity.dispose();
    session.dispose();
    await store.close();
  }
}

void main() {
  test(
    'read-only plan does not change inventory, jobs, Activity or reservation',
    () async {
      final f = _Fixture();
      await f.seed();
      final before = f.store.read();
      final confirmation = await f.prepare();
      expect(confirmation.packageId, 'fixture-package');
      expect(confirmation.version, '1');
      expect(confirmation.verifiedBytes, _bytes);
      expect(confirmation.fileCount, 3);
      expect(f.discard.isCurrent(confirmation), isTrue);
      expect(f.store.read(), before);
      expect(f.backend.deletions, 0);
      f.discard.cancelPending();
      expect(f.discard.isCurrent(confirmation), isFalse);
      expect(f.store.read(), before);
      await f.close();
    },
  );

  test('explicit discard persists plan/job before effect and releases only verified completion', () async {
    final f = _Fixture();
    await f.seed();
    final old = f.records.single;
    final oldActivity = f.activity.read(f.session.scope).single;
    await f.jobs.enqueue(f.session.scope, 'unrelated', 'other');
    final confirmation = await f.prepare();
    f.backend.beforeDelete = () {
      expect(f.record.state, ManagedInstallationState.discarding);
      expect(f.record.reservesContent, isTrue);
      expect(f.record.discard!.plan, _plan);
      expect(f.records.last.state, JobState.running);
    };
    await f.confirm(confirmation);
    expect(f.record.state, ManagedInstallationState.discarded);
    expect(f.record.reservesContent, isFalse);
    expect(f.record.discard!.removedFiles, 3);
    expect(
      f.records.singleWhere((job) => job.key == old.key).state,
      JobState.completed,
    );
    expect(f.records.last.intentId, '$_id.discard.1');
    expect(f.records.last.state, JobState.completed);
    expect(
      f.records.singleWhere((job) => job.kind == 'unrelated').state,
      JobState.queued,
    );
    final preserved = f.activity
        .read(f.session.scope)
        .singleWhere((event) => event.id == oldActivity.id);
    expect(preserved.count, oldActivity.count);
    expect(preserved.occurredAt, oldActivity.occurredAt);
    expect(
      () => f.confirm(confirmation),
      throwsA(_failure(FailureKind.conflict)),
    );
    await f.repository.prepare(
      _intent('d' * 64),
      ManagedStageObservation(
        operationId: 'd' * 64,
        state: ManagedStageObservationState.checkpoint,
        verifiedBytes: 0,
        verifiedChunks: 0,
        rootIdentity: _root,
        sourceIdentity: _source,
      ),
      CancellationToken(),
    );
    expect(f.backend.deletions, 1);
    await f.close();
  });

  for (final oldState in <JobState?>[
    JobState.recoveryRequired,
    JobState.queued,
    null,
  ]) {
    test(
      'discard atomically settles exact ${oldState?.name ?? 'absent'} staging orphan',
      () async {
        final f = _Fixture();
        await f.seed(oldState: oldState);
        await f.confirm(await f.prepare());
        expect(f.records.last.state, JobState.completed);
        if (oldState != null) {
          expect(
            f.records.first.state,
            oldState == JobState.recoveryRequired
                ? JobState.failed
                : JobState.cancelled,
          );
        }
        expect(f.backend.deletions, 1);
        await f.close();
      },
    );
  }

  test('running staging job cannot be retired for discard', () async {
    final f = _Fixture();
    await f.seed(oldState: JobState.running);
    await expectLater(
      f.confirm(await f.prepare()),
      throwsA(_failure(FailureKind.conflict)),
    );
    expect(f.record.state, ManagedInstallationState.staged);
    expect(f.records.single.state, JobState.running);
    expect(f.backend.deletions, 0);
    await f.close();
  });

  for (final state in [
    ContentStagingState.recovery,
    ContentStagingState.conflict,
    ContentStagingState.unavailable,
  ]) {
    test('${state.name} native plan cannot issue confirmation', () async {
      final f = _Fixture();
      await f.seed();
      f.backend.planState = state;
      await expectLater(f.prepare(), throwsA(_failure(FailureKind.recovery)));
      expect(f.backend.deletions, 0);
      expect(f.record.discard, isNull);
      await f.close();
    });
  }

  test('repository change and session generation invalidate a planned confirmation', () async {
    final f = _Fixture();
    await f.seed();
    final first = await f.prepare();
    await f.repository.reconcile(
      f.repository.leaseCurrent(_id)!,
      f.observation(_bytes),
      CancellationToken(),
    );
    expect(f.discard.isCurrent(first), isFalse);
    expect(() => f.confirm(first), throwsA(_failure(FailureKind.conflict)));
    final second = await f.prepare();
    f.session.replace(f.session.scope);
    expect(f.discard.isCurrent(second), isFalse);
    expect(() => f.confirm(second), throwsA(_failure(FailureKind.conflict)));
    expect(f.backend.deletions, 0);
    await f.close();
  });

  test(
    'changed or corrupt native plan inspection blocks before durable admission',
    () async {
      final f = _Fixture();
      await f.seed();
      final c = await f.prepare();
      final before = f.store.read();
      f.backend.inspectionState = ContentDiscardState.recovery;
      await expectLater(f.confirm(c), throwsA(_failure(FailureKind.recovery)));
      expect(f.store.read(), before);
      expect(f.backend.deletions, 0);
      await f.close();
    },
  );

  test(
    'plan write failure is atomic and cannot delete or silently retry',
    () async {
      final f = _Fixture();
      await f.seed();
      final c = await f.prepare();
      final before = f.store.read();
      f.store.failPrepare = true;
      await expectLater(f.confirm(c), throwsA(_failure(FailureKind.storage)));
      expect(f.store.read(), before);
      expect(f.backend.deletions, 0);
      f.store.failPrepare = false;
      await f.coordinator.drain(f.session.scope);
      expect(f.backend.deletions, 0);
      await f.recover();
      await f.confirm(await f.prepare());
      expect(f.backend.deletions, 1);
      await f.close();
    },
  );

  for (final replace in [false, true]) {
    test(
      'post-commit ${replace ? 'account replacement' : 'cancellation'} cancels unstarted discard',
      () async {
        final f = _Fixture();
        await f.seed();
        final c = await f.prepare();
        final scope = f.session.scope;
        final token = CancellationToken();
        f.store.afterPrepare = () {
          if (replace) {
            f.session.replace(const AccountScope.verified('fixture', 'other'));
          } else {
            token.cancel();
          }
        };
        await expectLater(
          f.confirm(c, token),
          throwsA(_failure(FailureKind.cancelled)),
        );
        expect(f.jobs.read(scope).last.state, JobState.cancelled);
        if (replace) {
          expect(f.repository.readCurrent(), isEmpty);
          f.session.replace(scope);
        }
        expect(
          f.record.state,
          ManagedInstallationState.discardRecoveryRequired,
        );
        expect(f.record.reservesContent, isTrue);
        expect(f.backend.deletions, 0);
        await f.close();
      },
    );
  }

  test(
    'failed abort retains durable intent and closes admission until recovery',
    () async {
      final f = _Fixture();
      await f.seed();
      final c = await f.prepare();
      final token = CancellationToken();
      f.store.afterPrepare = () {
        f.store.failCleanup = true;
        token.cancel();
      };
      await expectLater(
        f.confirm(c, token),
        throwsA(_failure(FailureKind.storage)),
      );
      expect(f.record.state, ManagedInstallationState.discardPrepared);
      expect(f.records.last.state, JobState.queued);
      f.store.failCleanup = false;
      await f.coordinator.drain(f.session.scope);
      expect(f.backend.deletions, 0);
      final shutdown = await f.coordinator.prepareShutdown();
      await f.discard.recoverInterrupted();
      f.discard.resume();
      await shutdown.release(f.session.scope);
      await f.coordinator.drain(f.session.scope);
      expect(f.backend.deletions, 0);
      expect(f.record.state, ManagedInstallationState.discardRecoveryRequired);
      expect(f.records.last.state, JobState.recoveryRequired);
      await f.close();
    },
  );

  test(
    'duplicate and cancelled deletion stay busy until actual settlement',
    () async {
      final f = _Fixture();
      await f.seed();
      final c = await f.prepare();
      f.backend.hold = Completer<void>();
      final token = CancellationToken();
      final run = f.confirm(c, token);
      final expected = expectLater(
        run,
        throwsA(_failure(FailureKind.cancelled)),
      );
      await f.backend.started.future;
      expect(() => f.confirm(c), throwsA(_failure(FailureKind.conflict)));
      token.cancel();
      var settled = false;
      unawaited(
        run.then(
          (_) {
            settled = true;
          },
          onError: (Object _) {
            settled = true;
          },
        ),
      );
      await Future<void>.delayed(Duration.zero);
      expect(settled, isFalse);
      f.backend.hold!.complete();
      await expected;
      expect(f.record.state, ManagedInstallationState.discardRecoveryRequired);
      expect(f.record.reservesContent, isTrue);
      final result = await f.inspect();
      expect(result, isNull);
      expect(f.record.state, ManagedInstallationState.discarded);
      expect(f.backend.deletions, 1);
      await f.close();
    },
  );

  test('partial removal needs fresh ready inspection and explicit new attempt', () async {
    final f = _Fixture();
    await f.seed();
    f.backend.deletionState = ContentDiscardState.cancelled;
    f.backend.removedBytes = managedStageChunkBytes;
    f.backend.removedFiles = 1;
    // Plan/confirm initially observes all files, then fake removal reports one.
    final c = await f.prepare();
    f.backend.removedBytes = 0;
    f.backend.removedFiles = 0;
    f.backend.beforeDelete = () {
      f.backend.removedBytes = managedStageChunkBytes;
      f.backend.removedFiles = 1;
    };
    await expectLater(f.confirm(c), throwsA(_failure(FailureKind.recovery)));
    expect(f.record.discard!.removedFiles, 1);
    expect(f.record.reservesContent, isTrue);
    final resumed = (await f.inspect())!;
    expect(resumed.recovery, isTrue);
    expect(resumed.remainingBytes, 7);
    expect(resumed.remainingFileCount, 2);
    f.backend.deletionState = ContentDiscardState.completed;
    await f.discard.resumeDiscardRecord(
      resumed,
      CancellationToken(),
      coordinator: f.coordinator,
    );
    expect(f.record.discard!.attempt, 2);
    expect(f.record.state, ManagedInstallationState.discarded);
    expect(f.records.map((j) => j.state), [
      JobState.completed,
      JobState.failed,
      JobState.completed,
    ]);
    expect(
      f.activity
          .read(f.session.scope)
          .any((a) => a.kind == ActivityKind.recovery),
      isTrue,
    );
    await f.close();
  });

  test(
    'recovery inspection refuses corrupt remaining plan and keeps reservation',
    () async {
      final f = _Fixture();
      await f.seed();
      await f.admit();
      await f.repository.recoverInterrupted();
      f.backend.inspectionState = ContentDiscardState.recovery;
      expect(await f.inspect(), isNull);
      expect(f.record.state, ManagedInstallationState.discardRecoveryRequired);
      expect(f.record.reservesContent, isTrue);
      expect(f.backend.deletions, 0);
      await f.close();
    },
  );

  test('restart only inspects and ordinary queue drain cannot replay prepared deletion', () async {
    final f = _Fixture();
    await f.seed();
    final attempt = await f.admit();
    await f.discard.recoverInterrupted();
    expect(f.record.state, ManagedInstallationState.discardRecoveryRequired);
    expect(f.backend.inspections, 1);
    expect(f.backend.deletions, 0);
    await f.coordinator.drain(f.session.scope);
    expect(f.records.last.state, JobState.recoveryRequired);
    expect(f.backend.deletions, 0);
    await expectLater(
      f.staging.reconcile(f.records.first, CancellationToken()),
      throwsA(_failure(FailureKind.conflict)),
    );
    final c = (await f.inspect())!;
    await f.confirm(c);
    await expectLater(
      f.discard.reconcile(attempt.job, CancellationToken()),
      throwsA(_failure(FailureKind.conflict)),
    );
    expect(f.backend.deletions, 1);
    await f.close();
  });

  test(
    'admission drain failure releases hold while keeping queue closed',
    () async {
      final f = _Fixture();
      await f.seed();
      final c = await f.prepare();
      f.backend.onDrain = (_) async {
        throw const AppFailure(FailureKind.recovery);
      };
      await expectLater(f.confirm(c), throwsA(_failure(FailureKind.recovery)));
      final shutdown = await f.coordinator.prepareShutdown();
      f.backend.onDrain = null;
      await f.discard.recoverInterrupted();
      f.discard.resume();
      await shutdown.release(f.session.scope);
      expect(f.backend.deletions, 0);
      await f.confirm(await f.prepare());
      expect(f.backend.deletions, 1);
      await f.close();
    },
  );

  for (final cancel in [true, false]) {
    test(
      'final plan drain ${cancel ? 'cancellation' : 'failure'} cannot leave confirmation usable',
      () async {
        final f = _Fixture();
        await f.seed();
        final entered = Completer<void>(), release = Completer<void>();
        f.backend.onDrain = (count) async {
          if (count == 2) {
            entered.complete();
            await release.future;
            if (!cancel) throw const AppFailure(FailureKind.recovery);
          }
        };
        final token = CancellationToken();
        final plan = f.prepare(token);
        final failure = cancel
            ? null
            : expectLater(plan, throwsA(_failure(FailureKind.recovery)));
        await entered.future;
        if (cancel) token.cancel();
        release.complete();
        if (failure != null) {
          await failure;
        } else {
          expect(f.discard.isCurrent(await plan), isFalse);
        }
        expect(f.backend.deletions, 0);
        await f.close();
      },
    );
  }

  test('discard state survives roundtrip, preserves extensions, and rejects stage mutation', () async {
    final f = _Fixture();
    await f.seed();
    await f.store.transact((draft) {
      final record =
          ((draft['managedInstallations'] as Map)['records'] as Map)[_id]
              as Map;
      record['futureExtension'] = {'preserved': true};
    });
    final admitted = await f.admit();
    await f.store.transact((draft) {
      final record =
          ((draft['managedInstallations'] as Map)['records'] as Map)[_id]
              as Map;
      (record['discard'] as Map)['futureExtension'] = 'retained';
    });
    await f.repository.recoverInterrupted();
    final snapshot = f.store.read();
    final copy = _Store();
    await copy.transact((draft) {
      draft.clear();
      draft.addAll(cloneData(snapshot));
    });
    final repository = ManagedInstallationRepository(
      copy,
      f.session,
      deviceId: _root,
    );
    final reopened = repository.findCurrent(_id)!;
    expect(reopened.discard!.plan, _plan);
    expect(reopened.discardAttemptId, admitted.job.intentId);
    expect(() => reopened.discard!.plan[0] = 99, throwsUnsupportedError);
    await expectLater(
      repository.beginStage(repository.leaseCurrent(_id)!, CancellationToken()),
      throwsA(_failure(FailureKind.recovery)),
    );
    await expectLater(
      repository.reconcile(
        repository.leaseCurrent(_id)!,
        f.observation(_bytes),
        CancellationToken(),
      ),
      throwsA(_failure(FailureKind.conflict)),
    );
    final raw =
        ((copy.read()['managedInstallations'] as Map)['records'] as Map)[_id]
            as Map;
    expect(raw['futureExtension'], {'preserved': true});
    expect((raw['discard'] as Map)['futureExtension'], 'retained');
    repository.dispose();
    await copy.close();
    await f.close();
  });

  test(
    'complete receipt with incomplete counts cannot release a reservation',
    () async {
      final f = _Fixture();
      await f.seed();
      final a = await f.admit();
      final lease = await f.repository.beginDiscard(
        a.lease,
        a.job.intentId!,
        CancellationToken(),
      );
      await expectLater(
        f.repository.reconcileDiscard(
          lease,
          const ContentDiscardReceipt(
            state: ContentDiscardState.completed,
            removedBytes: 0,
            removedFiles: 0,
          ),
          CancellationToken(),
        ),
        throwsA(_failure(FailureKind.invalidData)),
      );
      expect(f.record.reservesContent, isTrue);
      await f.close();
    },
  );

  test(
    'opaque plan bounds and whole envelope quota reject before commit',
    () async {
      final f = _Fixture();
      await f.seed();
      final before = f.store.read();
      final admission = f.coordinator.acquireIdle();
      await expectLater(
        f.repository.prepareDiscard(
          f.repository.leaseCurrent(_id)!,
          Uint8List(contentDiscardMaximumPlanBytes + 1),
          _bytes,
          2,
          CancellationToken(),
          jobs: f.jobs,
          checkAdmission: admission.check,
        ),
        throwsA(_failure(FailureKind.invalidData)),
      );
      expect(f.store.read(), before);
      await f.store.transact((draft) {
        (draft['managedInstallations'] as Map)['preserved'] =
            'x' * (400 * 1024);
      });
      final largeBefore = f.store.read();
      await expectLater(
        f.repository.prepareDiscard(
          f.repository.leaseCurrent(_id)!,
          Uint8List(contentDiscardMaximumPlanBytes),
          _bytes,
          2,
          CancellationToken(),
          jobs: f.jobs,
          checkAdmission: admission.check,
        ),
        throwsA(_failure(FailureKind.invalidData)),
      );
      expect(f.store.read(), largeBefore);
      admission.release();
      expect(f.backend.deletions, 0);
      await f.close();
    },
  );

  test('schema rejects missing plan, malformed base64 and mismatched terminal counts', () async {
    final f = _Fixture();
    await f.seed();
    await f.admit();
    final good = f.store.read()['managedInstallations'] as Map;
    for (final change in <void Function(Map)>[
      (r) => r.remove('discard'),
      (r) => (r['discard'] as Map)['plan'] = 'not-base64!',
      (r) => (r['discard'] as Map)['plan'] = base64Encode(
        Uint8List(contentDiscardMaximumPlanBytes + 1),
      ),
      (r) => r['state'] = 'discarded',
      (r) => (r['discard'] as Map)['attempt'] = 1000,
    ]) {
      final copy = jsonDecode(jsonEncode(good)) as Map;
      change((copy['records'] as Map)[_id] as Map);
      expect(
        () => validateManagedInstallations(copy),
        throwsA(_failure(FailureKind.invalidData)),
      );
    }
    await f.close();
  });

  for (final state in [
    JobState.queued,
    JobState.running,
    JobState.recoveryRequired,
  ]) {
    test(
      'restart settles exact $state job after inventory commit without another deletion',
      () async {
        final f = _Fixture();
        await f.seed();
        final attempt = await f.admit();
        if (state != JobState.queued) {
          await f.jobs.transition(
            f.session.scope,
            attempt.job,
            JobState.running,
          );
          if (state == JobState.recoveryRequired) {
            await f.jobs.transition(f.session.scope, attempt.job, state);
          }
        }
        final lease = await f.repository.beginDiscard(
          attempt.lease,
          attempt.job.intentId!,
          CancellationToken(),
        );
        await f.repository.reconcileDiscard(
          lease,
          const ContentDiscardReceipt(
            state: ContentDiscardState.completed,
            removedBytes: _bytes,
            removedFiles: 3,
          ),
          CancellationToken(),
        );
        expect(f.records.last.state, state);
        expect(f.record.state, ManagedInstallationState.discarded);
        final oldActivity = f.activity
            .read(f.session.scope)
            .where((a) => a.kind == ActivityKind.recovery)
            .toList();
        await f.discard.recoverInterrupted();
        expect(f.records.last.state, JobState.completed);
        expect(f.records.last.checkpoint, _bytes);
        final events = f.activity.read(f.session.scope);
        expect(
          events.where((a) => a.kind == ActivityKind.completed),
          hasLength(2),
        );
        for (final event in oldActivity) {
          expect(events.any((a) => a.id == event.id), isTrue);
        }
        final first = f.store.read();
        await f.discard.recoverInterrupted();
        expect(f.store.read(), first);
        expect(f.backend.deletions, 0);
        expect(f.backend.inspections, 0);
        await f.close();
      },
    );
  }

  test('native inspection completion commits inventory and exact queue result together', () async {
    final f = _Fixture();
    await f.seed();
    final attempt = await f.admit();
    await f.repository.recoverInterrupted();
    await f.jobs.enqueue(f.session.scope, 'unrelated', 'other');
    f.backend.completed = true;
    expect(await f.inspect(), isNull);
    expect(f.record.state, ManagedInstallationState.discarded);
    expect(
      f.records.singleWhere((j) => j.key == attempt.job.key).state,
      JobState.completed,
    );
    expect(f.records.last.state, JobState.queued);
    expect(f.backend.deletions, 0);
    await f.close();
  });

  test(
    'shorter planning receipt cannot weaken the retained prefix floor',
    () async {
      final f = _Fixture();
      await f.seed();
      f.backend
        ..planState = ContentStagingState.checkpoint
        ..plannedBytes = managedStageChunkBytes
        ..plannedChunks = 1;
      await expectLater(
        f.prepare(),
        throwsA(_failure(FailureKind.recovery)),
      );
      expect(f.backend.lastRequest!.minimumVerifiedBytes, _bytes);
      final admission = f.coordinator.acquireIdle();
      try {
        await expectLater(
          f.repository.prepareDiscard(
            f.repository.leaseCurrent(_id)!,
            _plan,
            managedStageChunkBytes,
            1,
            CancellationToken(),
            jobs: f.jobs,
            checkAdmission: admission.check,
          ),
          throwsA(_failure(FailureKind.recovery)),
        );
      } finally {
        admission.release();
      }
      expect(f.record.discard, isNull);
      expect(f.record.reservesContent, isTrue);
      expect(f.backend.deletions, 0);
      await f.close();
    },
  );

  test(
    'source metadata drift after planning fails exact CAS before deletion',
    () async {
      final f = _Fixture();
      await f.seed();
      final confirmation = await f.prepare();
      await f.store.transact((draft) {
        final raw =
            ((draft['managedInstallations'] as Map)['records'] as Map)[_id]
                as Map;
        raw['sourceIdentity'] = '${'1' * 16}:${'8' * 32}';
      });
      await expectLater(
        f.confirm(confirmation),
        throwsA(_failure(FailureKind.conflict)),
      );
      expect(f.record.discard, isNull);
      expect(f.backend.deletions, 0);
      await f.close();
    },
  );

  test(
    'opaque plan drift cannot reconcile a stale receipt or release reservation',
    () async {
      final f = _Fixture();
      await f.seed();
      final attempt = await f.admit();
      final lease = await f.repository.requireDiscardRecovery(attempt.lease);
      await f.store.transact((draft) {
        final raw =
            ((draft['managedInstallations'] as Map)['records'] as Map)[_id]
                as Map;
        (raw['discard'] as Map)['plan'] = base64Encode([2, 4, 6]);
      });
      await expectLater(
        f.repository.reconcileDiscard(
          lease,
          const ContentDiscardReceipt(
            state: ContentDiscardState.completed,
            removedBytes: _bytes,
            removedFiles: 3,
          ),
          CancellationToken(),
          observedJobs: f.jobs,
        ),
        throwsA(_failure(FailureKind.conflict)),
      );
      expect(f.record.reservesContent, isTrue);
      expect(f.records.last.state, JobState.queued);
      await f.close();
    },
  );
}
