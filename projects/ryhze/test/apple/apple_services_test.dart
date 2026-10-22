import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/store_schema.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/collections/collection_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/library/data/library_preferences.dart';
import 'package:ryhze/features/library/model/library_entry.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/platform/apple/apple_credentials.dart';
import 'package:ryhze/platform/apple/apple_platform_client.dart';
import 'package:ryhze/platform/apple/apple_resource_policy.dart';
import 'package:ryhze/platform/apple/apple_transaction_store.dart';

const channel = MethodChannel('com.ryhze.ryhze/apple.v1');
const client = ApplePlatformClient();
const guest = AccountScope.local();
const accountA = AccountScope.verified('provider', 'a');
const accountB = AccountScope.verified('provider', 'b');
const title = LibraryIdentity(providerId: 'provider', titleId: 'title');

StoreData blank() => {
  'schema': currentStoreSchema,
  'accounts': <String, Object?>{},
  'preferences': <String, Object?>{},
};
Uint8List jsonBytes(Object value) =>
    Uint8List.fromList(utf8.encode(jsonEncode(value)));
Matcher fails(FailureKind kind) =>
    throwsA(isA<AppFailure>().having((error) => error.kind, 'kind', kind));

/// Models the native byte/CAS protocol only. Native disk and Keychain acceptance
/// is covered by the separate Apple host tests, not this in-memory transport.
class NativeFixture {
  int revision = 0;
  Uint8List? bytes;
  String? writeFailure;
  int writes = 0;
  int migrations = 0;
  String? migrationFailure;
  final backups = <int, ({int schema, Uint8List bytes})>{};
  Completer<void>? writeGate;
  Completer<Object?>? credentialGate;
  final calls = <MethodCall>[];
  final credentials = <String, Uint8List>{};

  Future<Object?> handle(MethodCall call) async {
    calls.add(call);
    final args = call.arguments as Map? ?? {};
    switch (call.method) {
      case 'bootstrap':
        return {
          'schemaVersion': 1,
          'platform': 'ios',
          'secureCredentials': true,
          'durableState': true,
        };
      case 'state.read':
        expect(args, isEmpty);
        return {
          'revision': revision,
          'value': bytes == null ? null : Uint8List.fromList(bytes!),
        };
      case 'state.compareAndSet':
        expect(args.keys.toSet(), {'expectedRevision', 'value'});
        expect(args['value'], isA<Uint8List>());
        writes++;
        final gate = writeGate;
        writeGate = null;
        if (gate != null) await gate.future;
        if (writeFailure != null) {
          throw PlatformException(
            code: writeFailure!,
            message: 'private native details',
          );
        }
        if (args['expectedRevision'] != revision) {
          throw PlatformException(code: 'conflict');
        }
        if (bytes != null && jsonDecode(utf8.decode(bytes!))['schema'] != 2) {
          throw PlatformException(code: 'unsupportedSchema');
        }
        bytes = Uint8List.fromList(args['value'] as Uint8List);
        revision++;
        return {'revision': revision, 'value': Uint8List.fromList(bytes!)};
      case 'state.migrate':
        expect(args.keys.toSet(), {
          'expectedRevision',
          'expectedValue',
          'value',
        });
        migrations++;
        if (args['expectedRevision'] != revision ||
            !listEquals(args['expectedValue'] as Uint8List, bytes)) {
          throw PlatformException(code: 'conflict');
        }
        final schema = jsonDecode(utf8.decode(bytes!))['schema'] as int;
        expect({0, 1}.contains(schema), true);
        expect(
          jsonDecode(utf8.decode(args['value'] as Uint8List))['schema'],
          2,
        );
        // Staged values model a single native SQL transaction. A throw before
        // commit must expose neither its backup nor its replacement snapshot.
        if (migrationFailure == 'beforeBackup') {
          throw PlatformException(code: 'ioFailure');
        }
        final backup = (schema: schema, bytes: Uint8List.fromList(bytes!));
        if (migrationFailure == 'afterBackup') {
          throw PlatformException(code: 'ioFailure');
        }
        final replacement = Uint8List.fromList(args['value'] as Uint8List);
        if (migrationFailure == 'beforeCommit') {
          throw PlatformException(code: 'ioFailure');
        }
        if (backups.containsKey(revision)) {
          throw PlatformException(code: 'corruptState');
        }
        backups[revision] = backup;
        bytes = replacement;
        revision++;
        if (migrationFailure == 'afterCommit') {
          throw PlatformException(code: 'ioFailure');
        }
        return {'revision': revision, 'value': Uint8List.fromList(bytes!)};
      case 'credentials.read':
        if (credentialGate != null) return credentialGate!.future;
        return credentials[args['scope']];
      case 'credentials.write':
        credentials[args['scope'] as String] = Uint8List.fromList(
          args['value'] as Uint8List,
        );
        return null;
      case 'credentials.delete':
        credentials.remove(args['scope']);
        return null;
      default:
        throw MissingPluginException();
    }
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  late NativeFixture native;
  setUp(() {
    native = NativeFixture();
    messenger.setMockMethodCallHandler(channel, native.handle);
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  group('shared TransactionStore on Apple', () {
    test(
      'uses shared repositories with durable account and title isolation',
      () async {
        final storage = await AppleTransactionStore.open();
        final saved = CollectionRepository(storage);
        await saved.setSaved(accountA, const SavedTitle('one', 'One'), true);
        await saved.setSaved(accountB, const SavedTitle('two', 'Two'), true);
        await LibraryPreferences(storage, accountA).save({title});
        await PreferenceRepository(storage)
            .save(const Preferences(reduceMotion: true));
        expect(storage.durable, true);
        expect(saved.read(guest), isEmpty);
        expect(saved.read(accountA).single.id, 'one');
        expect(saved.read(accountB).single.id, 'two');
        expect(LibraryPreferences(storage, accountB).read(), isEmpty);
        await storage.close();
        final reopened = await AppleTransactionStore.open();
        addTearDown(reopened.close);
        expect(CollectionRepository(reopened).read(accountA).single.id, 'one');
        expect(LibraryPreferences(reopened, accountA).read(), {title});
        expect(PreferenceRepository(reopened).read().reduceMotion, true);
        expect(native.credentials, isEmpty);
      },
    );

    test(
      'raw bytes retain Unicode-distinct account keys and unknown fields',
      () async {
        final data = blank();
        const composed = AccountScope.verified('provider', '\u00e9');
        const decomposed = AccountScope.verified('provider', 'e\u0301');
        data['accounts'] = {
          composed.key: {
            'saved': {'a': 'Composed'},
          },
          decomposed.key: {
            'saved': {'b': 'Decomposed'},
          },
        };
        data['futureMetadata'] = {
          'original': ['keep', 7],
        };
        native.bytes = jsonBytes(data);
        native.revision = 2;
        final storage = await AppleTransactionStore.open();
        addTearDown(storage.close);
        await PreferenceRepository(storage)
            .save(const Preferences(privatePreviews: false));
        final restored = jsonDecode(utf8.decode(native.bytes!)) as Map;
        expect((restored['accounts'] as Map).length, 2);
        expect(
          (restored['accounts'] as Map)[composed.key],
          data['accounts'] is Map
              ? (data['accounts'] as Map)[composed.key]
              : null,
        );
        expect(restored['futureMetadata'], data['futureMetadata']);
        expect(CollectionRepository(storage).read(decomposed).single.id, 'b');
      },
    );

    test(
      'serial transactions and detached reads cannot lose or mutate data',
      () async {
        final storage = await AppleTransactionStore.open();
        addTearDown(storage.close);
        StoreData? retainedDraft;
        final one = storage.transact((draft) {
          retainedDraft = draft;
          draft['first'] = 1;
        });
        final two = storage.transact((draft) {
          draft['second'] = 2;
        });
        await Future.wait([one, two]);
        retainedDraft!['first'] = 999;
        final detached = storage.read();
        (detached['preferences'] as Map)['reduceMotion'] = true;
        expect(storage.read()['first'], 1);
        expect(storage.read()['second'], 2);
        expect(storage.read()['preferences'], isEmpty);
      },
    );

    test(
      'invalid callback draft never reaches native and does not poison writes',
      () async {
        final storage = await AppleTransactionStore.open();
        addTearDown(storage.close);
        await expectLater(
          storage.transact((draft) {
            draft['accounts'] = 'invalid';
          }),
          fails(FailureKind.invalidData),
        );
        expect(native.writes, 0);
        await storage.transact((draft) {
          draft['valid'] = true;
        });
        expect(storage.read()['valid'], true);
      },
    );

    test(
      'CAS conflict preserves committed state and never replays callback',
      () async {
        final left = await AppleTransactionStore.open();
        final right = await AppleTransactionStore.open();
        addTearDown(left.close);
        addTearDown(right.close);
        await left.transact((draft) {
          draft['winner'] = true;
        });
        var callbacks = 0;
        await expectLater(
          right.transact((draft) {
            callbacks++;
            draft['loser'] = true;
          }),
          fails(FailureKind.conflict),
        );
        await expectLater(
          right.transact((draft) {
            callbacks++;
          }),
          fails(FailureKind.storage),
        );
        expect(callbacks, 1);
        expect(jsonDecode(utf8.decode(native.bytes!))['winner'], true);
        expect(right.read().containsKey('loser'), false);
      },
    );

    test(
      'uncertain write failure requires reopen without a success fallback',
      () async {
        final storage = await AppleTransactionStore.open();
        native.writeFailure = 'ioFailure';
        await expectLater(
          storage.transact((draft) {
            draft['change'] = true;
          }),
          fails(FailureKind.storage),
        );
        expect(storage.read().containsKey('change'), false);
        native.writeFailure = null;
        await expectLater(
          storage.transact((draft) {
            draft['change'] = true;
          }),
          fails(FailureKind.storage),
        );
        await storage.close();
        final reopened = await AppleTransactionStore.open();
        addTearDown(reopened.close);
        await reopened.transact((draft) {
          draft['change'] = true;
        });
        expect(reopened.read()['change'], true);
      },
    );

    test('close drains accepted writes and fences new transactions', () async {
      final storage = await AppleTransactionStore.open();
      final gate = native.writeGate = Completer<void>();
      final write = storage.transact((draft) {
        draft['saved'] = true;
      });
      await Future<void>.delayed(Duration.zero);
      final close = storage.close();
      await expectLater(
        storage.transact((draft) {
          draft['late'] = true;
        }),
        fails(FailureKind.storage),
      );
      gate.complete();
      await Future.wait([write, close]);
      expect(() => storage.read(), throwsA(isA<AppFailure>()));
      expect(jsonDecode(utf8.decode(native.bytes!))['saved'], true);
    });

    test(
      'existing shared recovery coordinator persists interrupted jobs',
      () async {
        final storage = await AppleTransactionStore.open();
        addTearDown(storage.close);
        final jobs = JobRepository(storage);
        final coordinator = JobCoordinator(jobs, const {});
        addTearDown(coordinator.close);
        final job = await jobs.enqueue(accountA, 'refresh', 'owned');
        await jobs.transition(accountA, job, JobState.running, checkpoint: 3);
        await coordinator.recover(accountA);
        expect(jobs.read(accountA).single.state, JobState.recoveryRequired);
        expect(jobs.read(accountA).single.checkpoint, 3);
        final count = native.writes;
        await coordinator.recover(accountA);
        expect(native.writes, count);
        expect(jobs.read(accountB), isEmpty);
      },
    );

    for (final schema in [0, 1]) {
      test(
        'schema $schema migration preserves original and shared content',
        () async {
          const composed = AccountScope.verified('provider', '\u00e9');
          const decomposed = AccountScope.verified('provider', 'e\u0301');
          final legacyJobKey = jsonEncode(['refresh', 'owned']);
          final legacy = {
            ...blank(),
            'schema': schema,
            'unknownTopLevel': {
              'keep': [3, true, 'unchanged'],
            },
            'preferences': {'reduceMotion': true, 'futureFlag': 'keep'},
            'accounts': {
              composed.key: {
                'saved': {'one': 'Composed'},
                'libraryFavourites': [
                  jsonEncode(['provider', 'title']),
                ],
                'unknownAccountField': {'keep': true},
                'jobs': {
                  legacyJobKey: {
                    'kind': 'refresh',
                    'item': 'owned',
                    'state': 'running',
                    'checkpoint': 4,
                    'updatedAt': '2026-10-06T11:12:13.000Z',
                  },
                },
              },
              decomposed.key: {
                'saved': {'two': 'Decomposed'},
              },
            },
          };
          final original = Uint8List.fromList(
            utf8.encode(
              '${const JsonEncoder.withIndent('  ').convert(legacy)}\n',
            ),
          );
          native.bytes = Uint8List.fromList(original);
          native.revision = 7;
          final storage = await AppleTransactionStore.open();
          expect(storage.read(), {...legacy, 'schema': currentStoreSchema});
          expect(native.backups[7]?.bytes, original);
          expect(native.backups[7]?.schema, schema);
          expect(native.revision, 8);
          expect(native.migrations, 1);
          expect(native.writes, 0);
          expect(
            JobRepository(storage).read(composed).single.key,
            legacyJobKey,
          );
          expect(
            JobRepository(storage).read(composed).single.state,
            JobState.running,
          );
          expect(
            CollectionRepository(storage).read(decomposed).single.id,
            'two',
          );
          await storage.close();
          final reopened = await AppleTransactionStore.open();
          addTearDown(reopened.close);
          expect(native.migrations, 1);
          await PreferenceRepository(reopened).save(const Preferences());
          expect(native.backups.length, 1);
          expect(native.backups[7]?.bytes, original);
          expect(reopened.read()['unknownTopLevel'], legacy['unknownTopLevel']);
        },
      );
    }

    for (final failure in ['beforeBackup', 'afterBackup', 'beforeCommit']) {
      test(
        'interrupted migration at $failure exposes no partial state',
        () async {
          final original = jsonBytes({...blank(), 'schema': 1, 'keep': true});
          native.bytes = Uint8List.fromList(original);
          native.revision = 3;
          native.migrationFailure = failure;
          await expectLater(
            AppleTransactionStore.open(),
            fails(FailureKind.storage),
          );
          expect(native.bytes, original);
          expect(native.revision, 3);
          expect(native.backups, isEmpty);
          native.migrationFailure = null;
          final reopened = await AppleTransactionStore.open();
          addTearDown(reopened.close);
          expect(reopened.read()['keep'], true);
          expect(native.backups[3]?.bytes, original);
          expect(native.revision, 4);
        },
      );
    }

    test(
      'lost migration reply reopens committed state without a second backup',
      () async {
        final original = jsonBytes({...blank(), 'schema': 1, 'keep': true});
        native.bytes = Uint8List.fromList(original);
        native.revision = 3;
        native.migrationFailure = 'afterCommit';
        await expectLater(
          AppleTransactionStore.open(),
          fails(FailureKind.storage),
        );
        expect(native.revision, 4);
        expect(native.backups[3]?.bytes, original);
        native.migrationFailure = null;
        final reopened = await AppleTransactionStore.open();
        addTearDown(reopened.close);
        expect(reopened.read()['schema'], currentStoreSchema);
        expect(reopened.read()['keep'], true);
        expect(native.migrations, 1);
        expect(native.backups.length, 1);
      },
    );

    test('migration checks exact original bytes as well as revision', () async {
      native.bytes = jsonBytes({...blank(), 'schema': 1});
      native.revision = 3;
      final previous = await client.readState();
      final winner = jsonBytes({...blank(), 'schema': 1, 'external': true});
      native.bytes = winner;
      await expectLater(
        client.migrateState(previous),
        throwsA(
          isA<ApplePlatformException>().having(
            (error) => error.code,
            'code',
            'conflict',
          ),
        ),
      );
      expect(native.bytes, winner);
      expect(native.revision, 3);
      expect(native.backups, isEmpty);
    });

    test(
      'invalid legacy records fail before migration and preserve original',
      () async {
        final original = jsonBytes({
          ...blank(),
          'schema': 1,
          'accounts': {
            accountA.key: {
              'jobs': {'invalid': {}},
            },
          },
        });
        native.bytes = original;
        native.revision = 2;
        await expectLater(
          AppleTransactionStore.open(),
          fails(FailureKind.recovery),
        );
        expect(native.bytes, original);
        expect(native.migrations, 0);
        expect(native.backups, isEmpty);
      },
    );

    test('schema 2 retains explicit job intent, safe failure and activity on reopen', () async {
      final storage = await AppleTransactionStore.open();
      final jobs = JobRepository(storage);
      final job = await jobs.enqueue(
        accountA,
        'refresh',
        'owned',
        intentId: 'intent-2',
      );
      await jobs.transition(accountA, job, JobState.running, checkpoint: 5);
      await jobs.transition(
        accountA,
        job,
        JobState.failed,
        failure: FailureKind.network,
      );
      await storage.close();
      final reopened = await AppleTransactionStore.open();
      addTearDown(reopened.close);
      final restored = JobRepository(reopened).read(accountA).single;
      expect(restored.key, jsonEncode(['refresh', 'owned', 'intent-2']));
      expect(restored.intentId, 'intent-2');
      expect(restored.failure, FailureKind.network);
      expect(restored.checkpoint, 5);
      expect(JobRepository(reopened).read(accountB), isEmpty);
      final account = (reopened.read()['accounts'] as Map)[accountA.key] as Map;
      expect((account['activity'] as Map).values.single['kind'], 'failed');
    });

    for (final schema in [-1, 3, 1.0, 2.0]) {
      test(
        'schema $schema fails closed without migration or replacement',
        () async {
          native.bytes = jsonBytes({...blank(), 'schema': schema});
          native.revision = 7;
          final original = Uint8List.fromList(native.bytes!);
          await expectLater(
            AppleTransactionStore.open(),
            fails(FailureKind.recovery),
          );
          expect(native.bytes, original);
          expect(native.writes, 0);
          expect(native.migrations, 0);
          expect(native.backups, isEmpty);
        },
      );
    }

    test(
      'malformed native bytes and shared records fail before exposure',
      () async {
        native.bytes = Uint8List.fromList([0xff, 0xff]);
        native.revision = 1;
        await expectLater(
          AppleTransactionStore.open(),
          fails(FailureKind.recovery),
        );
        native.bytes = jsonBytes({
          ...blank(),
          'accounts': {'not-json': {}},
        });
        await expectLater(
          AppleTransactionStore.open(),
          fails(FailureKind.recovery),
        );
        expect(native.writes, 0);
      },
    );

    test(
      'invalid surrogate identities cannot collapse across UTF8 transport',
      () async {
        final storage = await AppleTransactionStore.open();
        addTearDown(storage.close);
        await expectLater(
          storage.transact((draft) {
            (draft['accounts'] as Map)[const AccountScope.verified(
                  'p',
                  '\ud800',
                ).key] =
                {};
          }),
          fails(FailureKind.invalidData),
        );
        expect(native.writes, 0);
      },
    );

    test(
      'missing bridge remains unavailable and never claims durable memory',
      () async {
        messenger.setMockMethodCallHandler(channel, null);
        await expectLater(
          AppleTransactionStore.open(),
          fails(FailureKind.storage),
        );
      },
    );
  });

  group('Keychain adapter uses the shared session', () {
    test('scopes preserve exact provider/account case and Unicode', () {
      final scopes = [
        accountA,
        accountB,
        const AccountScope.verified('Provider', 'a'),
        const AccountScope.verified('provider', '\u00e9'),
        const AccountScope.verified('provider', 'e\u0301'),
        const AccountScope.verified('a:b', 'c'),
        const AccountScope.verified('a', 'b:c'),
      ];
      expect(
        scopes.map(ApplePlatformClient.credentialScopeKey).toSet(),
        hasLength(scopes.length),
      );
      expect(
        jsonDecode(
          utf8.decode(
            base64Url.decode(ApplePlatformClient.credentialScopeKey(accountA)),
          ),
        ),
        ['provider', 'a'],
      );
    });

    test('local and malformed scopes never reach native credentials', () async {
      await expectLater(
        client.writeCredential(guest, Uint8List.fromList([1])),
        fails(FailureKind.authentication),
      );
      await expectLater(
        client.readCredential(const AccountScope.verified('p', '\ud800')),
        fails(FailureKind.invalidData),
      );
      expect(native.calls, isEmpty);
    });

    test(
      'typed credential bytes use exact shared scope and session slot',
      () async {
        final session = SessionController()..replace(accountA);
        addTearDown(session.dispose);
        final credentials = AppleCredentials(session);
        final input = Uint8List.fromList([1, 2, 3]);
        final write = credentials.write(input);
        input[0] = 9;
        await write;
        expect(await credentials.read(), [1, 2, 3]);
        expect((native.calls.first.arguments as Map)['slot'], 'session');
        session.replace(accountB);
        expect(await credentials.read(), isNull);
      },
    );

    test(
      'late credential result and queued write are cancelled on switch',
      () async {
        final session = SessionController()..replace(accountA);
        addTearDown(session.dispose);
        final credentials = AppleCredentials(session);
        final gate = native.credentialGate = Completer<Object?>();
        final read = credentials.read();
        final checkRead = expectLater(read, fails(FailureKind.cancelled));
        await Future<void>.delayed(Duration.zero);
        final write = credentials.write(Uint8List.fromList([1]));
        final checkWrite = expectLater(write, fails(FailureKind.cancelled));
        session.replace(accountB);
        gate.complete(Uint8List.fromList([9]));
        await Future.wait([checkRead, checkWrite]);
        expect(native.credentials, isEmpty);
      },
    );

    test('locked read is not absence and error details are redacted', () async {
      messenger.setMockMethodCallHandler(channel, (call) async {
        throw PlatformException(
          code: 'locked',
          message: 'private diagnostic',
          details: 'private value',
        );
      });
      await expectLater(
        client.readCredential(accountA),
        throwsA(
          isA<ApplePlatformException>()
              .having((error) => error.code, 'code', 'locked')
              .having(
                (error) => error.toString(),
                'message',
                isNot(contains('private')),
              ),
        ),
      );
    });
  });

  group('resource boundaries', () {
    final policy = AppleResourcePolicy(allowedHosts: ['media.ryhze.com']);
    test('default denies network and desktop executable schemes', () {
      final none = AppleResourcePolicy();
      for (final value in [
        'https://media.ryhze.com/a',
        'steam://run/1',
        'file:///a.exe',
        'javascript:alert(1)',
        'data:text/plain,x',
      ]) {
        expect(none.allowsExternalUri(Uri.parse(value)), false);
      }
    });
    test(
      'HTTPS allowlist rejects host tricks, control encoding and traversal',
      () {
        expect(
          policy.allowsNetworkUri(
            Uri.parse('https://media.ryhze.com/cover.webp'),
          ),
          true,
        );
        for (final value in [
          'http://media.ryhze.com/a',
          'https://media.ryhze.com.evil.com/a',
          'https://media.ryhze.com@evil.com/a',
          'https://user@media.ryhze.com/a',
          'https://media.ryhze.com:8443/a',
          'https://media.ryhze.com/a#fragment',
          'https://127.0.0.1/a',
          'https://[::1]/a',
          'https://media.ryhze.com/a%2fb',
          'https://media.ryhze.com/%252e%252e/a',
          'https://media.ryhze.com/a?x=%0a',
        ]) {
          expect(
            policy.allowsNetworkUri(Uri.parse(value)),
            false,
            reason: value,
          );
        }
      },
    );
    test('relative files reject traversal and bundled media is explicit', () {
      expect(
        policy.requireRelativeFilePath('cache/cover.webp'),
        'cache/cover.webp',
      );
      for (final value in [
        '',
        '/etc/passwd',
        '../escape',
        'a/../b',
        'a//b',
        'a\\b',
        'C:/a',
        'a/%2e%2e',
        'a\u0000b',
      ]) {
        expect(
          () => policy.requireRelativeFilePath(value),
          throwsA(isA<ApplePolicyException>()),
        );
      }
      expect(policy.allowsBundledMedia('assets/brand/symbol-480.webp'), true);
      expect(policy.allowsBundledMedia('assets/brand/wordmark-348.webp'), true);
      expect(policy.allowsBundledMedia('assets/brand/payload.exe'), false);
    });
  });
}
