import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/app_route.dart';
import 'package:ryhze/platform/windows/windows_activation.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.tests/activation');
  const codec = StandardMethodCodec();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final acks = <int>[];
  var initial = <Object?>[];
  Future<Object?> event(Object? value) async {
    final reply = await messenger.handlePlatformMessage(
      channel.name,
      codec.encodeMethodCall(MethodCall('activate', value)),
      null,
    );
    return reply == null ? null : codec.decodeEnvelope(reply);
  }

  setUp(() {
    acks.clear();
    initial = [];
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.method == 'connect') {
        return {
          'schemaVersion': 1,
          'singleInstance': true,
          'activations': initial,
        };
      }
      if (call.method == 'ack') {
        acks.add(call.arguments['sequence'] as int);
        return null;
      }
      throw MissingPluginException();
    });
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  Future<WindowsActivationClient> connect() async =>
      (await WindowsActivationClient.connect(channel: channel))!;
  test('cold, warm, replay and focus are ordered and acknowledged', () async {
    initial = [
      {'sequence': 1, 'uri': 'ryhze://app/games/library'},
    ];
    final client = await connect();
    await event({'sequence': 2, 'uri': null});
    await event(initial.first);
    expect(client.pending.length, 2);
    expect(
      client.pending.first.route!.destination,
      AppDestination.gamesLibrary,
    );
    await client.acknowledge(client.pending.last);
    expect(acks, isEmpty);
    await client.acknowledge(client.pending.first);
    await event(initial.first);
    expect(client.pending.length, 1);
    await client.acknowledge(client.pending.first);
    expect(acks, [1, 2]);
    client.dispose();
  });
  test(
    'invalid routes, oversized envelopes and conflicting replay rejected',
    () async {
      final client = await connect();
      for (final value in [
        {'sequence': 1, 'uri': 'ryhze://app/install?exe=bad'},
        {'sequence': 1, 'uri': 'ryhze://app/games/store?provider=evil'},
        {'sequence': 0, 'uri': null},
        {'sequence': 1, 'uri': null, 'run': 'cmd'},
      ]) {
        await expectLater(event(value), throwsA(isA<PlatformException>()));
      }
      await event({'sequence': 1, 'uri': 'ryhze://app/games/store'});
      await expectLater(
        event({'sequence': 1, 'uri': 'ryhze://app/films/store'}),
        throwsA(isA<PlatformException>()),
      );
      expect(client.pending.length, 1);
      client.dispose();
    },
  );
  test('unavailable native host leaves browsing available', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (_) async => throw MissingPluginException(),
    );
    expect(await WindowsActivationClient.connect(channel: channel), isNull);
  });
  test('open overlay retains cold activation until it closes', () async {
    initial = [
      {'sequence': 1, 'uri': 'ryhze://app/games/library'},
    ];
    final app = AppServices(storage: MemoryTransactionStore());
    final held = Completer<void>();
    final overlay = app.overlays.run(() => held.future);
    app.attachWindowsActivation(await connect());
    expect(app.navigation.destination, AppDestination.gamesStore);
    expect(acks, isEmpty);
    held.complete();
    await overlay;
    await Future<void>.delayed(Duration.zero);
    expect(app.navigation.destination, AppDestination.gamesLibrary);
    expect(acks, [1]);
    await app.close();
  });
  test('activation cannot establish library ownership', () async {
    initial = [
      {
        'sequence': 1,
        'uri': 'ryhze://app/games/library?provider=steam&item=123',
      },
    ];
    final app = AppServices(storage: MemoryTransactionStore());
    app.attachWindowsActivation(await connect());
    await Future<void>.delayed(Duration.zero);
    expect(app.routes.value.title, isNull);
    expect(app.routes.value.failure, isNotNull);
    expect(acks, [1]);
    await app.close();
  });
}
