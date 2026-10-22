import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

void main() {
  const scope = AccountScope.local();
  test(
    'explicit cross-kind retry retires only the matching interrupted intent',
    () async {
      final store = MemoryTransactionStore();
      final jobs = JobRepository(store);
      final prior = await jobs.enqueue(
        scope,
        'stage',
        'item',
        intentId: 'original',
      );
      await jobs.transition(scope, prior, JobState.running);
      await jobs.transition(scope, prior, JobState.recoveryRequired);
      await store.transact((draft) {
        jobs.admitRetryIn(
          draft,
          scope,
          kind: 'discard',
          previousKind: 'stage',
          item: 'item',
          previousIntentId: 'original',
          intentId: 'original.discard.1',
          checkpoint: 0,
        );
      });
      final records = jobs.read(scope);
      expect(records.first.state, JobState.failed);
      expect(records.last.kind, 'discard');
      expect(records.last.state, JobState.queued);
      jobs.dispose();
      await store.close();
    },
  );
  test('wrong previous kind or running operation cannot be retired', () async {
    for (final running in [false, true]) {
      final store = MemoryTransactionStore();
      final jobs = JobRepository(store);
      final prior = await jobs.enqueue(
        scope,
        'stage',
        'item',
        intentId: 'original',
      );
      if (running) await jobs.transition(scope, prior, JobState.running);
      final snapshot = jobs.read(scope).single.state;
      await expectLater(
        store.transact((draft) {
          jobs.admitRetryIn(
            draft,
            scope,
            kind: 'discard',
            previousKind: running ? 'stage' : 'wrong',
            item: 'item',
            previousIntentId: 'original',
            intentId: 'original.discard.1',
            checkpoint: 0,
          );
        }),
        throwsA(isA<AppFailure>()),
      );
      expect(jobs.read(scope).single.state, snapshot);
      jobs.dispose();
      await store.close();
    }
  });
}
