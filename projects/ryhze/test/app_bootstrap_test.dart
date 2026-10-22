import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

class _RecoveryStore extends MemoryTransactionStore {
  bool failWrites = false;
  bool failClose = false;
  int closes = 0;

  @override
  Future<void> transact(void Function(StoreData) update) async {
    if (failWrites) throw StateError('Synthetic recovery write failure');
    await super.transact(update);
  }

  @override
  Future<void> close() async {
    closes++;
    await super.close();
    if (failClose) throw StateError('Synthetic cleanup failure');
  }
}

void main() {
  for (final failClose in [false, true]) {
    test(
      'failed bootstrap closes candidate and retains safe fallback, cleanup failure=$failClose',
      () async {
        final storage = _RecoveryStore();
        final jobs = JobRepository(storage);
        const scope = AccountScope.local();
        final job = await jobs.enqueue(scope, 'fixture', 'interrupted');
        await jobs.transition(scope, job, JobState.running);
        storage
          ..failWrites = true
          ..failClose = failClose;
        final services = await AppServices.bootstrap(
          openStore: () async => storage,
        );
        addTearDown(services.close);
        expect(storage.closes, 1);
        expect(services.storage, isNot(same(storage)));
        expect(services.storageFailure?.kind, FailureKind.recovery);
        expect(services.store.writable, isFalse);
        expect(services.workspace.writable, isFalse);
        expect(jobs.read(scope).single.state, JobState.running);
      },
    );
  }

  test(
    'successful bootstrap recovers shared jobs before returning services',
    () async {
      final storage = _RecoveryStore();
      final jobs = JobRepository(storage);
      const scope = AccountScope.local();
      final job = await jobs.enqueue(scope, 'fixture', 'interrupted');
      await jobs.transition(scope, job, JobState.running);
      final services = await AppServices.bootstrap(
        openStore: () async => storage,
      );
      expect(services.storage, same(storage));
      expect(services.storageFailure, isNull);
      expect(jobs.read(scope).single.state, JobState.recoveryRequired);
      expect(services.workspace.events.single.kind, ActivityKind.recovery);
      expect(services.workspace.events.single.count, 1);
      expect(storage.closes, 0);
      await services.close();
      expect(storage.closes, 1);
    },
  );
}
