import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/platform/windows/windows_artwork_cache.dart';

import 'artwork_fixture.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('test.ryhze.artwork');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  Matcher invalid = throwsA(
    isA<AppFailure>().having((e) => e.kind, 'kind', FailureKind.invalidData),
  );
  test(
    'channel supports fixed opaque keys, inert bytes and ordered drain',
    () async {
      final calls = <String>[];
      final now = DateTime.utc(2026, 10, 11);
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call.method);
        if (call.method == 'probe') return {'schemaVersion': 1, 'cache': true};
        if (call.method == 'get' || call.method == 'put') {
          expect(call.arguments['key'], artworkRef().key);
        } else {
          expect(call.arguments, isNull);
        }
        return switch (call.method) {
          'get' => {
            'bytes': artworkPng(),
            'storedAtMs': now.millisecondsSinceEpoch,
          },
          'health' => {'entries': 1, 'bytes': 70, 'preserved': 2},
          'clear' => {'removed': 1, 'preserved': 2},
          _ => null,
        };
      });
      final cache = (await WindowsArtworkCache.connect(channel: channel))!;
      await cache.put(artworkRef().key, artworkPng(), now);
      expect((await cache.get(artworkRef().key))!.storedAt, now);
      expect((await cache.health()).preserved, 2);
      expect((await cache.clear()).removed, 1);
      await cache.drain();
      expect(calls, ['probe', 'put', 'get', 'health', 'clear', 'drain']);
    },
  );
  test('missing or malformed probe stays unavailable', () async {
    for (final probe in [
      null,
      {'schemaVersion': 2, 'cache': true},
      {'schemaVersion': 1, 'cache': false},
      {'schemaVersion': 1, 'cache': true, 'unknown': true},
    ]) {
      messenger.setMockMethodCallHandler(channel, (_) async => probe);
      expect(await WindowsArtworkCache.connect(channel: channel), isNull);
    }
  });
  test('invalid inputs rejected before channel invocation', () async {
    var calls = 0;
    messenger.setMockMethodCallHandler(channel, (_) async {
      calls++;
      return null;
    });
    final cache = WindowsArtworkCache(channel: channel);
    for (final key in ['../image', 'a' * 63, '${'a' * 64}\n', 'A' * 64]) {
      await expectLater(cache.get(key), invalid);
    }
    await expectLater(
      cache.put('a' * 64, Uint8List(0), DateTime.now()),
      invalid,
    );
    await expectLater(
      cache.put('a' * 64, artworkPng(), DateTime.utc(2200)),
      invalid,
    );
    expect(calls, 0);
  });
  test(
    'malformed native values never become valid cached data or cleared status',
    () async {
      final cache = WindowsArtworkCache(channel: channel);
      for (final result in [
        [],
        {'bytes': [], 'storedAtMs': 0},
        {'bytes': Uint8List(0), 'storedAtMs': 0},
        {'bytes': artworkPng(), 'storedAtMs': -1},
      ]) {
        messenger.setMockMethodCallHandler(channel, (_) async => result);
        await expectLater(cache.get('a' * 64), invalid);
      }
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => {'entries': 65, 'bytes': 0, 'preserved': 0},
      );
      await expectLater(cache.health(), invalid);
      messenger.setMockMethodCallHandler(channel, (_) async => {'removed': 1});
      await expectLater(cache.clear(), invalid);
      messenger.setMockMethodCallHandler(channel, (_) async => true);
      await expectLater(cache.drain(), invalid);
    },
  );
  test(
    'native failure strings do not escape closed error vocabulary',
    () async {
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => throw PlatformException(
          code: 'io',
          message: 'private arbitrary path',
        ),
      );
      await expectLater(
        WindowsArtworkCache(channel: channel).health(),
        throwsA(
          isA<AppFailure>()
              .having((e) => e.kind, 'kind', FailureKind.storage)
              .having(
                (e) => e.toString(),
                'message',
                isNot(contains('private arbitrary')),
              ),
        ),
      );
    },
  );
}
