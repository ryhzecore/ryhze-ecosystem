import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/collections/collection_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

import 'support/journal_fixture.dart';

void main() {
  late Directory root;
  setUp(() async {
    root = await createJournalFixture('ryhze-store-');
  });
  tearDown(() async {
    await root.delete(recursive: true);
  });
  test(
    'transactions survive restart without leaking exact account keys',
    () async {
      var store = await JournalStore.open(root);
      var repository = CollectionRepository(store);
      const first = AccountScope.verified('provider', 'Account');
      const second = AccountScope.verified('provider', 'account');
      await Future.wait([
        repository.setSaved(first, const SavedTitle('Title', 'First'), true),
        repository.setSaved(first, const SavedTitle('title', 'Second'), true),
      ]);
      expect(repository.read(second), isEmpty);
      await store.close();
      store = await JournalStore.open(root);
      repository = CollectionRepository(store);
      expect(repository.read(first).map((v) => v.id), ['Title', 'title']);
      final copy = store.read();
      (copy['accounts'] as Map).clear();
      expect(repository.read(first), hasLength(2));
      await expectLater(
        store.transact((draft) {
          draft.clear();
          throw StateError('abort');
        }),
        throwsStateError,
      );
      expect(repository.read(first), hasLength(2));
      await store.close();
    },
  );
  test('interrupted trailing transaction is backed up before repair', () async {
    var store = await JournalStore.open(root);
    await PreferenceRepository(store)
        .save(const Preferences(reduceMotion: true));
    await store.close();
    final file = File('${root.path}/state.journal');
    await file.writeAsString('{"incomplete', mode: FileMode.append);
    final interrupted = await file.readAsBytes();
    store = await JournalStore.open(root);
    expect(PreferenceRepository(store).read().reduceMotion, isTrue);
    expect(
      root.listSync().where((file) => file.path.contains('.recovery-')),
      hasLength(1),
    );
    final recovery = root.listSync().singleWhere(
      (file) => file.path.contains('.recovery-'),
    );
    expect(await File(recovery.path).readAsBytes(), interrupted);
    await PreferenceRepository(store)
        .save(const Preferences(reduceMotion: false));
    await store.close();
    store = await JournalStore.open(root);
    expect(PreferenceRepository(store).read().reduceMotion, isFalse);
    await store.close();
  });
  test('complete corruption is preserved and refuses writes', () async {
    final file = File('${root.path}/state.journal');
    await file.writeAsString('{"broken":true}\n');
    await expectLater(JournalStore.open(root), throwsA(isA<AppFailure>()));
    expect(await file.readAsString(), '{"broken":true}\n');
  });
  test('newer schema is blocked; v0 migration preserves unknown data', () async {
    final file = File('${root.path}/state.journal');
    Future<void> seed(int schema) async {
      final payload = jsonEncode({
        'schema': schema,
        'accounts': {},
        'preferences': {},
        'preserved': 'intent',
      });
      await file.writeAsString(
        '${jsonEncode({'sequence': 1, 'checksum': JournalStore.checksum(payload), 'payload': payload})}\n',
      );
    }

    await seed(3);
    final original = await file.readAsString();
    await expectLater(JournalStore.open(root), throwsA(isA<AppFailure>()));
    expect(await file.readAsString(), original);
    await seed(0);
    final store = await JournalStore.open(root);
    expect(store.read()['schema'], 2);
    expect(store.read()['preserved'], 'intent');
    expect(
      root.listSync().where((file) => file.path.contains('.schema0-')),
      hasLength(1),
    );
    await store.close();
  });
  test('schema 1 migration preserves saved records and old job identities with a backup', () async {
    final payload = jsonEncode({
      'schema': 1,
      'preferences': {'reduceMotion': true},
      'accounts': {
        const AccountScope.local().key: {
          'saved': {'ExactID': 'Saved title'},
          'jobs': {
            jsonEncode(['fixture', 'item']): {
              'kind': 'fixture',
              'item': 'item',
              'state': 'completed',
              'checkpoint': 4,
              'updatedAt': '2026-10-07T00:00:00Z',
            },
          },
        },
      },
    });
    final file = File('${root.path}/state.journal');
    await file.writeAsString(
      '${jsonEncode({'sequence': 1, 'checksum': JournalStore.checksum(payload), 'payload': payload})}\n',
    );
    final original = await file.readAsBytes();
    final store = await JournalStore.open(root);
    expect(store.read()['schema'], 2);
    expect(
      CollectionRepository(store).read(const AccountScope.local()).single.id,
      'ExactID',
    );
    expect(
      JobRepository(store).read(const AccountScope.local()).single.checkpoint,
      4,
    );
    expect(
      root.listSync().where((file) => file.path.contains('.schema1-')),
      hasLength(1),
    );
    final backup = root.listSync().singleWhere(
      (file) => file.path.contains('.schema1-'),
    );
    expect(await File(backup.path).readAsBytes(), original);
    await store.close();
  });
  test('second owner and linked journal are rejected', () async {
    final store = await JournalStore.open(root);
    await expectLater(JournalStore.open(root), throwsA(isA<AppFailure>()));
    await store.close();
    await File('${root.path}/state.journal').delete();
    final target = File('${root.path}/target');
    await target.writeAsString('unrelated data', flush: true);
    await Link('${root.path}/state.journal').create(target.path);
    await expectLater(JournalStore.open(root), throwsA(isA<AppFailure>()));
    expect(await target.readAsString(), 'unrelated data');
  });
  test('scope replacement cancels existing work synchronously', () {
    final session = SessionController();
    final token = session.beginWork();
    session.replace(const AccountScope.verified('identity', 'next'));
    expect(token.isCancelled, isTrue);
    expect(session.generation, 1);
    session.signOut();
    expect(session.scope.isLocal, isTrue);
    session.dispose();
  });
}
