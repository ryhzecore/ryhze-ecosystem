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
  int calls = 0;
  late CancellationToken cancellation;
  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken token,
    Future<void> Function(int) checkpoint,
  ) async {
    calls++;
    cancellation = token;
    if (!started.isCompleted) started.complete();
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
  for (final boundary in ['close', 'quiesce', 'prepareShutdown']) {
    test(
      '$boundary joins durable work even when cancellation throws',
      () async {
        final storage = MemoryTransactionStore();
        final repository = JobRepository(storage);
        final operation = _Operation();
        final queue = JobCoordinator(repository, {'fixture': operation});
        await repository.enqueue(scope, 'fixture', 'one');
        await repository.enqueue(scope, 'fixture', 'two');
        final running = queue.drain(scope);
        await operation.started.future;
        final first = StateError('synthetic cancellation failure');
        operation.cancellation.onCancel(() => throw first);
        var reported = false;
        final Future<Object?> closing = switch (boundary) {
          'close' => queue.close(),
          'quiesce' => queue.quiesce(),
          _ => queue.prepareShutdown(),
        };
        final checked = expectLater(
          closing.whenComplete(() => reported = true),
          throwsA(same(first)),
        );
        await Future<void>.delayed(Duration.zero);
        expect(reported, isFalse);
        expect(repository.read(scope).first.state, JobState.running);
        expect(operation.calls, 1);
        operation.settle.complete();
        await running;
        await checked;
        expect(repository.read(scope).first.state, JobState.recoveryRequired);
        expect(repository.read(scope).first.checkpoint, 1);
        expect(repository.read(scope).last.state, JobState.queued);
        await queue.close();
        repository.dispose();
        await storage.close();
      },
    );
  }
  test(
    'old lease release cannot override a newer lifecycle suspension',
    () async {
      final storage = MemoryTransactionStore();
      final repository = JobRepository(storage);
      final queue = JobCoordinator(repository, {});
      final lease = await queue.prepareShutdown();
      queue.suspend();
      await repository.enqueue(scope, 'unsupported', 'one');
      await lease.release(scope);
      await queue.drain(scope);
      expect(repository.read(scope).single.state, JobState.queued);
      await queue.resume(scope);
      await queue.drain(scope);
      expect(repository.read(scope).single.state, JobState.blocked);
      await queue.close();
      repository.dispose();
      await storage.close();
    },
  );
  test(
    'shutdown lease joins effects and holds admission across resume',
    () async {
      final storage = MemoryTransactionStore();
      final repository = JobRepository(storage);
      final operation = _Operation();
      final queue = JobCoordinator(repository, {'fixture': operation});
      await repository.enqueue(scope, 'fixture', 'one');
      await repository.enqueue(scope, 'fixture', 'two');
      final draining = queue.drain(scope);
      await operation.started.future;
      var granted = false;
      final preparing = queue.prepareShutdown().then((value) {
        granted = true;
        return value;
      });
      expect(operation.cancellation.isCancelled, isTrue);
      await queue.resume(scope);
      await queue.drain(scope);
      expect(granted, isFalse);
      expect(operation.calls, 1);
      await expectLater(queue.prepareShutdown(), throwsA(isA<AppFailure>()));
      operation.settle.complete();
      await draining;
      final lease = await preparing;
      expect(repository.read(scope).first.state, JobState.recoveryRequired);
      await queue.resume(scope);
      await queue.drain(scope);
      expect(operation.calls, 1);
      await lease.release(scope);
      await lease.release(scope);
      await queue.drain(scope);
      expect(operation.calls, 2);
      expect(repository.read(scope).last.state, JobState.completed);
      await queue.close();
      repository.dispose();
      await storage.close();
    },
  );

  test('closed coordinator never grants or reopens a shutdown lease', () async {
    final storage = MemoryTransactionStore();
    final repository = JobRepository(storage);
    final queue = JobCoordinator(repository, {});
    final lease = await queue.prepareShutdown();
    await queue.close();
    await lease.release(scope);
    await expectLater(queue.prepareShutdown(), throwsA(isA<AppFailure>()));
    await repository.enqueue(scope, 'unsupported', 'one');
    await queue.drain(scope);
    expect(repository.read(scope).single.state, JobState.queued);
    repository.dispose();
    await storage.close();
  });

  test(
    'close racing held work cancels preparation without granting lease',
    () async {
      final storage = MemoryTransactionStore();
      final repository = JobRepository(storage);
      final operation = _Operation();
      final queue = JobCoordinator(repository, {'fixture': operation});
      await repository.enqueue(scope, 'fixture', 'one');
      final draining = queue.drain(scope);
      await operation.started.future;
      final preparing = expectLater(
        queue.prepareShutdown(),
        throwsA(isA<AppFailure>()),
      );
      final closing = queue.close();
      operation.settle.complete();
      await Future.wait([preparing, closing, draining]);
      expect(repository.read(scope).single.state, JobState.recoveryRequired);
      repository.dispose();
      await storage.close();
    },
  );
}
