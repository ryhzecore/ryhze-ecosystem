import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/data/store_schema.dart';

import 'support/journal_fixture.dart';

List<int> _record(Map<String, Object?> data, [int sequence = 1]) {
  final payload = jsonEncode(data);
  return utf8.encode(
    '${jsonEncode({'sequence': sequence, 'checksum': JournalStore.checksum(payload), 'payload': payload})}\n',
  );
}

Map<String, Object?> _snapshot({int schema = currentStoreSchema}) => {
  'schema': schema,
  'accounts': <String, Object?>{},
  'preferences': <String, Object?>{},
};

Future<void> _openAndClose(Directory root) async {
  final store = await JournalStore.open(root);
  await store.close();
}

void main() {
  late Directory root;
  late File journal;
  setUp(() async {
    root = await createJournalFixture('ryhze-journal-boundary-');
    journal = File('${root.path}/state.journal');
  });
  tearDown(() async => root.delete(recursive: true));

  for (final prefix in [
    '',
    utf8.decode(_record(_snapshot())),
    '{malformed}\n',
    '{"sequence":1,"checksum":0,"payload":"{}"}\n',
  ]) {
    for (final trailing in [false, true]) {
      test(
        'older checkpoint cannot replace a newer main schema, prefix=$prefix trailing=$trailing',
        () async {
          final original = [
            ...utf8.encode(prefix),
            ..._record(
              _snapshot(schema: currentStoreSchema + 1),
              prefix.isEmpty ? 1 : 9,
            ),
            if (trailing) ...utf8.encode('{interrupted'),
          ];
          final checkpoint = File('${journal.path}.checkpoint');
          final temporary = File('${checkpoint.path}.tmp');
          final recovery = _record(_snapshot());
          await journal.writeAsBytes(original);
          await checkpoint.writeAsBytes(recovery);
          await temporary.writeAsString('unfinished temporary');
          await expectLater(_openAndClose(root), throwsA(isA<AppFailure>()));
          expect(await journal.readAsBytes(), original);
          expect(await checkpoint.readAsBytes(), recovery);
          expect(await temporary.readAsString(), 'unfinished temporary');
          expect(root.listSync(), hasLength(3));
        },
      );
    }
  }

  for (final point in [
    'temporary',
    'checkpoint',
    'quarantine',
    'quarantineWithCheckpoint',
  ]) {
    for (final linked in [false, true]) {
      test(
        '$point collision preserves main and existing destination, link=$linked',
        () async {
          final sentinel = File('${root.path}/sentinel');
          await sentinel.writeAsString('unrelated existing content');
          final temporary = File('${journal.path}.checkpoint.tmp');
          final target = point == 'temporary'
              ? temporary.path
              : point == 'checkpoint'
              ? '${journal.path}.checkpoint'
              : '${temporary.path}.recovery-';
          final collision = _SideFileCollision(
            target,
            sentinel.path,
            linked: linked,
            prefix: point.startsWith('quarantine'),
          );
          late List<int> original;
          if (point.startsWith('quarantine')) {
            original = _record(_snapshot());
            await journal.writeAsBytes(original);
            await temporary.writeAsString('unfinished temporary');
            final checkpoint = File('${journal.path}.checkpoint');
            final checkpointBytes = _record(_snapshot()..['preserved'] = true);
            if (point == 'quarantineWithCheckpoint') {
              await checkpoint.writeAsBytes(checkpointBytes);
            }
            await expectLater(
              IOOverrides.runWithIOOverrides(
                () => _openAndClose(root),
                collision,
              ),
              throwsA(isA<AppFailure>()),
            );
            expect(await temporary.readAsString(), 'unfinished temporary');
            if (point == 'quarantineWithCheckpoint') {
              expect(await checkpoint.readAsBytes(), checkpointBytes);
            }
          } else {
            final data = _snapshot()..['padding'] = 'x' * 320;
            original = [];
            for (var sequence = 1; ; sequence++) {
              final next = _record(data, sequence);
              if (original.length + next.length > 2048) break;
              original.addAll(next);
            }
            await journal.writeAsBytes(original);
            final store = await JournalStore.open(root, byteLimit: 2048);
            try {
              await expectLater(
                IOOverrides.runWithIOOverrides(
                  () => store.transact((draft) => draft['next'] = true),
                  collision,
                ),
                throwsA(isA<AppFailure>()),
              );
              expect(store.read(), data);
              await expectLater(
                store.transact((draft) => draft['afterFailure'] = true),
                throwsA(isA<AppFailure>()),
              );
            } finally {
              await store.close();
            }
          }
          expect(collision.destination, isNotNull);
          expect(await journal.readAsBytes(), original);
          expect(await sentinel.readAsString(), 'unrelated existing content');
          expect(
            await File(collision.destination!).readAsString(),
            'unrelated existing content',
          );
        },
      );
    }
  }

  for (final point in ['temporary', 'checkpoint', 'quarantine']) {
    test(
      'interrupted $point copy preserves the original main journal',
      () async {
        final temporary = File('${journal.path}.checkpoint.tmp');
        final checkpoint = File('${journal.path}.checkpoint');
        final target = point == 'temporary'
            ? temporary.path
            : point == 'checkpoint'
            ? checkpoint.path
            : '${temporary.path}.recovery-';
        final failure = _CopyFailure(target, prefix: point == 'quarantine');
        late List<int> original;
        if (point == 'quarantine') {
          original = _record(_snapshot());
          await journal.writeAsBytes(original);
          await temporary.writeAsString('unfinished temporary');
          await expectLater(
            IOOverrides.runWithIOOverrides(() => _openAndClose(root), failure),
            throwsA(isA<AppFailure>()),
          );
          expect(await temporary.readAsString(), 'unfinished temporary');
        } else {
          final data = _snapshot()..['padding'] = 'x' * 320;
          original = [];
          for (var sequence = 1; ; sequence++) {
            final next = _record(data, sequence);
            if (original.length + next.length > 2048) break;
            original.addAll(next);
          }
          await journal.writeAsBytes(original);
          final store = await JournalStore.open(root, byteLimit: 2048);
          try {
            await expectLater(
              IOOverrides.runWithIOOverrides(
                () => store.transact((draft) => draft['next'] = true),
                failure,
              ),
              throwsA(isA<AppFailure>()),
            );
            expect(store.read(), data);
            await expectLater(
              store.transact((draft) => draft['afterFailure'] = true),
              throwsA(isA<AppFailure>()),
            );
          } finally {
            await store.close();
          }
        }
        expect(failure.destination, isNotNull);
        expect(await File(failure.destination!).length(), greaterThan(0));
        expect(await journal.readAsBytes(), original);
        if (point == 'checkpoint') {
          final partial = await checkpoint.readAsBytes();
          final staged = await temporary.readAsBytes();
          await expectLater(_openAndClose(root), throwsA(isA<AppFailure>()));
          expect(await journal.readAsBytes(), original);
          expect(await checkpoint.readAsBytes(), partial);
          expect(await temporary.readAsBytes(), staged);
        } else {
          final partial = await File(failure.destination!).readAsBytes();
          await _openAndClose(root);
          expect(await journal.readAsBytes(), original);
          if (point == 'temporary') {
            expect(await temporary.exists(), isFalse);
            final archived = root.listSync().singleWhere(
              (entry) => entry.path.contains('.tmp.recovery-'),
            );
            expect(await File(archived.path).readAsBytes(), partial);
          } else {
            expect(await File(failure.destination!).readAsBytes(), partial);
          }
        }
      },
    );
  }

  for (final field in [
    'authority',
    'account',
    'jobKind',
    'jobItem',
    'favouriteProvider',
    'favouriteItem',
    'activityReference',
  ]) {
    test('persisted $field rejects every C0 control without repair', () async {
      for (var code = 0; code < 32; code++) {
        final bad = 'before${String.fromCharCode(code)}after';
        final data = _snapshot();
        final authority = field == 'authority' ? bad : 'provider';
        final account = field == 'account' ? bad : 'account';
        final accountData = <String, Object?>{};
        (data['accounts'] as Map)[jsonEncode([authority, account])] =
            accountData;
        if (field.startsWith('job')) {
          final kind = field == 'jobKind' ? bad : 'fixture';
          final item = field == 'jobItem' ? bad : 'item';
          accountData['jobs'] = {
            jsonEncode([kind, item]): {
              'kind': kind,
              'item': item,
              'state': 'queued',
              'checkpoint': 0,
              'updatedAt': '2026-10-10T00:00:00Z',
            },
          };
        } else if (field.startsWith('favourite')) {
          accountData['libraryFavourites'] = [
            jsonEncode([
              field == 'favouriteProvider' ? bad : 'provider',
              field == 'favouriteItem' ? bad : 'item',
            ]),
          ];
        } else if (field == 'activityReference') {
          accountData['activity'] = {
            jsonEncode([bad, 'completed']): {
              'kind': 'completed',
              'count': 1,
              'acknowledged': false,
              'occurredAt': '2026-10-10T00:00:00Z',
            },
          };
        }
        final bytes = _record(data);
        await journal.writeAsBytes(bytes);
        await expectLater(
          _openAndClose(root),
          throwsA(isA<AppFailure>()),
          reason: 'control=$code',
        );
        expect(await journal.readAsBytes(), bytes);
        expect(root.listSync(), hasLength(1));
      }
    });
  }
}

/// Inject the destination immediately before create/write/rename, after the
/// production existence checks. This is deterministic collision coverage, not
/// proof against hostile path substitution after exclusive reservation.
final class _SideFileCollision extends IOOverrides {
  _SideFileCollision(
    this.target,
    this.sentinel, {
    required this.linked,
    this.prefix = false,
  });
  final String target;
  final String sentinel;
  final bool linked;
  final bool prefix;
  String? destination;

  void inject(String path) {
    final normalized = path.replaceAll('\\', '/');
    final match = target.replaceAll('\\', '/');
    if (destination != null ||
        !(prefix ? normalized.startsWith(match) : normalized == match)) {
      return;
    }
    destination = path;
    if (linked) {
      super.createLink(path).createSync(sentinel);
    } else {
      super.createFile(path).writeAsStringSync('unrelated existing content');
    }
  }

  @override
  File createFile(String path) =>
      _InterceptFile(super.createFile(path), inject);
}

final class _InterceptFile implements File {
  _InterceptFile(this.file, this.beforeWrite);
  final File file;
  final void Function(String) beforeWrite;
  @override
  String get path => file.path;
  @override
  Future<bool> exists() => file.exists();
  @override
  Future<int> length() => file.length();
  @override
  Future<Uint8List> readAsBytes() => file.readAsBytes();
  @override
  Future<File> create({bool recursive = false, bool exclusive = false}) {
    beforeWrite(path);
    return file.create(recursive: recursive, exclusive: exclusive);
  }

  @override
  Future<File> writeAsBytes(
    List<int> bytes, {
    FileMode mode = FileMode.write,
    bool flush = false,
  }) {
    beforeWrite(path);
    return file.writeAsBytes(bytes, mode: mode, flush: flush);
  }

  @override
  Future<File> rename(String newPath) {
    beforeWrite(newPath);
    return file.rename(newPath);
  }

  @override
  Future<RandomAccessFile> open({FileMode mode = FileMode.read}) =>
      file.open(mode: mode);
  @override
  Future<FileSystemEntity> delete({bool recursive = false}) =>
      file.delete(recursive: recursive);
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

final class _CopyFailure extends IOOverrides {
  _CopyFailure(this.target, {required this.prefix});
  final String target;
  final bool prefix;
  String? destination;

  bool shouldFail(String path) {
    final normalized = path.replaceAll('\\', '/');
    final match = target.replaceAll('\\', '/');
    if (destination != null ||
        !(prefix ? normalized.startsWith(match) : normalized == match)) {
      return false;
    }
    destination = path;
    return true;
  }

  @override
  File createFile(String path) =>
      _CopyFailureFile(super.createFile(path), shouldFail);
}

final class _CopyFailureFile extends _InterceptFile {
  _CopyFailureFile(File file, this.shouldFail) : super(file, (_) {});
  final bool Function(String) shouldFail;

  @override
  Future<File> writeAsBytes(
    List<int> bytes, {
    FileMode mode = FileMode.write,
    bool flush = false,
  }) async {
    if (shouldFail(path)) {
      await file.writeAsBytes(
        bytes.take(bytes.length ~/ 2).toList(),
        mode: mode,
        flush: true,
      );
      throw const FileSystemException('Injected interruption during copy');
    }
    return file.writeAsBytes(bytes, mode: mode, flush: flush);
  }
}
