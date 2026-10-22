import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/platform/windows/windows_security.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.tests/security');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const scope = AccountScope.verified('provider-case', 'Account-case');
  final probe = {
    'schemaVersion': 1,
    'secureCredentials': true,
    'fileIdentity': true,
  };
  final calls = <MethodCall>[];
  setUp(() {
    calls.clear();
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'probe') return probe;
      if (call.method == 'readCredential') return Uint8List.fromList([1, 2, 3]);
      return null;
    });
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  Future<WindowsSecurityClient> connect() async =>
      (await WindowsSecurityClient.connect(channel: channel))!;

  test('closed credential owner rejects further native work', () async {
    final session = SessionController()..replace(scope);
    final owner = WindowsCredentials(session, await connect());
    owner.dispose();
    session.dispose();
    await expectLater(owner.read(), throwsA(isA<AppFailure>()));
    await expectLater(
      owner.write(Uint8List.fromList([1])),
      throwsA(isA<AppFailure>()),
    );
    await expectLater(owner.delete(), throwsA(isA<AppFailure>()));
    expect(calls.length, 1);
  });

  test('host capability must have exact versioned schema', () async {
    for (final response in [
      null,
      {},
      {...probe, 'schemaVersion': 2},
      {...probe, 'extra': true},
      {...probe, 'secureCredentials': 'true'},
    ]) {
      messenger.setMockMethodCallHandler(channel, (_) async => response);
      expect(await WindowsSecurityClient.connect(channel: channel), isNull);
    }
  });
  test('exact credential namespace and opaque bytes cross channel', () async {
    final client = await connect();
    expect(await client.read(scope), [1, 2, 3]);
    await client.write(scope, Uint8List.fromList([0, 255]));
    await client.delete(scope);
    for (final call in calls.skip(1)) {
      expect(call.arguments['provider'], 'provider-case');
      expect(call.arguments['account'], 'Account-case');
    }
    expect(calls[2].arguments['value'], [0, 255]);
  });
  test(
    'guest, control characters and oversized credential rejected before IPC',
    () async {
      final client = await connect();
      for (final invalid in [
        const AccountScope.local(),
        const AccountScope.verified('a\n', 'b'),
        AccountScope.verified('a', '🪁' * 129),
      ]) {
        await expectLater(client.read(invalid), throwsA(isA<AppFailure>()));
      }
      await expectLater(
        client.write(scope, Uint8List(65537)),
        throwsA(isA<AppFailure>()),
      );
      expect(calls.length, 1);
    },
  );
  test('native failures do not leak exception strings', () async {
    final client = await connect();
    for (final code in ['denied', 'recovery', 'conflict', 'unknown-secret']) {
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => throw PlatformException(
          code: code,
          message: 'private-token-and-path',
        ),
      );
      try {
        await client.read(scope);
        fail('expected failure');
      } on AppFailure catch (error) {
        expect(error.toString(), isNot(contains('private')));
      }
    }
  });
  test('malformed native plaintext rejected', () async {
    final client = await connect();
    for (final value in [
      <int>[1],
      Uint8List(0),
      Uint8List(65537),
      'token',
    ]) {
      messenger.setMockMethodCallHandler(channel, (_) async => value);
      await expectLater(client.read(scope), throwsA(isA<AppFailure>()));
    }
  });
  test(
    'session replacement rejects late read and prevents queued stale write',
    () async {
      final client = await connect();
      final session = SessionController()..replace(scope);
      final credentials = WindowsCredentials(session, client);
      final pending = Completer<Uint8List>();
      final entered = Completer<void>();
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        entered.complete();
        return pending.future;
      });
      final read = credentials.read();
      final readCheck = expectLater(
        read,
        throwsA(
          isA<AppFailure>().having(
            (v) => v.kind,
            'kind',
            FailureKind.cancelled,
          ),
        ),
      );
      await entered.future;
      final write = credentials.write(Uint8List.fromList([9]));
      final writeCheck = expectLater(
        write,
        throwsA(
          isA<AppFailure>().having(
            (v) => v.kind,
            'kind',
            FailureKind.cancelled,
          ),
        ),
      );
      session.signOut();
      pending.complete(Uint8List.fromList([7]));
      await Future.wait([readCheck, writeCheck]);
      expect(calls.where((c) => c.method == 'writeCredential'), isEmpty);
      session.dispose();
    },
  );
  test(
    'file inspection rejects traversal and validates native identity',
    () async {
      final client = await connect();
      await expectLater(
        client.inspectFile(r'C:\Games', r'..\bad.exe'),
        throwsA(isA<AppFailure>()),
      );
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => {
          'identity': Uint8List(16),
          'volume': '123',
          'directory': false,
          'links': 1,
        },
      );
      final result = await client.inspectFile(r'C:\Games', r'game.exe');
      expect(result.volume, '123');
      expect(result.links, 1);
    },
  );
}
