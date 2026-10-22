import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/platform/apple/first_frame_probe.dart';

// Simulated platform/channel and raster completion, not native rendering proof.
const _nonce = '0123456789abcdef0123456789abcdef';
const _processID = 2468;
const _channel = MethodChannel('ryhze.test/first-frame-probe');
const _configuration = {
  'schemaVersion': 1,
  'nonce': _nonce,
  'processID': _processID,
};
const _landmarks = [
  'app-chrome-scroll',
  'catalog-scroll',
  'hero-surface',
  'destination-store',
  'destination-library',
];

class _Transport {
  Object? configuration = _configuration;
  bool rejectUpdates = false;
  final calls = <MethodCall>[];

  void attach(WidgetTester tester) {
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(_channel, (
      call,
    ) async {
      calls.add(call);
      if (call.method == 'configuration') return configuration;
      if (rejectUpdates) {
        throw PlatformException(
          code: 'synthetic-private-code',
          message: 'synthetic-private-message',
          details: 'synthetic-private-details',
        );
      }
      return null;
    });
    addTearDown(() {
      tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        _channel,
        null,
      );
    });
  }

  List<Map<Object?, Object?>> get updates => [
    for (final call in calls)
      if (call.method == 'update')
        Map<Object?, Object?>.from(call.arguments as Map),
  ];

  List<Object?> get events => [for (final update in updates) update['event']];
}

Map<String, Object?> _event(String event, [String? failureCode]) => {
  'nonce': _nonce,
  'processID': _processID,
  'event': event,
  'failureCode': failureCode,
};

Future<FirstFrameProbe> _connect(
  WidgetTester tester,
  _Transport transport, {
  Future<void> Function()? waitForRaster,
  Duration rasterTimeout = const Duration(seconds: 60),
}) async {
  transport.attach(tester);
  final probe = await FirstFrameProbe.connect(
    channel: _channel,
    waitForRaster: waitForRaster ?? () async {},
    rasterTimeout: rasterTimeout,
  );
  expect(probe, isNotNull);
  addTearDown(probe!.dispose);
  return probe;
}

Widget _shell({Widget Function(Widget)? alterHero, bool omitHero = false}) =>
    Directionality(
      textDirection: TextDirection.ltr,
      child: Column(
        children: [
          for (final landmark in _landmarks)
            if (landmark != 'hero-surface' || !omitHero)
              landmark == 'hero-surface' && alterHero != null
                  ? alterHero(
                      SizedBox(key: Key(landmark), width: 40, height: 40),
                    )
                  : SizedBox(key: Key(landmark), width: 40, height: 40),
        ],
      ),
    );

void main() {
  final ios = TargetPlatformVariant.only(TargetPlatform.iOS);

  testWidgets('absent or malformed configuration never starts a probe', (
    tester,
  ) async {
    final transport = _Transport()..attach(tester);
    final invalid = <Object?>[
      null,
      false,
      [],
      {},
      {..._configuration, 'schemaVersion': 2},
      {..._configuration, 'schemaVersion': 1.0},
      {..._configuration, 'schemaVersion': '1'},
      {..._configuration, 'nonce': _nonce.substring(1)},
      {..._configuration, 'nonce': '${_nonce}0'},
      {..._configuration, 'nonce': _nonce.toUpperCase()},
      {..._configuration, 'nonce': 'g${_nonce.substring(1)}'},
      {..._configuration, 'processID': 0},
      {..._configuration, 'processID': -1},
      {..._configuration, 'processID': 1.5},
      {..._configuration, 'processID': '2468'},
      {..._configuration, 'processID': 0x80000000},
      {..._configuration, 'unexpected': 'synthetic-private-value'},
    ];
    for (final configuration in invalid) {
      transport.configuration = configuration;
      expect(await FirstFrameProbe.connect(channel: _channel), isNull);
    }
    expect(transport.calls, hasLength(invalid.length));
    expect(transport.updates, isEmpty);
  }, variant: ios);

  testWidgets(
    'other platforms do not contact the probe channel',
    (tester) async {
      final transport = _Transport()..attach(tester);
      expect(await FirstFrameProbe.connect(channel: _channel), isNull);
      expect(transport.calls, isEmpty);
    },
    variant: TargetPlatformVariant({
      TargetPlatform.macOS,
      TargetPlatform.android,
    }),
  );

  for (final size in [const Size(390, 844), const Size(1024, 768)]) {
    testWidgets('real iOS shell waits for raster completion at $size', (
      tester,
    ) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = size;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final transport = _Transport();
      final raster = Completer<void>();
      final probe = await _connect(
        tester,
        transport,
        waitForRaster: () => raster.future,
      );
      await probe.bootstrapCompleted(healthy: true);
      await tester.pumpWidget(probe.wrap(const RyhzeApp()));
      await tester.pump();
      for (final landmark in _landmarks) {
        final finder = find.byKey(Key(landmark));
        expect(finder, findsOneWidget);
        final rect = tester.getRect(finder);
        expect(rect.isFinite, isTrue);
        expect(rect.width, greaterThan(0));
        expect(rect.height, greaterThan(0));
        expect(rect.overlaps(Offset.zero & size), isTrue);
      }
      expect(transport.updates, [
        _event('bootstrap-healthy'),
        _event('shell-layout'),
      ]);
      raster.complete();
      await tester.pump();
      await tester.pump();
      expect(transport.updates, [
        _event('bootstrap-healthy'),
        _event('shell-layout'),
        _event('first-frame-rasterized'),
      ]);
      await tester.pump();
      expect(
        transport.events.where((event) => event == 'first-frame-rasterized'),
        hasLength(1),
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    }, variant: ios);
  }

  final invalidShells = <String, Widget Function()>{
    'missing landmark': () => _shell(omitHero: true),
    'offstage landmark': () =>
        _shell(alterHero: (hero) => Offstage(child: hero)),
    'invisible landmark': () => _shell(
      alterHero: (hero) =>
          Visibility(visible: false, maintainState: true, child: hero),
    ),
    'transparent landmark': () =>
        _shell(alterHero: (hero) => Opacity(opacity: 0, child: hero)),
    'zero-size landmark': () =>
        _shell(alterHero: (hero) => SizedBox.shrink(child: hero)),
    'offscreen landmark': () => _shell(
      alterHero: (hero) =>
          Transform.translate(offset: const Offset(2000, 0), child: hero),
    ),
    'hidden IndexedStack landmark': () => _shell(
      alterHero: (hero) => IndexedStack(
        index: 0,
        children: [const SizedBox(width: 40, height: 40), hero],
      ),
    ),
  };
  for (final entry in invalidShells.entries) {
    testWidgets('${entry.key} prevents shell readiness', (tester) async {
      final transport = _Transport();
      var rasterCalls = 0;
      final probe = await _connect(
        tester,
        transport,
        waitForRaster: () async {
          rasterCalls++;
        },
      );
      await probe.bootstrapCompleted(healthy: true);
      await tester.pumpWidget(probe.wrap(entry.value()));
      await tester.pump();
      expect(transport.updates, [
        _event('bootstrap-healthy'),
        _event('failed', 'shell-layout'),
      ]);
      expect(rasterCalls, 0);
    }, variant: ios);
  }

  testWidgets(
    'degraded bootstrap remains terminal after a later healthy report',
    (tester) async {
      final transport = _Transport();
      final probe = await _connect(tester, transport);
      await probe.bootstrapCompleted(healthy: false);
      await probe.bootstrapCompleted(healthy: true);
      await tester.pumpWidget(probe.wrap(_shell()));
      await tester.pump();
      await probe.fail('flutter-error');
      expect(transport.updates, [
        _event('bootstrap-degraded', 'bootstrap-degraded'),
      ]);
    },
    variant: ios,
  );

  testWidgets('layout without successful bootstrap is rejected', (
    tester,
  ) async {
    final transport = _Transport();
    final probe = await _connect(tester, transport);
    await tester.pumpWidget(probe.wrap(_shell()));
    await tester.pump();
    expect(transport.updates, [_event('failed', 'bootstrap-failed')]);
  }, variant: ios);

  testWidgets('raster timeout stays failed after delayed raster completion', (
    tester,
  ) async {
    final transport = _Transport();
    final raster = Completer<void>();
    final probe = await _connect(
      tester,
      transport,
      waitForRaster: () => raster.future,
      rasterTimeout: const Duration(milliseconds: 20),
    );
    await probe.bootstrapCompleted(healthy: true);
    await tester.pumpWidget(probe.wrap(_shell()));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 21));
    raster.complete();
    await tester.pump();
    await probe.bootstrapCompleted(healthy: true);
    expect(transport.updates, [
      _event('bootstrap-healthy'),
      _event('shell-layout'),
      _event('failed', 'raster-timeout'),
    ]);
  }, variant: ios);

  testWidgets('raster exceptions report only a fixed failure code', (
    tester,
  ) async {
    final transport = _Transport();
    final probe = await _connect(
      tester,
      transport,
      waitForRaster: () async {
        throw StateError('synthetic-private-raster-detail');
      },
    );
    await probe.bootstrapCompleted(healthy: true);
    await tester.pumpWidget(probe.wrap(_shell()));
    await tester.pump();
    expect(transport.updates, [
      _event('bootstrap-healthy'),
      _event('shell-layout'),
      _event('failed', 'probe-failed'),
    ]);
    expect(tester.takeException(), isNull);
  }, variant: ios);

  testWidgets('late failure invalidates readiness and is sticky', (
    tester,
  ) async {
    final transport = _Transport();
    final probe = await _connect(tester, transport);
    await probe.bootstrapCompleted(healthy: true);
    await tester.pumpWidget(probe.wrap(_shell()));
    await tester.pump();
    await probe.fail('flutter-error');
    await probe.bootstrapCompleted(healthy: true);
    await probe.fail('async-error');
    await tester.pump();
    expect(transport.updates, [
      _event('bootstrap-healthy'),
      _event('shell-layout'),
      _event('first-frame-rasterized'),
      _event('failed', 'flutter-error'),
    ]);
  }, variant: ios);

  testWidgets('disposing a pending probe suppresses later raster reports', (
    tester,
  ) async {
    final transport = _Transport();
    final raster = Completer<void>();
    final probe = await _connect(
      tester,
      transport,
      waitForRaster: () => raster.future,
    );
    await probe.bootstrapCompleted(healthy: true);
    await tester.pumpWidget(probe.wrap(_shell()));
    await tester.pump();
    await tester.pumpWidget(const SizedBox.shrink());
    raster.complete();
    await tester.pump();
    await probe.fail('async-error');
    expect(transport.updates, [
      _event('bootstrap-healthy'),
      _event('shell-layout'),
    ]);
  }, variant: ios);

  testWidgets(
    'transport failure is bounded and never exposes exception details',
    (tester) async {
      final transport = _Transport()..rejectUpdates = true;
      final probe = await _connect(tester, transport);
      await probe.bootstrapCompleted(healthy: true);
      await tester.pumpWidget(probe.wrap(_shell()));
      await tester.pump();
      expect(transport.updates, [
        _event('bootstrap-healthy'),
        _event('failed', 'probe-failed'),
      ]);
      expect(tester.takeException(), isNull);
    },
    variant: ios,
  );

  for (final asyncError in [false, true]) {
    testWidgets('probe chains and restores error handlers, async=$asyncError', (
      tester,
    ) async {
      final transport = _Transport();
      final probe = await _connect(tester, transport);
      final originalFlutter = FlutterError.onError;
      final originalAsync = PlatformDispatcher.instance.onError;
      final forwarded = <Object>[];
      final error = StateError('synthetic-private-error');
      final stack = StackTrace.current;
      final details = FlutterErrorDetails(exception: error, stack: stack);
      void previousFlutter(FlutterErrorDetails value) => forwarded.add(value);
      bool previousAsync(Object value, StackTrace trace) {
        forwarded.addAll([value, trace]);
        return false;
      }

      var preservedDecision = true;
      var restoredFlutter = false;
      var restoredAsync = false;
      try {
        FlutterError.onError = previousFlutter;
        PlatformDispatcher.instance.onError = previousAsync;
        probe.installErrorHandlers();
        probe.installErrorHandlers();
        if (asyncError) {
          preservedDecision = PlatformDispatcher.instance.onError!(
            error,
            stack,
          );
        } else {
          FlutterError.onError!(details);
        }
        await tester.pump();
        probe.dispose();
        restoredFlutter = identical(FlutterError.onError, previousFlutter);
        restoredAsync = identical(
          PlatformDispatcher.instance.onError,
          previousAsync,
        );
      } finally {
        FlutterError.onError = originalFlutter;
        PlatformDispatcher.instance.onError = originalAsync;
      }
      expect(restoredFlutter, isTrue);
      expect(restoredAsync, isTrue);
      expect(forwarded, asyncError ? [error, stack] : [details]);
      if (asyncError) expect(preservedDecision, isFalse);
      expect(transport.updates, [
        _event('failed', asyncError ? 'async-error' : 'flutter-error'),
      ]);
    }, variant: ios);
  }

  testWidgets('dispose preserves handlers installed by a later owner', (
    tester,
  ) async {
    final transport = _Transport();
    final probe = await _connect(tester, transport);
    final originalFlutter = FlutterError.onError;
    final originalAsync = PlatformDispatcher.instance.onError;
    void laterFlutter(FlutterErrorDetails details) {}
    bool laterAsync(Object error, StackTrace stack) => true;
    var preservedFlutter = false;
    var preservedAsync = false;
    try {
      probe.installErrorHandlers();
      FlutterError.onError = laterFlutter;
      PlatformDispatcher.instance.onError = laterAsync;
      probe.dispose();
      preservedFlutter = identical(FlutterError.onError, laterFlutter);
      preservedAsync = identical(
        PlatformDispatcher.instance.onError,
        laterAsync,
      );
    } finally {
      FlutterError.onError = originalFlutter;
      PlatformDispatcher.instance.onError = originalAsync;
    }
    expect(preservedFlutter, isTrue);
    expect(preservedAsync, isTrue);
    expect(transport.updates, isEmpty);
  }, variant: ios);
}
