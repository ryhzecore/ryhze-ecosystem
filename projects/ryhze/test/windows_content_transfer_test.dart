import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/features/installations/content_transfer_backend.dart';
import 'package:ryhze/platform/windows/windows_content_transfer.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('test.content.transfer');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final root = '${'1' * 16}:${'2' * 32}';
  final directory = '${'3' * 16}:${'4' * 32}';
  final source = '${'5' * 16}:${'6' * 32}';
  final hash = 'a' * 64;
  ContentTransferRequest request({
    int attempt = 0,
    int observed = 0,
    bool identified = false,
    bool writing = false,
  }) => ContentTransferRequest(
    slot: 0,
    operationId: hash,
    expectedSize: 3,
    expectedSha256: hash,
    attempt: attempt,
    rootIdentity: root,
    directoryIdentity: directory,
    sourceIdentity: identified || writing ? source : '',
    observedBytes: observed,
    observedSha256: identified ? hash : '',
  );
  Map<String, Object> probe() => {
    'schemaVersion': 1,
    'transfer': true,
    'activation': false,
    'authenticity': false,
    'deviceId': root,
  };
  Map<String, Object> receipt({
    String state = 'writing',
    int count = 0,
    int attempt = 0,
  }) => {
    'state': state,
    'rootIdentity': root,
    'directoryIdentity': directory,
    'sourceIdentity':
        state == 'complete' ||
            state == 'checkpoint' ||
            state == 'discarded' ||
            state == 'writing'
        ? source
        : '',
    'sourceRelative': 'transfer-0/$hash-$attempt.bin',
    'observedBytes': count,
    'observedSha256':
        state == 'complete' || state == 'checkpoint' || state == 'discarded'
        ? hash
        : '',
  };
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test('probe requires exact inert native capability and identity', () async {
    for (final value in [
      {...probe(), 'activation': true},
      {...probe(), 'authenticity': true},
      {...probe(), 'schemaVersion': 1.0},
      {...probe(), 'transfer': false},
      {...probe(), 'deviceId': ''},
      {...probe(), 'extra': 1},
    ]) {
      messenger.setMockMethodCallHandler(channel, (_) async => value);
      expect(await WindowsContentTransfer.connect(channel: channel), isNull);
    }
  });
  test(
    'bounded descriptor has no arbitrary path, root or executable authority',
    () async {
      MethodCall? captured;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe();
        captured = call;
        return receipt();
      });
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      expect(
        (await backend.begin(request(), CancellationToken())).state,
        ContentTransferState.writing,
      );
      final args = captured!.arguments as Map;
      expect(args.keys.toSet(), {'request', 'resume'});
      expect((args['request'] as Map).keys.toSet(), {
        'slot',
        'attempt',
        'operationId',
        'expectedSize',
        'expectedSha256',
        'rootIdentity',
        'directoryIdentity',
        'sourceIdentity',
        'observedBytes',
        'observedSha256',
      });
      expect(
        (args['request'] as Map)['expectedSha256'],
        Uint8List.fromList(List.filled(32, 170)),
      );
    },
  );
  test(
    'append receipts must account for exact new bytes and stay inert',
    () async {
      var value = receipt(count: 3);
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'probe' ? probe() : value,
      );
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      expect(
        (await backend.append(
          request(observed: 1, writing: true),
          Uint8List.fromList([2, 3]),
          CancellationToken(),
        )).observedBytes,
        3,
      );
      for (final bad in [
        receipt(count: 2),
        receipt(state: 'complete', count: 3),
        {...receipt(count: 3), 'sourceIdentity': ''},
        {...receipt(count: 3), 'rootIdentity': directory},
        {...receipt(count: 3), 'sourceRelative': '../escape'},
      ]) {
        value = bad;
        await expectLater(
          backend.append(
            request(observed: 1, writing: true),
            Uint8List.fromList([2, 3]),
            CancellationToken(),
          ),
          throwsA(isA<AppFailure>()),
        );
      }
    },
  );
  test(
    'complete requires same size hash source and directory identity',
    () async {
      var value = receipt(state: 'complete', count: 3);
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'probe' ? probe() : value,
      );
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      expect(
        (await backend.finish(
          request(observed: 3, writing: true),
          CancellationToken(),
        )).state,
        ContentTransferState.complete,
      );
      for (final bad in [
        receipt(state: 'complete', count: 2),
        {...receipt(state: 'complete', count: 3), 'observedSha256': 'b' * 64},
        {...receipt(state: 'complete', count: 3), 'sourceIdentity': ''},
        {...receipt(state: 'complete', count: 3), 'directoryIdentity': source},
      ]) {
        value = bad;
        await expectLater(
          backend.finish(
            request(observed: 3, writing: true),
            CancellationToken(),
          ),
          throwsA(isA<AppFailure>()),
        );
      }
    },
  );
  test(
    'resume transmits exact old observation and expects replayed byte count',
    () async {
      MethodCall? captured;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe();
        captured = call;
        return receipt(count: 2, attempt: 1);
      });
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      final old = request(observed: 2, identified: true);
      final result = await backend.begin(
        request(attempt: 1),
        CancellationToken(),
        resume: old,
      );
      expect(result.observedBytes, 2);
      expect(
        ((captured!.arguments as Map)['resume'] as Map)['sourceIdentity'],
        source,
      );
      await expectLater(
        backend.begin(request(), CancellationToken(), resume: old),
        throwsA(isA<AppFailure>()),
      );
    },
  );
  test(
    'discard requires durable observed identity size and prefix digest',
    () async {
      var calls = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe();
        calls++;
        return receipt(state: 'discarded', count: 2);
      });
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      await expectLater(
        backend.discard(request(observed: 2), CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      expect(calls, 0);
      expect(
        (await backend.discard(
          request(observed: 2, identified: true),
          CancellationToken(),
        )).state,
        ContentTransferState.discarded,
      );
    },
  );
  test(
    'oversized chunks and bytes past expected size rejected before host',
    () async {
      var calls = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls++;
        return probe();
      });
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      for (final bytes in [
        Uint8List(0),
        Uint8List(4),
        Uint8List(256 * 1024 + 1),
      ]) {
        await expectLater(
          backend.append(request(), bytes, CancellationToken()),
          throwsA(isA<AppFailure>()),
        );
      }
      expect(calls, 1);
    },
  );
  test(
    'cancel does not claim drain and drain bars concurrent admission',
    () async {
      final work = Completer<Object?>(), drain = Completer<Object?>();
      final calls = <String>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call.method);
        return switch (call.method) {
          'probe' => probe(),
          'inspect' => work.future,
          'drain' => drain.future,
          _ => null,
        };
      });
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      final cancellation = CancellationToken();
      final operation = backend.inspect(request(), cancellation);
      final checked = expectLater(
        operation,
        throwsA(
          isA<AppFailure>().having(
            (e) => e.kind,
            'kind',
            FailureKind.cancelled,
          ),
        ),
      );
      cancellation.cancel();
      final settled = backend.drain();
      var finished = false;
      unawaited(
        settled.then((_) {
          finished = true;
        }),
      );
      await Future<void>.delayed(Duration.zero);
      expect(calls, contains('cancel'));
      expect(finished, false);
      work.complete(receipt(state: 'checkpoint', count: 2));
      await checked;
      await expectLater(
        backend.begin(request(), CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      drain.complete();
      await settled;
      expect(finished, true);
    },
  );
  test(
    'closed boundary is permanent and close awaits native settlement',
    () async {
      final completion = Completer<Object?>();
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe();
        return await completion.future;
      });
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      final closing = backend.close();
      var closed = false;
      unawaited(closing.then((_) => closed = true));
      await expectLater(
        backend.inspect(request(), CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      await Future<void>.delayed(Duration.zero);
      expect(closed, isFalse);
      completion.complete();
      await closing;
      expect(closed, isTrue);
    },
  );
  test('native error strings cannot become user-visible diagnostics', () async {
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.method == 'probe') return probe();
      throw PlatformException(code: 'recovery', message: 'secret local path');
    });
    final backend = (await WindowsContentTransfer.connect(channel: channel))!;
    await expectLater(
      backend.inspect(request(), CancellationToken()),
      throwsA(
        isA<AppFailure>()
            .having((e) => e.kind, 'kind', FailureKind.recovery)
            .having(
              (e) => e.toString(),
              'safe message',
              isNot(contains('secret')),
            ),
      ),
    );
  });
  test(
    'inspection cannot adopt a filename without recorded creation identity',
    () async {
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'probe'
            ? probe()
            : receipt(state: 'checkpoint', count: 2),
      );
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      await expectLater(
        backend.inspect(request(), CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      expect(
        (await backend.inspect(
          request(writing: true),
          CancellationToken(),
        )).state,
        ContentTransferState.checkpoint,
      );
    },
  );
  test(
    'known unhashed creation can reconcile absence but preserves host refusal',
    () async {
      var present = false;
      var calls = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe();
        calls++;
        if (present) throw PlatformException(code: 'recovery');
        return {...receipt(state: 'discarded'), 'observedSha256': ''};
      });
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      final result = await backend.discard(
        request(writing: true),
        CancellationToken(),
      );
      expect(result.state, ContentTransferState.discarded);
      expect(result.sourceIdentity, source);
      expect(result.observedBytes, 0);
      expect(result.observedSha256, isEmpty);
      present = true;
      await expectLater(
        backend.discard(request(writing: true), CancellationToken()),
        throwsA(
          isA<AppFailure>().having((e) => e.kind, 'kind', FailureKind.recovery),
        ),
      );
      expect(calls, 2);
    },
  );
  test(
    'inspection accepts absent directory only in a missing receipt',
    () async {
      var result = {...receipt(state: 'missing'), 'directoryIdentity': ''};
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'probe' ? probe() : result,
      );
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      final plan = request(writing: true);
      final missing = await backend.inspect(plan, CancellationToken());
      expect(missing.state, ContentTransferState.missing);
      expect(missing.directoryIdentity, isEmpty);
      expect(missing.sourceIdentity, isEmpty);
      expect(plan.directoryIdentity, directory);
      expect(plan.sourceIdentity, source);
      for (final bad in [
        {...receipt(state: 'missing'), 'directoryIdentity': source},
        {...receipt(state: 'checkpoint'), 'directoryIdentity': ''},
        {...receipt(state: 'missing'), 'sourceIdentity': source},
      ]) {
        result = bad;
        await expectLater(
          backend.inspect(plan, CancellationToken()),
          throwsA(isA<AppFailure>()),
        );
      }
    },
  );
  test(
    'uncreated reservation discard can acknowledge only zero absent evidence',
    () async {
      var result = {
        ...receipt(state: 'discarded'),
        'sourceIdentity': '',
        'observedSha256': '',
      };
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'probe' ? probe() : result,
      );
      final backend = (await WindowsContentTransfer.connect(channel: channel))!;
      expect(
        (await backend.discard(request(), CancellationToken())).state,
        ContentTransferState.discarded,
      );
      result = receipt(state: 'discarded', count: 2);
      await expectLater(
        backend.discard(request(), CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
    },
  );
}
