import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/domain/app_route.dart';
import 'package:ryhze/features/activity/activity_repository.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

import 'support/journal_fixture.dart';

void main() {
  const scope = AccountScope.local();
  test('job outcome and coalesced activity survive restart together', () async {
    final dir = await createJournalFixture('ryhze-activity-');
    try {
      var store = await JournalStore.open(dir);
      var activity = ActivityRepository(store);
      final jobs = JobRepository(store, activity: activity);
      final job = await jobs.enqueue(scope, 'fixture', 'item');
      await jobs.transition(scope, job, JobState.running);
      await jobs.transition(scope, job, JobState.recoveryRequired);
      await jobs.transition(scope, job, JobState.recoveryRequired);
      expect(activity.read(scope).single.count, 2);
      expect(
        activity.read(scope).single.route?.toUri().toString(),
        'ryhze://app/activity',
      );
      expect(
        activity.read(scope).single.preview(private: true),
        'Ryhze activity is available.',
      );
      await activity.acknowledge(scope, activity.read(scope).single.id);
      activity.dispose();
      await store.close();
      store = await JournalStore.open(dir);
      activity = ActivityRepository(store);
      expect(
        JobRepository(store).read(scope).single.state,
        JobState.recoveryRequired,
      );
      expect(activity.read(scope).single.acknowledged, isTrue);
      expect(
        activity.read(scope).single.route?.destination,
        AppDestination.activity,
      );
      expect(
        activity.read(const AccountScope.verified('test', 'other')),
        isEmpty,
      );
      await activity.clearAcknowledged(scope);
      expect(activity.read(scope), isEmpty);
      expect(JobRepository(store).read(scope), hasLength(1));
      activity.dispose();
      await store.close();
    } finally {
      await dir.delete(recursive: true);
    }
  });
  test(
    'activity is bounded and invalid targets roll back transaction',
    () async {
      final store = MemoryTransactionStore();
      final activity = ActivityRepository(store);
      final now = DateTime.utc(2026);
      await store.transact((draft) {
        for (var i = 0; i < 250; i++) {
          ActivityRepository.recordIn(
            draft,
            scope,
            '$i',
            ActivityKind.blocked,
            now.add(Duration(seconds: i)),
          );
        }
      });
      expect(activity.read(scope), hasLength(200));
      expect(
        activity.read(scope).first.occurredAt,
        now.add(const Duration(seconds: 249)),
      );
      await expectLater(
        store.transact((draft) {
          ActivityRepository.recordIn(
            draft,
            scope,
            'invalid',
            ActivityKind.failed,
            now,
            route: const AppRoute(
              AppDestination.gamesStore,
              itemId: '../command',
            ),
          );
        }),
        throwsA(isA<AppFailure>()),
      );
      expect(activity.read(scope), hasLength(200));
      activity.dispose();
      await store.close();
    },
  );
}
