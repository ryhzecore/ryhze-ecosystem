import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

class _Operation implements JobOperation {
  final started = Completer<void>();
  final settle = Completer<void>();
  final items = <String>[];
  late CancellationToken token;
  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken cancellation,
    Future<void> Function(int) checkpoint,
  ) async {
    token = cancellation;
    items.add(job.item);
    started.complete();
    await settle.future;
    await checkpoint(1);
    return JobOutcome.completed;
  }

  @override
  Future<JobOutcome> reconcile(JobRecord job, CancellationToken token) async =>
      JobOutcome.unknown;
}

void main() {
  const scope = AccountScope.local();
  test('failed settlement releases hold but keeps admission suspended', () async {
    final store = MemoryTransactionStore();
    final repository = JobRepository(store);
    final queue = JobCoordinator(repository, {});
    final job = await repository.enqueue(scope, 'missing', 'selected');
    final lease = queue.acquireIdle();
    lease.suspendAfterSettlementFailure();
    expect(
      () => lease.run(scope, job, CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
    expect(queue.acquireIdle, throwsA(isA<AppFailure>()));
    await queue.drain(scope);
    expect(repository.read(scope).single.state, JobState.queued);
    // Only the owner may call this after its separate native recovery succeeds.
    await queue.resume(scope);
    final recovered = queue.acquireIdle();
    recovered.release();
    await queue.close();
    repository.dispose();
    await store.close();
  });
  test(
    'idle hold blocks competing admission and runs only its selected job',
    () async {
      final store = MemoryTransactionStore();
      final repository = JobRepository(store);
      final operation = _Operation();
      final queue = JobCoordinator(repository, {'fixture': operation});
      await repository.enqueue(scope, 'fixture', 'unrelated');
      final selected = await repository.enqueue(scope, 'fixture', 'selected');
      final lease = queue.acquireIdle();
      expect(queue.acquireIdle, throwsA(isA<AppFailure>()));
      await expectLater(queue.prepareShutdown(), throwsA(isA<AppFailure>()));
      await queue.drain(scope);
      await queue.resume(scope);
      expect(operation.items, isEmpty);
      final cancellation = CancellationToken();
      var settled = false;
      final running = lease
          .run(scope, selected, cancellation)
          .whenComplete(() => settled = true);
      await operation.started.future;
      expect(queue.acquireIdle, throwsA(isA<AppFailure>()));
      cancellation.cancel();
      expect(operation.token.isCancelled, isTrue);
      expect(settled, isFalse);
      operation.settle.complete();
      await running;
      expect(operation.items, ['selected']);
      final jobs = repository.read(scope);
      expect(jobs.first.state, JobState.queued);
      expect(jobs.last.state, JobState.recoveryRequired);
      await queue.close();
      repository.dispose();
      await store.close();
    },
  );

  test(
    'new suspension invalidates hold and release cannot reopen the queue',
    () async {
      final store = MemoryTransactionStore();
      final repository = JobRepository(store);
      final queue = JobCoordinator(repository, {});
      final selected = await repository.enqueue(scope, 'missing', 'selected');
      final lease = queue.acquireIdle();
      queue.suspend();
      expect(lease.check, throwsA(isA<AppFailure>()));
      expect(
        () => lease.run(scope, selected, CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      lease.release();
      lease.release();
      await queue.drain(scope);
      expect(repository.read(scope).single.state, JobState.queued);
      await queue.resume(scope);
      await queue.drain(scope);
      expect(repository.read(scope).single.state, JobState.blocked);
      await queue.close();
      repository.dispose();
      await store.close();
    },
  );

  test('close and pre-cancellation refuse held work before effects', () async {
    final store = MemoryTransactionStore();
    final repository = JobRepository(store);
    final queue = JobCoordinator(repository, {});
    final selected = await repository.enqueue(scope, 'missing', 'selected');
    final lease = queue.acquireIdle();
    expect(
      () => lease.run(scope, selected, CancellationToken()..cancel()),
      throwsA(isA<AppFailure>()),
    );
    await queue.close();
    expect(lease.check, throwsA(isA<AppFailure>()));
    expect(
      () => lease.run(scope, selected, CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
    lease.release();
    expect(queue.acquireIdle, throwsA(isA<AppFailure>()));
    expect(repository.read(scope).single.state, JobState.queued);
    repository.dispose();
    await store.close();
  });
}
