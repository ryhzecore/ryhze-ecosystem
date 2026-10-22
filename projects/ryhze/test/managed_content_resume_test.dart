import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/activity/activity_repository.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/managed_content_service.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_installation_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

const _prefix = managedStageChunkBytes;
final _operation = 'a' * 64;
final _root = '${'1' * 16}:${'2' * 32}';
final _source = '${'1' * 16}:${'3' * 32}';

ManagedStageIntent _intent() => ManagedStageIntent(
  operationId: _operation,
  item: ProviderItemRef('fixture', 'game', namespace: 'exact'),
  packageId: 'fixture-package',
  version: '1',
  manifestSha256: 'b' * 64,
  sourceRelative: 'fixture.bin',
  expectedSize: 600000,
  expectedSha256: 'c' * 64,
  slot: 0,
);

Matcher _failure(FailureKind kind) =>
    isA<AppFailure>().having((error) => error.kind, 'kind', kind);

// Only ordering/cancellation is simulated. Native tests separately verify real
// pinned file identities, retained prefix integrity and write-boundary floors.
class _Backend implements ContentStagingBackend {
  @override
  String get deviceId => _root;
  String sourceIdentity = _source;
  int bytes = _prefix, stages = 0, drains = 0;
  ContentStagingState inspectionState = ContentStagingState.checkpoint;
  ContentStagingState stageState = ContentStagingState.staged;
  Completer<void>? stageHold;
  final started = Completer<void>();
  Future<void> Function(int)? onDrain;
  final List<ContentStagingRequest> requests = [];

  ContentStagingReceipt receipt(ContentStagingState state, int verified) =>
      ContentStagingReceipt(
        state: state,
        verifiedBytes: verified,
        verifiedChunks: (verified + _prefix - 1) ~/ _prefix,
        rootIdentity: _root,
        sourceIdentity: sourceIdentity,
      );

  @override
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async => receipt(inspectionState, bytes);

  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    stages++;
    requests.add(request);
    if (!started.isCompleted) started.complete();
    await stageHold?.future;
    return receipt(
      stageState,
      stageState == ContentStagingState.staged ? request.size : bytes,
    );
  }

  @override
  Future<void> cancel() async {}
  @override
  Future<void> drain() async {
    await onDrain?.call(++drains);
    if (stages > 0) await stageHold?.future;
  }

  @override
  Future<void> close() => drain();
}

class _Store extends MemoryTransactionStore {
  @override
  bool get durable => true;
  bool failRetry = false, failAbort = false;
  void Function()? afterRetry;
  @override
  Future<void> transact(void Function(StoreData) change) async {
    var retry = false;
    await super.transact((draft) {
      final before = _state(draft);
      change(draft);
      retry = before != 'resumePrepared' && _state(draft) == 'resumePrepared';
      if ((failRetry && retry) ||
          (failAbort &&
              before == 'resumePrepared' &&
              _state(draft) != before)) {
        throw const AppFailure(FailureKind.storage);
      }
    });
    if (retry) {
      final callback = afterRetry;
      afterRetry = null;
      callback?.call();
    }
  }

  String? _state(StoreData data) =>
      ((data['managedInstallations'] as Map?)?['records']
              as Map?)?[_operation]?['state']
          as String?;
}

class _Fixture {
  _Fixture() {
    activity = ActivityRepository(store);
    jobs = JobRepository(store, activity: activity);
    repository = ManagedInstallationRepository(store, session, deviceId: _root);
    service = ManagedContentService(repository, backend, jobs, session);
    coordinator = JobCoordinator(jobs, {
      ManagedContentService.jobKind: service,
    });
    session.addListener(coordinator.cancelActive);
  }
  final store = _Store();
  final session = SessionController();
  final backend = _Backend();
  late final ActivityRepository activity;
  late final JobRepository jobs;
  late final ManagedInstallationRepository repository;
  late final ManagedContentService service;
  late final JobCoordinator coordinator;
  ManagedInstallationRecord get record => repository.findCurrent(_operation)!;
  List<JobRecord> get records => jobs.read(session.scope);

  ManagedStageObservation observation(
    int bytes, {
    ManagedStageObservationState state =
        ManagedStageObservationState.checkpoint,
  }) => ManagedStageObservation(
    operationId: _operation,
    state: state,
    verifiedBytes: bytes,
    verifiedChunks: (bytes + _prefix - 1) ~/ _prefix,
    rootIdentity: _root,
    sourceIdentity: _source,
  );

  Future<void> seed({JobState? oldState = JobState.recoveryRequired}) async {
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
        _operation,
        intentId: _operation,
      );
    }
    if (oldState == JobState.queued || oldState == null) {
      backend.bytes = 0;
      await repository.requireRecovery(lease);
      return;
    }
    await jobs.transition(session.scope, old!, JobState.running);
    lease = await repository.beginStage(lease, CancellationToken());
    await repository.reconcile(
      lease,
      observation(_prefix),
      CancellationToken(),
    );
    if (oldState == JobState.recoveryRequired) {
      await jobs.transition(session.scope, old, JobState.recoveryRequired);
    }
  }

  Future<void> inspect([CancellationToken? token]) =>
      service.inspectRecord(_operation, token ?? CancellationToken());
  Future<void> resume([CancellationToken? token]) => service.resumeRecord(
    _operation,
    token ?? CancellationToken(),
    coordinator: coordinator,
  );
  Future<void> recover() async {
    await service.recoverInterrupted();
    service.resume();
    await coordinator.resume(session.scope);
  }

  Future<void> close() async {
    await coordinator.close();
    await service.close();
    repository.dispose();
    jobs.dispose();
    activity.dispose();
    session.dispose();
    await store.close();
  }
}

void main() {
  test(
    'explicit prefix retry preserves history and completes only selected job',
    () async {
      final f = _Fixture();
      await f.seed();
      final old = f.records.single;
      final oldActivity = f.activity.read(f.session.scope).single;
      await f.jobs.enqueue(f.session.scope, 'unrelated', 'other');
      expect(() => f.resume(), throwsA(_failure(FailureKind.recovery)));
      await f.inspect();
      expect(f.service.canResume(_operation), isTrue);
      await f.resume();
      expect(f.record.state, ManagedInstallationState.staged);
      expect(f.record.attempt, 1);
      expect(f.record.resumeFloorBytes, 600000);
      expect(f.backend.requests.single.operationId, _operation);
      expect(f.backend.requests.single.minimumVerifiedBytes, _prefix);
      expect(
        f.records.singleWhere((job) => job.key == old.key).state,
        JobState.failed,
      );
      expect(
        f.records
            .singleWhere((job) => job.intentId == '$_operation.retry.1')
            .state,
        JobState.completed,
      );
      expect(
        f.records.singleWhere((job) => job.kind == 'unrelated').state,
        JobState.queued,
      );
      final preserved = f.activity
          .read(f.session.scope)
          .singleWhere((item) => item.id == oldActivity.id);
      expect(preserved.kind, ActivityKind.recovery);
      expect(preserved.occurredAt, oldActivity.occurredAt);
      expect(preserved.count, oldActivity.count);
      final idempotent = await f.jobs.enqueue(
        f.session.scope,
        ManagedContentService.jobKind,
        _operation,
        intentId: _operation,
      );
      expect(idempotent.key, old.key);
      expect(idempotent.state, JobState.failed);
      await f.close();
    },
  );

  for (final oldState in <JobState?>[JobState.queued, null]) {
    test(
      'explicit retry handles ${oldState?.name ?? 'absent'} original crash orphan',
      () async {
        final f = _Fixture();
        await f.seed(oldState: oldState);
        await f.inspect();
        await f.resume();
        expect(f.backend.stages, 1);
        expect(f.record.attempt, 1);
        if (oldState != null) {
          expect(f.records.first.state, JobState.cancelled);
        }
        expect(f.records.last.state, JobState.completed);
        await f.close();
      },
    );
  }

  test(
    'running prior attempt and ordinary competing admission remain blocked',
    () async {
      final f = _Fixture();
      await f.seed(oldState: JobState.running);
      await f.inspect();
      await expectLater(f.resume(), throwsA(_failure(FailureKind.conflict)));
      await expectLater(
        f.jobs.enqueue(
          f.session.scope,
          ManagedContentService.jobKind,
          _operation,
          intentId: 'competing',
        ),
        throwsA(_failure(FailureKind.conflict)),
      );
      expect(f.backend.stages, 0);
      expect(f.record.attempt, 0);
      expect(f.records.single.state, JobState.running);
      await f.close();
    },
  );

  test('duplicate action cannot start a second worker; cancellation awaits settlement', () async {
    final f = _Fixture();
    await f.seed();
    await f.inspect();
    f.backend.stageHold = Completer<void>();
    final token = CancellationToken();
    final running = f.resume(token);
    final expected = expectLater(
      running,
      throwsA(_failure(FailureKind.cancelled)),
    );
    await f.backend.started.future;
    expect(() => f.resume(), throwsA(_failure(FailureKind.conflict)));
    token.cancel();
    var settled = false;
    unawaited(
      running.then(
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
    f.backend.stageHold!.complete();
    await expected;
    expect(f.backend.stages, 1);
    expect(f.record.state, ManagedInstallationState.recoveryRequired);
    expect(f.record.reservesContent, isTrue);
    expect(f.records.last.state, JobState.recoveryRequired);
    expect(f.service.canResume(_operation), isFalse);
    await f.close();
  });

  test(
    'repeated failed attempts retain distinct queue and activity history',
    () async {
      final f = _Fixture();
      await f.seed();
      f.backend.stageState = ContentStagingState.quota;
      for (var attempt = 1; attempt <= 2; attempt++) {
        await f.inspect();
        await expectLater(f.resume(), throwsA(_failure(FailureKind.recovery)));
        expect(f.record.attempt, attempt);
        expect(f.record.state, ManagedInstallationState.recoveryRequired);
        expect(f.records.last.intentId, '$_operation.retry.$attempt');
        expect(f.records.last.state, JobState.recoveryRequired);
      }
      expect(f.backend.stages, 2);
      expect(f.records.map((job) => job.state), [
        JobState.failed,
        JobState.failed,
        JobState.recoveryRequired,
      ]);
      expect(
        f.activity
            .read(f.session.scope)
            .where((event) => event.kind == ActivityKind.recovery),
        hasLength(3),
      );
      await f.close();
    },
  );

  for (final bad in [
    ContentStagingState.recovery,
    ContentStagingState.unavailable,
  ]) {
    test('${bad.name} observation cannot authorize retry', () async {
      final f = _Fixture();
      await f.seed();
      f.backend.inspectionState = bad;
      await f.inspect();
      expect(f.service.canResume(_operation), isFalse);
      expect(() => f.resume(), throwsA(_failure(FailureKind.recovery)));
      expect(f.backend.stages, 0);
      await f.close();
    });
  }

  test(
    'fresh source replacement after inspection cannot start retry',
    () async {
      final f = _Fixture();
      await f.seed();
      await f.inspect();
      f.backend.sourceIdentity = '${'1' * 16}:${'4' * 32}';
      await expectLater(f.resume(), throwsA(_failure(FailureKind.recovery)));
      expect(f.record.verifiedBytes, 0);
      expect(f.record.resumeFloorBytes, _prefix);
      expect(f.backend.stages, 0);
      expect(f.record.attempt, 0);
      await f.close();
    },
  );

  test('error receipt retains a floor that repeated shorter inspections cannot lower', () async {
    final f = _Fixture();
    await f.seed(oldState: null);
    await f.repository.reconcile(
      f.repository.leaseCurrent(_operation)!,
      f.observation(_prefix, state: ManagedStageObservationState.quota),
      CancellationToken(),
    );
    expect(f.record.resumeFloorBytes, _prefix);
    f.backend.bytes = 0;
    for (var i = 0; i < 2; i++) {
      await f.inspect();
      expect(f.record.verifiedBytes, 0);
      expect(f.record.resumeFloorBytes, _prefix);
      expect(f.service.canResume(_operation), isFalse);
    }
    expect(f.backend.stages, 0);
    await f.close();
  });

  test(
    'retry write failure leaves old inventory and queue atomically unchanged',
    () async {
      final f = _Fixture();
      await f.seed();
      await f.inspect();
      final before = f.store.read();
      f.store.failRetry = true;
      await expectLater(f.resume(), throwsA(_failure(FailureKind.storage)));
      expect(f.store.read(), before);
      expect(f.backend.stages, 0);
      f.store.failRetry = false;
      await f.recover();
      await f.inspect();
      await f.resume();
      expect(f.backend.stages, 1);
      await f.close();
    },
  );

  for (final replaceAccount in [false, true]) {
    test(
      'post-commit ${replaceAccount ? 'account replacement' : 'cancellation'} retires unstarted retry',
      () async {
        final f = _Fixture();
        await f.seed();
        await f.inspect();
        final oldScope = f.session.scope;
        final token = CancellationToken();
        f.store.afterRetry = () {
          if (replaceAccount) {
            f.session.replace(const AccountScope.verified('fixture', 'other'));
          } else {
            token.cancel();
          }
        };
        await expectLater(
          f.resume(token),
          throwsA(_failure(FailureKind.cancelled)),
        );
        expect(f.backend.stages, 0);
        expect(f.jobs.read(oldScope).last.state, JobState.cancelled);
        if (replaceAccount) {
          expect(f.repository.readCurrent(), isEmpty);
          f.session.replace(oldScope);
        }
        expect(f.record.attempt, 1);
        expect(f.record.state, ManagedInstallationState.recoveryRequired);
        expect(f.service.canResume(_operation), isFalse);
        await f.close();
      },
    );
  }

  test(
    'failed post-commit cleanup keeps queue suspended until metadata recovery',
    () async {
      final f = _Fixture();
      await f.seed();
      await f.inspect();
      final token = CancellationToken();
      f.store.afterRetry = () {
        f.store.failAbort = true;
        token.cancel();
      };
      await expectLater(
        f.resume(token),
        throwsA(_failure(FailureKind.storage)),
      );
      expect(f.record.state, ManagedInstallationState.resumePrepared);
      expect(f.records.last.state, JobState.queued);
      f.store.failAbort = false;
      await f.coordinator.drain(f.session.scope);
      expect(f.backend.stages, 0);
      // No leaked anonymous admission hold prevents explicit shutdown/recovery.
      final shutdown = await f.coordinator.prepareShutdown();
      await f.service.recoverInterrupted();
      f.service.resume();
      await shutdown.release(f.session.scope);
      await f.coordinator.drain(f.session.scope);
      expect(f.backend.stages, 0);
      expect(f.record.state, ManagedInstallationState.recoveryRequired);
      expect(f.records.last.state, JobState.recoveryRequired);
      await f.close();
    },
  );

  for (final cancelInspection in [true, false]) {
    test(
      'final inspection drain ${cancelInspection ? 'cancellation' : 'failure'} discards ticket',
      () async {
        final f = _Fixture();
        await f.seed();
        final entered = Completer<void>();
        final release = Completer<void>();
        f.backend.onDrain = (count) async {
          if (count == 2) {
            entered.complete();
            await release.future;
            if (!cancelInspection) throw const AppFailure(FailureKind.recovery);
          }
        };
        final token = CancellationToken();
        final inspecting = f.inspect(token);
        final expected = cancelInspection
            ? null
            : expectLater(inspecting, throwsA(_failure(FailureKind.recovery)));
        await entered.future;
        if (cancelInspection) token.cancel();
        release.complete();
        if (expected != null) {
          await expected;
        } else {
          await inspecting;
        }
        expect(f.service.canResume(_operation), isFalse);
        expect(f.backend.stages, 0);
        await f.close();
      },
    );
  }

  test('admission drain failure releases its hold but requires successful owner recovery', () async {
    final f = _Fixture();
    await f.seed();
    await f.inspect();
    f.backend.onDrain = (_) async {
      throw const AppFailure(FailureKind.recovery);
    };
    await expectLater(f.resume(), throwsA(_failure(FailureKind.recovery)));
    expect(f.backend.stages, 0);
    final shutdown = await f.coordinator.prepareShutdown();
    f.backend.onDrain = null;
    await f.service.recoverInterrupted();
    f.service.resume();
    await shutdown.release(f.session.scope);
    await f.inspect();
    await f.resume();
    expect(f.backend.stages, 1);
    await f.close();
  });

  test('restart recovery never replays admitted retry and invalidates old job identity', () async {
    final f = _Fixture();
    await f.seed();
    final oldJob = f.records.single;
    final admission = f.coordinator.acquireIdle();
    final attempt = await f.repository.prepareResume(
      f.repository.leaseCurrent(_operation)!,
      f.observation(_prefix),
      CancellationToken(),
      jobs: f.jobs,
      checkAdmission: admission.check,
    );
    admission.release();
    expect(f.record.state, ManagedInstallationState.resumePrepared);
    await f.service.recoverInterrupted();
    expect(f.record.attemptId, attempt.job.intentId);
    expect(f.record.state, ManagedInstallationState.recoveryRequired);
    expect(f.service.canResume(_operation), isFalse);
    await expectLater(
      f.service.reconcile(oldJob, CancellationToken()),
      throwsA(_failure(FailureKind.conflict)),
    );
    await f.coordinator.drain(f.session.scope);
    expect(f.backend.stages, 0);
    expect(f.records.last.state, JobState.recoveryRequired);
    await f.inspect();
    await f.resume();
    expect(f.record.attempt, 2);
    expect(f.backend.stages, 1);
    await f.close();
  });
}
