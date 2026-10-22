import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

class _Gate {
  final entered = Completer<void>();
  final release = Completer<void>();

  Future<void> wait() async {
    entered.complete();
    await release.future;
  }
}

class _ControlledStore extends MemoryTransactionStore {
  bool fail = false;
  _Gate? nextWrite;

  @override
  Future<void> transact(void Function(StoreData) update) async {
    final gate = nextWrite;
    nextWrite = null;
    if (gate != null) await gate.wait();
    if (fail) throw const AppFailure(FailureKind.storage);
    await super.transact(update);
  }
}

class _Operation implements JobOperation {
  int executions = 0;
  int reconciliations = 0;
  _Gate? executionGate;
  _Gate? reconciliationGate;
  CancellationToken? active;

  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken token,
    Future<void> Function(int) checkpoint,
  ) async {
    executions++;
    active = token;
    final gate = executionGate;
    executionGate = null;
    await checkpoint(1);
    if (gate != null) await gate.wait();
    await checkpoint(2);
    return JobOutcome.completed;
  }

  @override
  Future<JobOutcome> reconcile(JobRecord job, CancellationToken token) async {
    reconciliations++;
    active = token;
    final gate = reconciliationGate;
    reconciliationGate = null;
    if (gate != null) await gate.wait();
    return JobOutcome.completed;
  }
}

void main() {
  const scope = AccountScope.local();
  late _ControlledStore store;
  late JobRepository jobs;
  late _Operation operation;
  late JobCoordinator coordinator;

  setUp(() {
    store = _ControlledStore();
    jobs = JobRepository(store);
    operation = _Operation();
    coordinator = JobCoordinator(jobs, {'fixture': operation});
  });
  tearDown(() async {
    await coordinator.close();
    await store.close();
  });

  Future<JobRecord> interrupted(AccountScope account, String item) async {
    final job = await jobs.enqueue(account, 'fixture', item);
    await jobs.transition(account, job, JobState.running);
    return job;
  }

  test(
    'suspension gates execute and reconcile until durable recovery',
    () async {
      final previous = await interrupted(scope, 'previous');
      await jobs.transition(scope, previous, JobState.recoveryRequired);
      final running = await interrupted(scope, 'running');
      await jobs.enqueue(scope, 'fixture', 'queued');
      coordinator.suspend();
      await coordinator.drain(scope);
      await coordinator.reconcile(scope, previous);
      expect(operation.executions, 0);
      expect(operation.reconciliations, 0);

      await coordinator.resume(scope);
      expect(
        jobs.read(scope).singleWhere((job) => job.key == running.key).state,
        JobState.recoveryRequired,
      );
      expect(operation.executions, 0);
      expect(operation.reconciliations, 0);
      await coordinator.drain(scope);
      await coordinator.reconcile(scope, previous);
      expect(operation.executions, 1);
      expect(operation.reconciliations, 1);
    },
  );

  test(
    'resume waits for a cancelled operation and its final checkpoint',
    () async {
      final gate = operation.executionGate = _Gate();
      final first = await jobs.enqueue(scope, 'fixture', 'first');
      await jobs.enqueue(scope, 'fixture', 'second');
      final drain = coordinator.drain(scope);
      await gate.entered.future;
      coordinator.suspend();
      expect(operation.active!.isCancelled, isTrue);
      final resume = coordinator.resume(scope);
      await coordinator.drain(scope);
      expect(operation.executions, 1);
      expect(jobs.read(scope).first.state, JobState.running);
      gate.release.complete();
      await resume;
      await drain;
      final recovered = jobs.read(scope).first;
      expect(recovered.key, first.key);
      expect(recovered.state, JobState.recoveryRequired);
      expect(recovered.checkpoint, 2);
      expect(operation.executions, 1);
      await coordinator.drain(scope);
      expect(operation.executions, 2);
      expect(jobs.read(scope).first.state, JobState.recoveryRequired);
    },
  );

  test(
    'resume waits for cancelled reconciliation without accepting its outcome',
    () async {
      final previous = await interrupted(scope, 'previous');
      await jobs.transition(scope, previous, JobState.recoveryRequired);
      await jobs.enqueue(scope, 'fixture', 'queued');
      final gate = operation.reconciliationGate = _Gate();
      final reconciliation = coordinator.reconcile(scope, previous);
      await gate.entered.future;
      coordinator.suspend();
      final resume = coordinator.resume(scope);
      await coordinator.drain(scope);
      expect(operation.active!.isCancelled, isTrue);
      expect(operation.executions, 0);
      gate.release.complete();
      await reconciliation;
      await resume;
      expect(jobs.read(scope).first.state, JobState.recoveryRequired);
      expect(operation.reconciliations, 1);
      await coordinator.drain(scope);
      expect(operation.executions, 1);
    },
  );

  for (final boundary in ['suspend', 'cancelActive']) {
    test('$boundary invalidates a resume during its recovery write', () async {
      await interrupted(scope, 'interrupted');
      await jobs.enqueue(scope, 'fixture', 'queued');
      final gate = store.nextWrite = _Gate();
      final resume = coordinator.resume(scope);
      await gate.entered.future;
      await coordinator.drain(scope);
      expect(operation.executions, 0);
      if (boundary == 'suspend') {
        coordinator.suspend();
      } else {
        coordinator.cancelActive();
      }
      gate.release.complete();
      await resume;
      await coordinator.drain(scope);
      await coordinator.reconcile(scope, jobs.read(scope).first);
      expect(operation.executions, 0);
      expect(operation.reconciliations, 0);
      await coordinator.resume(scope);
      await coordinator.drain(scope);
      expect(operation.executions, 1);
    });
  }

  test(
    'failed resume retains closed admission until an explicit successful retry',
    () async {
      await interrupted(scope, 'interrupted');
      await jobs.enqueue(scope, 'fixture', 'queued');
      store.fail = true;
      await expectLater(coordinator.resume(scope), throwsA(isA<AppFailure>()));
      store.fail = false;
      await coordinator.drain(scope);
      expect(operation.executions, 0);
      expect(jobs.read(scope).first.state, JobState.running);
      await coordinator.resume(scope);
      expect(jobs.read(scope).first.state, JobState.recoveryRequired);
      await coordinator.drain(scope);
      expect(operation.executions, 1);
    },
  );

  test(
    'overlapping scope resumes serialize recovery before admitting work',
    () async {
      const replacement = AccountScope.verified('fixture', 'replacement');
      await interrupted(scope, 'old');
      await interrupted(replacement, 'current');
      await jobs.enqueue(replacement, 'fixture', 'queued');
      final gate = store.nextWrite = _Gate();
      final oldResume = coordinator.resume(scope);
      await gate.entered.future;
      coordinator.cancelActive();
      final currentResume = coordinator.resume(replacement);
      await coordinator.drain(replacement);
      expect(operation.executions, 0);
      gate.release.complete();
      await Future.wait([oldResume, currentResume]);
      expect(jobs.read(replacement).first.state, JobState.recoveryRequired);
      expect(operation.executions, 0);
      await coordinator.drain(replacement);
      expect(operation.executions, 1);
    },
  );

  test(
    'close waits for pending recovery and permanently prevents admission',
    () async {
      await interrupted(scope, 'interrupted');
      await jobs.enqueue(scope, 'fixture', 'queued');
      final gate = store.nextWrite = _Gate();
      final resume = coordinator.resume(scope);
      await gate.entered.future;
      var closed = false;
      final close = coordinator.close().then((_) => closed = true);
      await Future<void>.delayed(Duration.zero);
      expect(closed, isFalse);
      gate.release.complete();
      await Future.wait([close, resume]);
      await coordinator.resume(scope);
      await coordinator.drain(scope);
      await coordinator.reconcile(scope, jobs.read(scope).first);
      expect(operation.executions, 0);
      expect(operation.reconciliations, 0);
    },
  );

  test(
    'failed initial job write releases ownership without an external effect',
    () async {
      final previous = await interrupted(scope, 'previous');
      await jobs.transition(scope, previous, JobState.recoveryRequired);
      final queued = await jobs.enqueue(scope, 'fixture', 'queued');
      store.fail = true;
      await expectLater(coordinator.drain(scope), throwsA(isA<AppFailure>()));
      expect(operation.executions, 0);
      expect(
        jobs.read(scope).singleWhere((job) => job.key == queued.key).state,
        JobState.queued,
      );
      store.fail = false;
      await coordinator.reconcile(scope, previous);
      expect(operation.reconciliations, 1);
      await coordinator.drain(scope);
      expect(operation.executions, 1);
      expect(
        jobs.read(scope).every((job) => job.state == JobState.completed),
        isTrue,
      );
    },
  );
}
