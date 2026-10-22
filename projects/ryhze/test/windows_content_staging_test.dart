import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/platform/windows/windows_content_staging.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('test.content.staging');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final identity = '${'1' * 16}:${'2' * 32}';
  final source = '${'3' * 16}:${'4' * 32}';
  final hash = 'a' * 64;
  ContentStagingRequest request({
    bool bound = true,
    String path = 'folder/payload.bin',
    int size = 3,
    int minimumVerifiedBytes = 0,
  }) => ContentStagingRequest(
    slot: 0,
    operationId: hash,
    manifestSha256: hash,
    sourceRelative: path,
    size: size,
    minimumVerifiedBytes: minimumVerifiedBytes,
    sha256: hash,
    rootIdentity: bound ? identity : '',
    sourceIdentity: bound ? source : '',
  );
  Map<String, Object> probe() => {
    'schemaVersion': 1,
    'staging': true,
    'activation': false,
    'authenticity': false,
    'deviceId': identity,
  };
  Map<String, Object> receipt({String state = 'staged'}) => {
    'state': state,
    'verifiedBytes': state == 'staged' ? 3 : 0,
    'verifiedChunks': state == 'staged' ? 1 : 0,
    'rootIdentity': identity,
    'sourceIdentity': source,
  };
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test('probe requires exact inert capability and device binding', () async {
    for (final value in [
      {...probe(), 'authenticity': true},
      {...probe(), 'activation': true},
      {...probe(), 'extra': true},
      {...probe(), 'deviceId': ''},
      {...probe(), 'schemaVersion': 1.0},
      {...probe(), 'staging': false},
    ]) {
      messenger.setMockMethodCallHandler(channel, (_) async => value);
      expect(await WindowsContentStaging.connect(channel: channel), isNull);
    }
    messenger.setMockMethodCallHandler(channel, (_) async => probe());
    expect(
      (await WindowsContentStaging.connect(channel: channel))!.deviceId,
      identity,
    );
  });
  test(
    'strict descriptor maps only bounded identity and incoming-relative fields',
    () async {
      MethodCall? captured;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe();
        captured = call;
        return receipt();
      });
      final backend = (await WindowsContentStaging.connect(channel: channel))!;
      final result = await backend.stage(request(), CancellationToken());
      expect(result.state, ContentStagingState.staged);
      final args = captured!.arguments as Map;
      expect(args.keys.toSet(), {
        'slot',
        'operationId',
        'manifestSha256',
        'sourceRelative',
        'size',
        'minimumVerifiedBytes',
        'sha256',
        'rootIdentity',
        'sourceIdentity',
      });
      expect(args['sourceRelative'], 'folder/payload.bin');
      expect(args['minimumVerifiedBytes'], 0);
      expect(args['sha256'], Uint8List.fromList(List.filled(32, 170)));
      await expectLater(
        backend.stage(request(bound: false), CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      for (final path in [
        '../escape',
        '/absolute',
        r'folder\file',
        'CON.bin',
        'x/../y',
        'x.',
      ]) {
        expect(() => request(path: path), throwsA(isA<AppFailure>()));
      }
    },
  );
  test(
    'resume floor is bounded and carried to the native effect boundary',
    () async {
      const chunk = 256 * 1024;
      const total = chunk * 2 + 7;
      for (final floor in [-1, 1, chunk + 1, total + 1]) {
        expect(
          () => request(size: total, minimumVerifiedBytes: floor),
          throwsA(isA<AppFailure>()),
        );
      }
      for (final floor in [0, chunk, chunk * 2, total]) {
        expect(
          request(size: total, minimumVerifiedBytes: floor).minimumVerifiedBytes,
          floor,
        );
      }
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe();
        if (call.method == 'probeDiscard') {
          return {'schemaVersion': 1, 'discard': false};
        }
        calls.add(call);
        return receipt(state: call.method == 'stage' ? 'recovery' : 'checkpoint');
      });
      final backend = (await WindowsContentStaging.connect(channel: channel))!;
      final bound = request(minimumVerifiedBytes: 3);
      final failedResume = await backend.stage(bound, CancellationToken());
      expect(failedResume.state, ContentStagingState.recovery);
      expect(failedResume.verifiedBytes, 0);
      expect((calls.single.arguments as Map)['minimumVerifiedBytes'], 3);
      // An inspection must still disclose fewer retained bytes; it grants no
      // permission to refill that loss under the admitted floor.
      final observed = await backend.inspect(bound, CancellationToken());
      expect(observed.state, ContentStagingState.checkpoint);
      expect(observed.verifiedBytes, 0);
      expect((calls.last.arguments as Map)['minimumVerifiedBytes'], 3);
    },
  );
  test(
    'preflight allows empty identities but response remains bound to device',
    () async {
      var wrongRoot = false;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe();
        return {
          ...receipt(state: 'checkpoint'),
          if (wrongRoot) 'rootIdentity': source,
        };
      });
      final backend = (await WindowsContentStaging.connect(channel: channel))!;
      expect(
        (await backend.inspect(
          request(bound: false),
          CancellationToken(),
        )).state,
        ContentStagingState.checkpoint,
      );
      wrongRoot = true;
      await expectLater(
        backend.inspect(request(bound: false), CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
    },
  );
  test(
    'cancelled reply does not settle drain or permit shutdown completion',
    () async {
      final work = Completer<Object?>(), settled = Completer<Object?>();
      var cancelCalls = 0, drainCalls = 0, closeCalls = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        switch (call.method) {
          case 'probe':
            return probe();
          case 'stage':
            return work.future;
          case 'cancel':
            cancelCalls++;
            return null;
          case 'drain':
            drainCalls++;
            return settled.future;
          case 'close':
            closeCalls++;
            return settled.future;
        }
        return null;
      });
      final backend = (await WindowsContentStaging.connect(channel: channel))!;
      final token = CancellationToken();
      final running = backend.stage(request(), token);
      final failed = expectLater(
        running,
        throwsA(
          isA<AppFailure>().having(
            (e) => e.kind,
            'kind',
            FailureKind.cancelled,
          ),
        ),
      );
      await Future<void>.delayed(Duration.zero);
      token.cancel();
      work.complete(receipt());
      await failed;
      expect(cancelCalls, 1);
      var drained = false, closed = false;
      final drain = backend.drain().then((_) => drained = true);
      final secondDrain = backend.drain();
      final close = backend.close().then((_) => closed = true);
      await Future<void>.delayed(Duration.zero);
      expect(drainCalls, 1);
      expect(closeCalls, 1);
      expect(drained, isFalse);
      expect(closed, isFalse);
      await expectLater(
        backend.inspect(request(), CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      settled.complete();
      await drain;
      await secondDrain;
      await close;
      expect(drained && closed, isTrue);
    },
  );
  test(
    'drain errors remain errors rather than successful settlement',
    () async {
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe();
        throw PlatformException(code: 'recovery');
      });
      final backend = (await WindowsContentStaging.connect(channel: channel))!;
      await expectLater(backend.drain(), throwsA(isA<PlatformException>()));
      await expectLater(backend.close(), throwsA(isA<PlatformException>()));
    },
  );
  test(
    'malformed receipts fail closed and overlapping requests are rejected',
    () async {
      Object? response;
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'probe' ? probe() : response,
      );
      final backend = (await WindowsContentStaging.connect(channel: channel))!;
      for (final value in [
        {...receipt(), 'verifiedBytes': 4},
        {...receipt(), 'verifiedChunks': 0},
        {...receipt(), 'sourceIdentity': ''},
        {...receipt(), 'extra': true},
        {...receipt(), 'state': 'installed'},
        {
          ...receipt(state: 'recovery'),
          'verifiedBytes': 1,
          'verifiedChunks': 0,
        },
      ]) {
        response = value;
        await expectLater(
          backend.inspect(request(), CancellationToken()),
          throwsA(isA<AppFailure>()),
        );
      }
      final pending = Completer<Object?>();
      messenger.setMockMethodCallHandler(channel, (_) => pending.future);
      final first = backend.inspect(request(), CancellationToken());
      await expectLater(
        backend.inspect(request(), CancellationToken()),
        throwsA(
          isA<AppFailure>().having((e) => e.kind, 'kind', FailureKind.conflict),
        ),
      );
      pending.complete(receipt());
      await first;
    },
  );
}
