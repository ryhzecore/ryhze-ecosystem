import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

class _FailingStore extends MemoryTransactionStore {
  bool fail = false;
  @override
  Future<void> transact(void Function(StoreData) update) {
    if (fail) throw const AppFailure(FailureKind.storage);
    return super.transact(update);
  }
}

void main() {
  test(
    'job observers see committed states only and no duplicate admission',
    () async {
      final store = _FailingStore();
      final jobs = JobRepository(store);
      const scope = AccountScope.local();
      final states = <JobState>[];
      jobs.addListener(() => states.add(jobs.read(scope).single.state));
      final job = await jobs.enqueue(scope, 'fixture', 'one');
      await jobs.enqueue(scope, 'fixture', 'one');
      expect(states, [JobState.queued]);
      await jobs.transition(scope, job, JobState.running);
      expect(states, [JobState.queued, JobState.running]);
      store.fail = true;
      await expectLater(
        jobs.transition(scope, job, JobState.completed),
        throwsA(isA<AppFailure>()),
      );
      expect(states, [JobState.queued, JobState.running]);
      jobs.dispose();
      store.fail = false;
      await jobs.transition(scope, job, JobState.recoveryRequired);
      expect(states, [JobState.queued, JobState.running]);
      await store.close();
    },
  );
}
