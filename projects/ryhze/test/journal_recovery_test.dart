import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/features/collections/collection_repository.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/session/session_controller.dart';

import 'support/journal_fixture.dart';

void main() {
  late Directory root;
  setUp(() async {
    root = await createJournalFixture('ryhze-recovery-');
  });
  tearDown(() async {
    await root.delete(recursive: true);
  });
  test('oversized interrupted journal and valid checkpoint remain untouched', () async {
    final payload = jsonEncode({
      'schema': 2,
      'accounts': {},
      'preferences': {},
    });
    final checkpoint = File('${root.path}/state.journal.checkpoint');
    final checkpointBytes = utf8.encode(
      '${jsonEncode({'sequence': 1, 'checksum': JournalStore.checksum(payload), 'payload': payload})}\n',
    );
    await checkpoint.writeAsBytes(checkpointBytes);
    final journal = File('${root.path}/state.journal');
    final original = List<int>.filled(2049, 65);
    await journal.writeAsBytes(original);
    await expectLater(
      JournalStore.open(root, byteLimit: 2048),
      throwsA(isA<AppFailure>()),
    );
    expect(await journal.readAsBytes(), original);
    expect(await checkpoint.readAsBytes(), checkpointBytes);
    expect(root.listSync(), hasLength(2));
  });
  test(
    'bounded compaction preserves repeated transactions and restarts',
    () async {
      var store = await JournalStore.open(root, byteLimit: 2048);
      for (var i = 0; i < 40; i++) {
        await PreferenceRepository(store)
            .save(Preferences(reduceMotion: i.isOdd));
      }
      expect(
        await File('${root.path}/state.journal').length(),
        lessThanOrEqualTo(2048),
      );
      await store.close();
      store = await JournalStore.open(root, byteLimit: 2048);
      expect(PreferenceRepository(store).read().reduceMotion, isTrue);
      await store.close();
    },
  );
  test('interrupted compaction restores validated checkpoint without losing original', () async {
    var store = await JournalStore.open(root);
    await PreferenceRepository(store)
        .save(const Preferences(reduceMotion: true));
    final payload = jsonEncode(store.read());
    await store.close();
    await File('${root.path}/state.journal.checkpoint').writeAsString(
      '${jsonEncode({'sequence': 1, 'checksum': JournalStore.checksum(payload), 'payload': payload})}\n',
    );
    await File('${root.path}/state.journal')
        .writeAsString('{interrupted replacement');
    store = await JournalStore.open(root);
    expect(PreferenceRepository(store).read().reduceMotion, isTrue);
    expect(
      root.listSync().where((file) => file.path.contains('.recovery-')),
      hasLength(1),
    );
    final recovery = root.listSync().singleWhere(
      (file) => file.path.contains('.recovery-'),
    );
    expect(
      await File(recovery.path).readAsString(),
      '{interrupted replacement',
    );
    expect(
      await File('${root.path}/state.journal.checkpoint').exists(),
      isFalse,
    );
    await store.close();
  });
  test(
    'incomplete temporary checkpoint does not replace committed data',
    () async {
      var store = await JournalStore.open(root);
      await PreferenceRepository(store)
          .save(const Preferences(reduceMotion: true));
      await store.close();
      await File('${root.path}/state.journal.checkpoint.tmp')
          .writeAsString('incomplete');
      store = await JournalStore.open(root);
      expect(PreferenceRepository(store).read().reduceMotion, isTrue);
      expect(
        root.listSync().where((file) => file.path.contains('.tmp.recovery-')),
        hasLength(1),
      );
      await store.close();
    },
  );
  test(
    'job identity and interrupted outcomes survive a real store reopen',
    () async {
      var store = await JournalStore.open(root);
      var jobs = JobRepository(store);
      const scope = AccountScope.local();
      final job = await jobs.enqueue(scope, 'test-operation', 'item');
      await jobs.transition(scope, job, JobState.running, checkpoint: 32);
      await store.close();
      store = await JournalStore.open(root);
      jobs = JobRepository(store);
      final coordinator = JobCoordinator(jobs, {});
      await coordinator.recover(scope);
      final again = await jobs.enqueue(scope, 'test-operation', 'item');
      expect(again.key, job.key);
      expect(again.checkpoint, 32);
      expect(again.state, JobState.recoveryRequired);
      await coordinator.close();
      await store.close();
    },
  );
}
