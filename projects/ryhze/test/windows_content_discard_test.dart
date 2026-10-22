import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/features/installations/content_discard_backend.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/platform/windows/windows_content_staging.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('test.content.discard');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final root = '${'1' * 16}:${'2' * 32}';
  final source = '${'3' * 16}:${'4' * 32}';
  final plan = Uint8List.fromList([1, 2, 3]);
  Object? probe = {'schemaVersion': 1, 'discard': true};
  late Future<Object?> Function(MethodCall) operation;
  final calls = <String>[];
  ContentStagingRequest request() => ContentStagingRequest(
    slot: 0,
    operationId: 'a' * 64,
    manifestSha256: 'b' * 64,
    sourceRelative: 'payload.bin',
    size: 3,
    sha256: 'c' * 64,
    rootIdentity: root,
    sourceIdentity: source,
    minimumVerifiedBytes: 3,
  );
  Map<String, Object?> planned() => {
    'state': 'staged',
    'plan': plan,
    'verifiedBytes': 3,
    'verifiedChunks': 1,
  };
  Map<String, Object?> removed({String state = 'completed'}) => {
    'state': state,
    'removedBytes': 3,
    'removedFiles': 2,
  };
  setUp(() {
    calls.clear();
    probe = {'schemaVersion': 1, 'discard': true};
    operation = (call) async =>
        call.method == 'planDiscard' ? planned() : removed();
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call.method);
      if (call.method == 'probe') {
        return {
          'schemaVersion': 1,
          'staging': true,
          'activation': false,
          'authenticity': false,
          'deviceId': root,
        };
      }
      if (call.method == 'probeDiscard') return probe;
      if (['cancel', 'drain', 'close'].contains(call.method)) return null;
      return operation(call);
    });
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  Future<WindowsContentStaging> connect() async =>
      (await WindowsContentStaging.connect(channel: channel))!;

  test(
    'optional discard capability is strict and does not disable staging',
    () async {
      for (final value in [
        null,
        {'schemaVersion': 1.0, 'discard': true},
        {'schemaVersion': 1, 'discard': false},
        {'schemaVersion': 1, 'discard': true, 'extra': true},
      ]) {
        probe = value;
        final backend = await connect();
        expect(backend.deviceId, root);
        expect(backend.discardAvailable, isFalse);
        await expectLater(
          backend.planDiscard(request(), CancellationToken()),
          throwsA(isA<AppFailure>()),
        );
      }
      expect(calls.where((c) => c == 'planDiscard'), isEmpty);
    },
  );
  test(
    'exact plan descriptor and immutable response stay separate from execution',
    () async {
      operation = (call) async {
        expect(call.method, 'planDiscard');
        final args = call.arguments as Map;
        expect(args.length, 9);
        expect(args['minimumVerifiedBytes'], 3);
        expect(args['rootIdentity'], root);
        expect(args.containsKey('delete'), isFalse);
        return planned();
      };
      final backend = await connect();
      final receipt = await backend.planDiscard(request(), CancellationToken());
      expect(receipt.verifiedBytes, 3);
      expect(receipt.plan, plan);
      expect(() => receipt.plan![0] = 9, throwsUnsupportedError);
      expect(calls.contains('discard'), isFalse);
    },
  );
  test(
    'malformed plans or success below admitted prefix are rejected',
    () async {
      final backend = await connect();
      for (final value in [
        {...planned(), 'plan': Uint8List(0)},
        {...planned(), 'plan': Uint8List(contentDiscardMaximumPlanBytes + 1)},
        {
          ...planned(),
          'verifiedBytes': 0,
          'verifiedChunks': 0,
          'state': 'checkpoint',
        },
        {...planned(), 'verifiedChunks': 0},
        {...planned(), 'state': 'recovery'},
        {...planned(), 'extra': true},
        {...planned(), 'verifiedBytes': 3.0},
      ]) {
        operation = (_) async => value;
        await expectLater(
          backend.planDiscard(request(), CancellationToken()),
          throwsA(isA<AppFailure>()),
        );
      }
    },
  );
  test(
    'strict inspection and execution results cannot turn ready into completion',
    () async {
      final backend = await connect();
      operation = (call) async {
        expect((call.arguments as Map).keys, ['plan']);
        expect((call.arguments as Map)['plan'], plan);
        return removed(state: 'ready');
      };
      expect(
        (await backend.inspectDiscard(plan, CancellationToken())).state,
        ContentDiscardState.ready,
      );
      await expectLater(
        backend.discard(plan, CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      for (final value in [
        {...removed(), 'removedFiles': 4098},
        {...removed(), 'removedBytes': -1},
        {...removed(), 'removedBytes': 1073741825},
        {...removed(), 'state': 'deleted'},
        {...removed(), 'removedFiles': 2.0},
        {...removed(), 'extra': true},
      ]) {
        operation = (_) async => value;
        await expectLater(
          backend.discard(plan, CancellationToken()),
          throwsA(isA<AppFailure>()),
        );
      }
    },
  );
  test('stage and discard share single-operation admission and cancellation settlement', () async {
    final backend = await connect();
    final complete = Completer<Object?>();
    operation = (_) => complete.future;
    final token = CancellationToken();
    final running = backend.discard(plan, token);
    final expected = expectLater(
      running,
      throwsA(
        isA<AppFailure>().having((e) => e.kind, 'kind', FailureKind.cancelled),
      ),
    );
    await expectLater(
      backend.stage(request(), CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
    await expectLater(
      backend.inspectDiscard(plan, CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
    token.cancel();
    expect(calls, contains('cancel'));
    var settled = false;
    unawaited(expected.then((_) => settled = true));
    await Future<void>.delayed(Duration.zero);
    expect(settled, isFalse);
    complete.complete(removed());
    await expected;
    await backend.drain();
    await backend.close();
    await expectLater(
      backend.discard(plan, CancellationToken()),
      throwsA(isA<AppFailure>()),
    );
  });
  test('empty and oversized plans are refused before IPC; platform details are closed', () async {
    final backend = await connect();
    for (final invalid in [
      Uint8List(0),
      Uint8List(contentDiscardMaximumPlanBytes + 1),
    ]) {
      await expectLater(
        backend.discard(invalid, CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
    }
    expect(calls.contains('discard'), isFalse);
    operation = (_) async => throw PlatformException(
      code: 'unexpected',
      message: 'private path/token',
    );
    await expectLater(
      backend.discard(plan, CancellationToken()),
      throwsA(
        isA<AppFailure>().having(
          (e) => e.message,
          'safe message',
          isNot(contains('private')),
        ),
      ),
    );
  });
}
