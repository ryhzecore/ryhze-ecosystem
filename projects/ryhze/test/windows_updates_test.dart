import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/platform/windows/windows_updates.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.tests/updates');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final calls = <MethodCall>[];
  bool available = false;
  setUp(() {
    calls.clear();
    available = false;
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'probe') {
        return {'schemaVersion': 1, 'staging': available, 'activation': false};
      }
      return {'state': 'staged', 'bytes': 3};
    });
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  Future<WindowsUpdateClient> connect() async =>
      (await WindowsUpdateClient.connect(channel: channel))!;
  test('missing publisher cannot stage or inspect', () async {
    final client = await connect();
    final token = CancellationToken();
    expect(
      (await client.stage(Uint8List(3), Uint8List(32), token)).state,
      WindowsUpdateStage.unavailable,
    );
    expect(
      (await client.inspect(Uint8List(32), 3, token)).state,
      WindowsUpdateStage.unavailable,
    );
    expect(calls.map((v) => v.method), ['probe']);
  });
  test('configured stage and inspect have no caller trust override', () async {
    available = true;
    final client = await connect();
    expect(
      (await client.stage(
        Uint8List.fromList([1, 2, 3]),
        Uint8List(32),
        CancellationToken(),
      )).state,
      WindowsUpdateStage.staged,
    );
    expect((calls.last.arguments as Map).keys, ['sha256', 'bytes']);
    expect(
      (await client.inspect(Uint8List(32), 3, CancellationToken())).bytes,
      3,
    );
    expect((calls.last.arguments as Map).keys, ['sha256', 'size']);
  });
  test('bounds, digest and cancellation are rejected before IPC', () async {
    available = true;
    final client = await connect();
    await expectLater(
      client.stage(Uint8List(0), Uint8List(32), CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
    await expectLater(
      client.inspect(Uint8List(31), 3, CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
    await expectLater(
      client.inspect(
        Uint8List(32),
        WindowsUpdateClient.maximumBytes + 1,
        CancellationToken(),
      ),
      throwsA(isA<AppFailure>()),
    );
    await expectLater(
      client.inspect(Uint8List(32), 3, CancellationToken()..cancel()),
      throwsA(isA<AppFailure>()),
    );
    expect(calls.length, 1);
  });
  test('malformed completions cannot mean verified staging', () async {
    available = true;
    final client = await connect();
    for (final response in [
      {'state': 'staged', 'bytes': 2},
      {'state': 'installed', 'bytes': 3},
      {'state': 'staged', 'bytes': 3, 'publisher': 'untrusted'},
      null,
    ]) {
      messenger.setMockMethodCallHandler(channel, (_) async => response);
      await expectLater(
        client.inspect(Uint8List(32), 3, CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
    }
  });
  test('cancelled completion does not claim rollback or replay', () async {
    available = true;
    final client = await connect();
    final completion = Completer<Object?>();
    final entered = Completer<void>();
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'cancel') return null;
      entered.complete();
      return completion.future;
    });
    final token = CancellationToken();
    final pending = client.stage(Uint8List(3), Uint8List(32), token);
    final rejected = expectLater(
      pending,
      throwsA(
        isA<AppFailure>().having((f) => f.kind, 'kind', FailureKind.cancelled),
      ),
    );
    await entered.future;
    token.cancel();
    completion.complete({'state': 'staged', 'bytes': 3});
    await rejected;
    expect(calls.map((v) => v.method), ['probe', 'stage', 'cancel']);
  });
  test(
    'worker cancellation and unknown completion retain safe typed failures',
    () async {
      available = true;
      final client = await connect();
      for (final entry in {
        'cancelled': FailureKind.cancelled,
        'recovery': FailureKind.recovery,
        'conflict': FailureKind.conflict,
        'invalidData': FailureKind.invalidData,
        'unexpected': FailureKind.unsupported,
      }.entries) {
        messenger.setMockMethodCallHandler(
          channel,
          (_) async => throw PlatformException(
            code: entry.key,
            message: 'private path/provider detail',
          ),
        );
        await expectLater(
          client.inspect(Uint8List(32), 3, CancellationToken()),
          throwsA(
            isA<AppFailure>()
                .having((e) => e.kind, 'kind', entry.value)
                .having(
                  (e) => e.toString(),
                  'safe message',
                  isNot(contains('private')),
                ),
          ),
        );
      }
    },
  );

  test('cancellation acknowledgement does not settle drain or close', () async {
    available = true;
    final client = await connect();
    final work = Completer<Object?>(), settled = Completer<Object?>();
    final entered = Completer<void>();
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'stage') {
        entered.complete();
        return work.future;
      }
      expect(call.arguments, isNull);
      if (call.method == 'cancel') {
        work.completeError(PlatformException(code: 'cancelled'));
        return null;
      }
      return settled.future;
    });
    final token = CancellationToken();
    final operation = client.stage(Uint8List(3), Uint8List(32), token);
    final rejected = expectLater(operation, throwsA(isA<AppFailure>()));
    await entered.future;
    token.cancel();
    await rejected;
    var drained = false, closed = false;
    final drain = client.drain().then((_) => drained = true);
    final duplicateDrain = client.drain();
    final close = client.close().then((_) => closed = true);
    await Future<void>.delayed(Duration.zero);
    expect(drained, isFalse);
    expect(closed, isFalse);
    expect(calls.where((call) => call.method == 'drain'), hasLength(1));
    await expectLater(
      client.inspect(Uint8List(32), 3, CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
    settled.complete(null);
    await Future.wait([drain, duplicateDrain, close]);
    expect(drained && closed, isTrue);
    await client.close();
    expect(calls.where((call) => call.method == 'close'), hasLength(1));
  });

  test('lifecycle recovery cannot reopen after a newer interruption', () async {
    available = true;
    final client = await connect();
    final settlement = Completer<Object?>();
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'drain') return settlement.future;
      return null;
    });
    client.interrupt();
    final resume = client.recoverAfterResume();
    final rejected = expectLater(
      resume,
      throwsA(
        isA<AppFailure>().having(
          (failure) => failure.kind,
          'kind',
          FailureKind.cancelled,
        ),
      ),
    );
    client.interrupt();
    settlement.complete(null);
    await rejected;
    await expectLater(
      client.inspect(Uint8List(32), 3, CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
    expect(calls.where((call) => call.method == 'inspect'), isEmpty);
    await client.recoverAfterResume();
    messenger.setMockMethodCallHandler(
      channel,
      (call) async =>
          call.method == 'inspect' ? {'state': 'staged', 'bytes': 3} : null,
    );
    expect(
      (await client.inspect(Uint8List(32), 3, CancellationToken())).state,
      WindowsUpdateStage.staged,
    );
    await client.close();
  });

  test(
    'failed or malformed drain keeps admission closed until recovery',
    () async {
      available = true;
      final client = await connect();
      for (final malformed in [false, true]) {
        messenger.setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          if (malformed) return {'settled': true};
          throw PlatformException(code: 'recovery', message: 'private details');
        });
        await expectLater(client.drain(), throwsA(isA<AppFailure>()));
        await expectLater(
          client.inspect(Uint8List(32), 3, CancellationToken()),
          throwsA(isA<AppFailure>()),
        );
      }
      expect(calls.where((call) => call.method == 'inspect'), isEmpty);
      messenger.setMockMethodCallHandler(channel, (_) async => null);
      await client.recoverAfterResume();
      await client.close();
    },
  );

  test('unconfigured lifecycle controls never infer a capability', () async {
    final client = await connect();
    client.interrupt();
    await client.recoverAfterResume();
    await client.cancel();
    await client.drain();
    await client.close();
    expect(calls.map((call) => call.method), ['probe']);
    await expectLater(
      client.stage(Uint8List(3), Uint8List(32), CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
  });
}
