import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/activity/activity_repository.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

void main() {
  const scope = AccountScope.local();
  test('observed completion preserves terminal history and ignores different attempt', () async {
    final store = MemoryTransactionStore();
    final activity = ActivityRepository(store);
    final jobs = JobRepository(store, activity: activity);
    final job = await jobs.enqueue(
      scope,
      'managedContentDiscard',
      'operation',
      intentId: 'attempt.1',
    );
    await store.transact((draft) {
      expect(
        jobs.completeObservedIn(
          draft,
          scope,
          kind: job.kind,
          item: job.item,
          intentId: 'attempt.2',
          checkpoint: 9,
        ),
        isFalse,
      );
    });
    expect(jobs.read(scope).single.state, JobState.queued);
    await store.transact((draft) {
      expect(
        jobs.completeObservedIn(
          draft,
          scope,
          kind: job.kind,
          item: job.item,
          intentId: job.intentId!,
          checkpoint: 9,
        ),
        isTrue,
      );
    });
    final complete = store.read();
    await store.transact((draft) {
      expect(
        jobs.completeObservedIn(
          draft,
          scope,
          kind: job.kind,
          item: job.item,
          intentId: job.intentId!,
          checkpoint: 20,
        ),
        isFalse,
      );
    });
    expect(store.read(), complete);
    expect(activity.read(scope).single.count, 1);
    final cancelled = await jobs.enqueue(
      scope,
      job.kind,
      job.item,
      intentId: 'attempt.2',
    );
    await jobs.transition(scope, cancelled, JobState.cancelled);
    await store.transact((draft) {
      expect(
        jobs.completeObservedIn(
          draft,
          scope,
          kind: job.kind,
          item: job.item,
          intentId: cancelled.intentId!,
          checkpoint: 9,
        ),
        isFalse,
      );
    });
    expect(jobs.read(scope).last.state, JobState.cancelled);
    jobs.dispose();
    activity.dispose();
    await store.close();
  });

  test(
    'observed completion rolls back with its owning inventory transaction',
    () async {
      final store = MemoryTransactionStore();
      final jobs = JobRepository(store);
      final job = await jobs.enqueue(
        scope,
        'managedContentDiscard',
        'operation',
        intentId: 'attempt.1',
      );
      final before = store.read();
      await expectLater(
        store.transact((draft) {
          jobs.completeObservedIn(
            draft,
            scope,
            kind: job.kind,
            item: job.item,
            intentId: job.intentId!,
            checkpoint: 9,
          );
          throw StateError('Injected inventory validation failure');
        }),
        throwsStateError,
      );
      expect(store.read(), before);
      jobs.dispose();
      await store.close();
    },
  );
}
