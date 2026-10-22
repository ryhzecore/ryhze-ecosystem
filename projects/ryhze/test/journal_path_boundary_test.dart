import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/features/collections/collection_repository.dart';

import 'support/journal_fixture.dart';

void main() {
  late Directory root;
  setUp(() async {
    root = await createJournalFixture('ryhze-path-boundary-');
  });
  tearDown(() async {
    await root.delete(recursive: true);
  });

  for (final scenario in ['trailing', 'migration', 'compaction']) {
    for (final linked in [false, true]) {
      test('$scenario backup collision preserves bytes, link=$linked', () async {
        final journal = File('${root.path}/state.journal');
        final payload = jsonEncode({
          'schema': scenario == 'migration' ? 1 : 2,
          'accounts': {},
          'preferences': {'reduceMotion': true},
        });
        final record =
            '${jsonEncode({'sequence': 1, 'checksum': JournalStore.checksum(payload), 'payload': payload})}\n';
        await journal.writeAsString(
          scenario == 'compaction'
              ? '{interrupted replacement'
              : '$record${scenario == 'trailing' ? '{incomplete' : ''}',
        );
        final checkpoint = File('${journal.path}.checkpoint');
        if (scenario == 'compaction') {
          await checkpoint.writeAsString(record);
        }
        final original = await journal.readAsBytes();
        final sentinel = File('${root.path}/unrelated.txt');
        await sentinel.writeAsString('preserve existing bytes');
        final collision = _BackupCollision(
          '${journal.path}.${scenario == 'migration' ? 'schema1' : 'recovery'}-',
          sentinel.path,
          linked,
        );
        await expectLater(
          IOOverrides.runWithIOOverrides(() async {
            final opened = await JournalStore.open(root);
            await opened.close();
            return opened;
          }, collision),
          throwsA(isA<AppFailure>()),
        );
        expect(collision.destination, isNotNull);
        expect(await journal.readAsBytes(), original);
        expect(await sentinel.readAsString(), 'preserve existing bytes');
        expect(
          await File(collision.destination!).readAsString(),
          'preserve existing bytes',
        );
        if (scenario == 'compaction') {
          expect(await checkpoint.readAsString(), record);
        }
        // Failure releases the handle; a fresh non-colliding backup can recover.
        final reopened = await JournalStore.open(root);
        expect(PreferenceRepository(reopened).read().reduceMotion, isTrue);
        await reopened.close();
      });
    }
  }

  test(
    'OS temp alias resolves before fixture creation and durable reopen',
    () async {
      final physical = await Directory('${root.path}/physical').create();
      final alias = await Link('${root.path}/os-temp-alias')
          .create(physical.path);
      final fixture = await IOOverrides.runZoned(
        () => createJournalFixture('journal-'),
        getSystemTempDirectory: () => Directory(alias.path),
      );
      expect(fixture.parent.path, await physical.resolveSymbolicLinks());
      var store = await JournalStore.open(fixture);
      await PreferenceRepository(store)
          .save(const Preferences(reduceMotion: true));
      await store.close();
      store = await JournalStore.open(fixture);
      expect(PreferenceRepository(store).read().reduceMotion, isTrue);
      await store.close();

      final bytes = await File('${fixture.path}/state.journal').readAsBytes();
      final name = fixture.path.split(Platform.pathSeparator).last;
      await expectLater(
        JournalStore.open(Directory('${alias.path}/$name')),
        throwsA(
          isA<AppFailure>().having((e) => e.kind, 'kind', FailureKind.denied),
        ),
      );
      expect(await File('${fixture.path}/state.journal').readAsBytes(), bytes);
    },
  );

  test(
    'linked app root and intermediate directory cannot create or alter data',
    () async {
      final target = await Directory('${root.path}/target').create();
      final sentinel = File('${target.path}/unrelated.txt');
      await sentinel.writeAsString('preserve me', flush: true);
      final alias = await Link('${root.path}/app-owned-link')
          .create(target.path);
      for (final candidate in [alias.path, '${alias.path}/new-child']) {
        await expectLater(
          JournalStore.open(Directory(candidate)),
          throwsA(
            isA<AppFailure>().having((e) => e.kind, 'kind', FailureKind.denied),
          ),
        );
        expect(await sentinel.readAsString(), 'preserve me');
        final entries = await target.list().toList();
        expect(entries, hasLength(1));
        expect(
          await entries.single.resolveSymbolicLinks(),
          await sentinel.resolveSymbolicLinks(),
        );
      }
    },
  );

  test(
    'linked recovery files preserve committed journal and unrelated targets',
    () async {
      final store = await JournalStore.open(root);
      await PreferenceRepository(store)
          .save(const Preferences(reduceMotion: true));
      await store.close();
      final journal = File('${root.path}/state.journal');
      final committed = await journal.readAsBytes();
      final target = File('${root.path}/unrelated.txt');
      await target.writeAsString('not a recovery checkpoint', flush: true);
      for (final suffix in ['.checkpoint', '.checkpoint.tmp']) {
        final link = await Link('${journal.path}$suffix').create(target.path);
        await expectLater(JournalStore.open(root), throwsA(isA<AppFailure>()));
        expect(await journal.readAsBytes(), committed);
        expect(await target.readAsString(), 'not a recovery checkpoint');
        await link.delete();
      }
      final reopened = await JournalStore.open(root);
      expect(PreferenceRepository(reopened).read().reduceMotion, isTrue);
      await reopened.close();
    },
  );
}

final class _BackupCollision extends IOOverrides {
  _BackupCollision(this.prefix, this.target, this.linked);
  final String prefix;
  final String target;
  final bool linked;
  String? destination;

  @override
  File createFile(String path) {
    final file = super.createFile(path);
    if (destination == null &&
        path.replaceAll('\\', '/').startsWith(prefix.replaceAll('\\', '/'))) {
      destination = path;
      if (linked) {
        super.createLink(path).createSync(target);
      } else {
        file.writeAsStringSync('preserve existing bytes');
      }
    }
    return file;
  }
}
