import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/store_schema.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/features/collections/collection_repository.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/library/data/library_preferences.dart';
import 'package:ryhze/features/library/library_controller.dart';
import 'package:ryhze/features/library/model/library_entry.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/platform/apple/apple_platform_client.dart';
import 'package:ryhze/platform/apple/apple_transaction_store.dart';

/// Simulates the versioned native byte transport, not SQLite or Keychain.
/// Real persistence and process-restart acceptance belong to Runner XCTest.
class _NativeTransport {
  int revision = 0;
  Uint8List? bytes;
  bool denyWrites = false;

  Future<Object?> call(MethodCall call) async {
    switch (call.method) {
      case 'bootstrap':
        return {
          'schemaVersion': 1,
          'platform': 'ios',
          'secureCredentials': true,
          'durableState': true,
        };
      case 'state.read':
        return {'revision': revision, 'value': bytes};
      case 'state.compareAndSet':
        if (denyWrites) {
          throw PlatformException(
            code: 'ioFailure',
            message: 'synthetic private native diagnostic',
          );
        }
        final args = call.arguments as Map;
        if (args['expectedRevision'] != revision) {
          throw PlatformException(code: 'conflict');
        }
        bytes = Uint8List.fromList(args['value'] as Uint8List);
        revision++;
        return {'revision': revision, 'value': bytes};
      default:
        throw MissingPluginException();
    }
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.apple.shared-integration-test');
  const client = ApplePlatformClient(channel: channel);
  late _NativeTransport native;
  setUp(() {
    native = _NativeTransport();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, native.call);
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  test(
    'shared repositories reopen Apple bytes without account or rights leakage',
    () async {
      const first = AccountScope.verified('fixture', 'Account');
      const second = AccountScope.verified('fixture', 'account');
      const favourite = LibraryIdentity(
        providerId: 'fixture',
        titleId: 'Title',
      );
      var services = AppServices(
        storage: await AppleTransactionStore.open(client: client),
      );
      services.session.replace(first);
      await services.collections.setSaved(
        first,
        const SavedTitle('Title', 'Saved fixture'),
        true,
      );
      await LibraryPreferences(services.storage, first).save({favourite});
      await services.savePreferences(const Preferences(reduceMotion: true));
      final job = await services.jobs.enqueue(
        first,
        'fixture',
        'Title',
        intentId: 'fixture-intent-1',
      );
      await services.jobs.transition(
        first,
        job,
        JobState.running,
        checkpoint: 42,
      );
      services.session.signOut();
      expect(services.collections.read(services.session.scope), isEmpty);
      expect(services.jobs.read(services.session.scope), isEmpty);
      expect(services.library.isFavourite(favourite), isFalse);
      await services.close();

      final stored = jsonDecode(utf8.decode(native.bytes!)) as Map;
      expect(stored['schema'], currentStoreSchema);
      expect((stored['accounts'] as Map).keys, [first.key]);
      services = AppServices(
        storage: await AppleTransactionStore.open(client: client),
      );
      services.session.replace(second);
      expect(services.collections.read(second), isEmpty);
      expect(services.jobs.read(second), isEmpty);
      expect(services.activity.read(second), isEmpty);
      expect(services.library.isFavourite(favourite), isFalse);
      services.session.replace(first);
      expect(services.collections.read(first).single.id, 'Title');
      expect(services.library.isFavourite(favourite), isTrue);
      expect(services.preferences.read().reduceMotion, isTrue);
      await services.recoverAfterResume();
      expect(services.jobs.read(first).single.state, JobState.recoveryRequired);
      expect(services.jobs.read(first).single.checkpoint, 42);
      expect(services.jobs.read(first).single.intentId, 'fixture-intent-1');
      expect(services.activity.read(first).single.kind, ActivityKind.recovery);
      expect(services.activity.read(second), isEmpty);
      await services.library.load();
      expect(services.library.status, LibraryStatus.unavailable);
      expect(services.library.entries, isEmpty);
      expect(services.coordinator.operations, isEmpty);
      await services.close();
    },
  );

  test(
    'failed native commit leaves shared data intact and requires reopening',
    () async {
      final services = AppServices(
        storage: await AppleTransactionStore.open(client: client),
      );
      const scope = AccountScope.local();
      await services.collections.setSaved(
        scope,
        const SavedTitle('first', 'Preserved fixture'),
        true,
      );
      final committed = Uint8List.fromList(native.bytes!);
      native.denyWrites = true;
      await expectLater(
        services.collections.setSaved(
          scope,
          const SavedTitle('second', 'Rejected fixture'),
          true,
        ),
        throwsA(
          isA<AppFailure>().having(
            (error) => error.message,
            'safe message',
            isNot(contains('private native diagnostic')),
          ),
        ),
      );
      expect(services.collections.read(scope).map((item) => item.id), [
        'first',
      ]);
      expect(native.bytes, committed);
      native.denyWrites = false;
      await expectLater(
        services.savePreferences(const Preferences(reduceMotion: true)),
        throwsA(isA<AppFailure>()),
      );
      expect(native.bytes, committed);
      await services.close();
      final reopened = await AppleTransactionStore.open(client: client);
      expect(CollectionRepository(reopened).read(scope).single.id, 'first');
      await reopened.close();
    },
  );
}
