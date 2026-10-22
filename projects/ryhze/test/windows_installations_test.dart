import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/platform/windows/windows_installations.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.test.installation.inventory');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final now = DateTime.utc(2026, 10, 10);
  Map<String, Object> item() => {
    'appId': '123',
    'title': 'Synthetic game',
    'libraryRoot': r'C:\SyntheticSteam',
    'relativeDirectory': r'steamapps\common\Synthetic Game',
    'stateFlags': '4',
  };
  const probe = {
    'schemaVersion': 1,
    'readOnlyInventory': true,
    'ownership': false,
    'launch': false,
  };
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  test(
    'inventory is an exact provider observation without ownership conversion',
    () async {
      final calls = <String>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call.method);
        expect(call.arguments, isNull);
        return call.method == 'probe'
            ? probe
            : {
                'quality': 'complete',
                'items': [item()],
              };
      });
      final result = await WindowsInstallationRepository(
        channel: channel,
        clock: () => now,
      ).inspect(CancellationToken());
      expect(result.quality, ReadQuality.complete);
      expect(result.observedAt, now);
      expect(result.values.single.item, ProviderItemRef('steam', '123'));
      expect(result.values.single.providerState, '4');
      expect(calls, ['probe', 'inspectSteam']);
      expect(() => result.values.clear(), throwsUnsupportedError);
    },
  );
  test('unavailable differs from a complete empty scan', () async {
    for (final quality in ['complete', 'partial', 'unavailable']) {
      messenger.setMockMethodCallHandler(
        channel,
        (call) async =>
            call.method == 'probe' ? probe : {'quality': quality, 'items': []},
      );
      final result = await WindowsInstallationRepository(channel: channel)
          .inspect(CancellationToken());
      expect(result.quality.name, quality);
      expect(result.failure == null, quality == 'complete');
    }
  });
  test('rejects malformed identity, traversal, duplicates and authority escalation', () async {
    for (final invalid in [
      [item()..['appId'] = '0123'],
      [item()..['appId'] = '4294967296'],
      [item(), item()],
      [item()..['relativeDirectory'] = r'steamapps\common\..'],
      [item()..['relativeDirectory'] = r'steamapps\common\Game\..\outside'],
      [item()..['libraryRoot'] = r'\\server\share'],
      [item()..['owned'] = true],
      List.generate(1001, (_) => item()),
    ]) {
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'probe'
            ? probe
            : {'quality': 'complete', 'items': invalid},
      );
      await expectLater(
        WindowsInstallationRepository(channel: channel)
            .inspect(CancellationToken()),
        throwsA(
          isA<AppFailure>().having(
            (error) => error.kind,
            'kind',
            FailureKind.invalidData,
          ),
        ),
      );
    }
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => {...probe, 'ownership': true},
    );
    await expectLater(
      WindowsInstallationRepository(channel: channel)
          .inspect(CancellationToken()),
      throwsA(
        isA<AppFailure>().having(
          (error) => error.kind,
          'kind',
          FailureKind.unsupported,
        ),
      ),
    );
  });
  test(
    'cancellation requests native stop and suppresses late observations',
    () async {
      final started = Completer<void>();
      final completion = Completer<Object?>();
      var cancelled = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'probe') return probe;
        if (call.method == 'cancel') {
          cancelled++;
          return null;
        }
        started.complete();
        return completion.future;
      });
      final repository = WindowsInstallationRepository(channel: channel);
      final token = CancellationToken();
      final pending = repository.inspect(token);
      await started.future;
      await expectLater(
        repository.inspect(CancellationToken()),
        throwsA(
          isA<AppFailure>().having(
            (error) => error.kind,
            'kind',
            FailureKind.conflict,
          ),
        ),
      );
      token.cancel();
      final assertion = expectLater(
        pending,
        throwsA(
          isA<AppFailure>().having(
            (error) => error.kind,
            'kind',
            FailureKind.cancelled,
          ),
        ),
      );
      completion.complete({
        'quality': 'complete',
        'items': [item()],
      });
      await assertion;
      await Future<void>.delayed(Duration.zero);
      expect(cancelled, 1);
    },
  );
  test('connect probes exact capability without starting discovery', () async {
    final methods = <String>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      methods.add(call.method);
      return probe;
    });
    expect(
      await WindowsInstallationRepository.connect(channel: channel),
      isNotNull,
    );
    expect(methods, ['probe']);
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => {...probe, 'launch': true},
    );
    expect(
      await WindowsInstallationRepository.connect(channel: channel),
      isNull,
    );
  });
}
