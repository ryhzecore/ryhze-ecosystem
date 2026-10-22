import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/managed_content_service.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_installation_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

// Synthetic transaction barriers exercise ordering; they do not establish
// filesystem durability, production rights or real native staging acceptance.
class _Store extends MemoryTransactionStore {
  @override
  bool get durable => true;
  Completer<void>? gate;
  final entered = Completer<void>();
  void Function()? afterCommit;
  bool failAfterCommit = false, failAbort = false;
  final commits = <StoreData>[];

  @override
  Future<void> transact(void Function(StoreData) change) async {
    if (gate != null) {
      if (!entered.isCompleted) entered.complete();
      await gate!.future;
    }
    await super.transact((draft) {
      change(draft);
      final records =
          (draft['managedInstallations'] as Map?)?['records'] as Map?;
      if (failAbort &&
          records?.values.any(
                (value) => value['state'] == 'recoveryRequired',
              ) ==
              true) {
        throw const AppFailure(FailureKind.storage);
      }
    });
    commits.add(read());
    final callback = afterCommit;
    afterCommit = null;
    callback?.call();
    if (failAfterCommit) {
      failAfterCommit = false;
      throw const AppFailure(FailureKind.storage);
    }
  }
}

class _Backend implements ContentStagingBackend {
  @override
  final deviceId = '${'1' * 16}:${'2' * 32}';
  final sourceId = '${'1' * 16}:${'3' * 32}';
  int inspections = 0, stages = 0;
  bool failDrain = false;
  Completer<void>? drainGate;
  final drainEntered = Completer<void>();

  @override
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    inspections++;
    return ContentStagingReceipt(
      state: ContentStagingState.checkpoint,
      verifiedBytes: 0,
      verifiedChunks: 0,
      rootIdentity: deviceId,
      sourceIdentity: sourceId,
    );
  }

  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    stages++;
    throw StateError('Admission tests must never execute a native stage');
  }

  @override
  Future<void> drain() async {
    if (!drainEntered.isCompleted) drainEntered.complete();
    await drainGate?.future;
    if (failDrain) throw const AppFailure(FailureKind.network);
  }

  @override
  Future<void> cancel() async {}
  @override
  Future<void> close() async {}
}

ManagedStageIntent _intent() => ManagedStageIntent(
  operationId: 'a' * 64,
  item: ProviderItemRef('fixture', 'game', namespace: 'exact'),
  packageId: 'fixture-package',
  version: '1',
  manifestSha256: 'b' * 64,
  sourceRelative: 'fixture.bin',
  expectedSize: 600000,
  expectedSha256: 'c' * 64,
  slot: 0,
);

class _Fixture {
  _Fixture({int maxPending = 32}) {
    jobs = JobRepository(store, maxPending: maxPending);
    inventory = ManagedInstallationRepository(
      store,
      session,
      deviceId: backend.deviceId,
    );
    service = ManagedContentService(inventory, backend, jobs, session);
    coordinator = JobCoordinator(jobs, {
      ManagedContentService.jobKind: service,
    });
    session.addListener(coordinator.cancelActive);
  }
  final store = _Store();
  final backend = _Backend();
  final session = SessionController();
  late final JobRepository jobs;
  late final ManagedInstallationRepository inventory;
  late final ManagedContentService service;
  late final JobCoordinator coordinator;
  final token = CancellationToken();

  Future<JobRecord> prepare() =>
      service.prepareLocalStage(_intent(), token, coordinator: coordinator);

  Future<void> close() async {
    await coordinator.close();
    await service.close();
    inventory.dispose();
    jobs.dispose();
    session.dispose();
    await store.close();
  }
}

Matcher _failure(FailureKind kind) =>
    isA<AppFailure>().having((error) => error.kind, 'kind', kind);

void main() {
  test(
    'initial inventory and queue are one commit; duplicate is inert',
    () async {
      final f = _Fixture();
      addTearDown(f.close);
      final first = await f.prepare();
      expect(f.store.commits, hasLength(1));
      expect(
        f.inventory.readCurrent().single.state,
        ManagedInstallationState.prepared,
      );
      expect(f.jobs.read(f.session.scope).single.state, JobState.queued);
      expect((await f.prepare()).key, first.key);
      expect(f.jobs.read(f.session.scope), hasLength(1));
      expect(f.inventory.readCurrent(), hasLength(1));
      expect(f.backend.stages, 0);
    },
  );

  test(
    'full queue rolls inventory reservation back with the rejected job',
    () async {
      final f = _Fixture(maxPending: 1);
      addTearDown(f.close);
      await f.jobs.enqueue(f.session.scope, 'unavailable', 'unrelated');
      final before = f.store.read();
      await expectLater(f.prepare(), throwsA(_failure(FailureKind.conflict)));
      expect(f.store.read(), before);
      expect(f.inventory.readCurrent(), isEmpty);
      expect(f.backend.stages, 0);
    },
  );

  for (final reason in ['cancel', 'same account replacement', 'suspend']) {
    test(
      '$reason while transaction waits creates neither record nor job',
      () async {
        final f = _Fixture();
        addTearDown(f.close);
        f.store.gate = Completer<void>();
        final operation = f.prepare();
        final result = expectLater(
          operation,
          throwsA(_failure(FailureKind.cancelled)),
        );
        await f.store.entered.future;
        switch (reason) {
          case 'cancel':
            f.token.cancel();
          case 'same account replacement':
            f.session.replace(f.session.scope);
          case 'suspend':
            f.service.interrupt();
        }
        f.store.gate!.complete();
        await result;
        expect(f.store.commits, isEmpty);
        expect(f.inventory.readCurrent(), isEmpty);
        expect(f.jobs.read(f.session.scope), isEmpty);
        expect(f.backend.stages, 0);
      },
    );

    test(
      '$reason after commit cancels exact queued work before drain',
      () async {
        final f = _Fixture();
        addTearDown(f.close);
        final oldScope = f.session.scope;
        Future<void>? attemptedDrain;
        f.store.afterCommit = () {
          attemptedDrain = f.coordinator.drain(oldScope);
          switch (reason) {
            case 'cancel':
              f.token.cancel();
            case 'same account replacement':
              f.session.replace(oldScope);
            case 'suspend':
              f.service.interrupt();
          }
        };
        await expectLater(
          f.prepare(),
          throwsA(_failure(FailureKind.cancelled)),
        );
        await attemptedDrain;
        expect(f.store.commits, hasLength(2));
        expect(f.jobs.read(oldScope).single.state, JobState.cancelled);
        expect(
          f.inventory.readCurrent().single.state,
          ManagedInstallationState.recoveryRequired,
        );
        await f.coordinator.drain(oldScope);
        expect(f.backend.stages, 0);
      },
    );
  }

  for (final state in [
    JobState.queued,
    JobState.cancelled,
    JobState.completed,
  ]) {
    test(
      'job-only collision in ${state.name} cannot create an inventory pair',
      () async {
        final f = _Fixture();
        addTearDown(f.close);
        final job = await f.jobs.enqueue(
          f.session.scope,
          ManagedContentService.jobKind,
          _intent().operationId,
          intentId: _intent().operationId,
        );
        if (state == JobState.completed) {
          await f.jobs.transition(f.session.scope, job, JobState.running);
        }
        if (state != JobState.queued) {
          await f.jobs.transition(f.session.scope, job, state);
        }
        final before = f.store.read();
        await expectLater(f.prepare(), throwsA(_failure(FailureKind.conflict)));
        expect(f.store.read(), before);
        expect(f.backend.stages, 0);
      },
    );
  }

  for (final action in ['cancel', 'close']) {
    test(
      '$action waits actual preflight drain before releasing admission',
      () async {
        final f = _Fixture();
        addTearDown(f.close);
        f.backend.drainGate = Completer<void>();
        var settled = false;
        final operation = f.prepare().whenComplete(() => settled = true);
        final result = expectLater(
          operation,
          throwsA(_failure(FailureKind.cancelled)),
        );
        await f.backend.drainEntered.future;
        Future<void>? closing;
        if (action == 'cancel') {
          f.token.cancel();
        } else {
          closing = f.service.close();
        }
        await Future<void>.delayed(Duration.zero);
        expect(settled, isFalse);
        expect(
          f.coordinator.acquireIdle,
          throwsA(_failure(FailureKind.conflict)),
        );
        expect(f.store.commits, isEmpty);
        f.backend.drainGate!.complete();
        await result;
        await closing;
        expect(f.inventory.readCurrent(), isEmpty);
        expect(f.jobs.read(f.session.scope), isEmpty);
        expect(f.backend.stages, 0);
      },
    );
  }

  test(
    'different account after commit cleans only the original scope',
    () async {
      final f = _Fixture();
      addTearDown(f.close);
      final previous = f.session.scope;
      f.store.afterCommit = () {
        f.session.replace(
          const AccountScope.verified('fixture', 'new-account'),
        );
      };
      await expectLater(f.prepare(), throwsA(_failure(FailureKind.cancelled)));
      expect(f.jobs.read(previous).single.state, JobState.cancelled);
      expect(f.jobs.read(f.session.scope), isEmpty);
      expect(f.inventory.readCurrent(), isEmpty);
      final records =
          (f.store.read()['managedInstallations'] as Map)['records'] as Map;
      final old = ManagedInstallationRecord.decode(
        records.values.single as Map,
      );
      expect(old.scope.key, previous.key);
      expect(old.state, ManagedInstallationState.recoveryRequired);
      await f.coordinator.drain(previous);
      expect(f.backend.stages, 0);
    },
  );

  test(
    'legacy prepared-only record requires recovery, not inferred admission',
    () async {
      final f = _Fixture();
      addTearDown(f.close);
      await f.inventory.prepare(
        _intent(),
        ManagedStageObservation(
          operationId: _intent().operationId,
          state: ManagedStageObservationState.checkpoint,
          verifiedBytes: 0,
          verifiedChunks: 0,
          rootIdentity: f.backend.deviceId,
          sourceIdentity: f.backend.sourceId,
        ),
        f.token,
      );
      final before = f.store.read();
      await expectLater(f.prepare(), throwsA(_failure(FailureKind.conflict)));
      expect(f.store.read(), before);
      expect(f.jobs.read(f.session.scope), isEmpty);
    },
  );

  for (final fault in ['flush', 'abort', 'drain']) {
    test('$fault failure keeps queue closed and never stages', () async {
      final f = _Fixture();
      addTearDown(f.close);
      if (fault == 'flush') f.store.failAfterCommit = true;
      if (fault == 'abort') {
        f.store.failAbort = true;
        f.store.afterCommit = f.token.cancel;
      }
      if (fault == 'drain') f.backend.failDrain = true;
      await expectLater(f.prepare(), throwsA(isA<AppFailure>()));
      await f.coordinator.drain(f.session.scope);
      expect(f.backend.stages, 0);
      expect(
        f.coordinator.acquireIdle,
        throwsA(_failure(FailureKind.conflict)),
      );
      if (fault == 'drain') {
        expect(f.inventory.readCurrent(), isEmpty);
      } else {
        expect(f.jobs.read(f.session.scope).single.state, JobState.queued);
        expect(
          f.inventory.readCurrent().single.state,
          ManagedInstallationState.prepared,
        );
      }
    });
  }

  test(
    'busy and unrelated coordinators reject before native inspection',
    () async {
      final f = _Fixture();
      addTearDown(f.close);
      final hold = f.coordinator.acquireIdle();
      expect(f.prepare, throwsA(_failure(FailureKind.conflict)));
      hold.release();
      final unrelated = JobCoordinator(JobRepository(f.store), {});
      await expectLater(
        () => f.service.prepareLocalStage(
          _intent(),
          f.token,
          coordinator: unrelated,
        ),
        throwsA(_failure(FailureKind.invalidData)),
      );
      await unrelated.close();
      expect(f.backend.inspections, 0);
      expect(f.store.commits, isEmpty);
    },
  );
}
