import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/platform/windows/windows_updates.dart';

// Synthetic channel settlement exercises application ownership, not a real
// publisher, installer, native trust provider or physical power transition.
class _Store extends MemoryTransactionStore {
  _Store(this.events);
  final List<String> events;
  @override
  Future<void> close() async {
    events.add('store-close');
    await super.close();
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.tests/update-lifecycle');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  late List<String> events;
  late AppServices app;
  late WindowsUpdateClient updates;

  setUp(() async {
    events = [];
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'probe'
          ? {'schemaVersion': 1, 'staging': true, 'activation': false}
          : null,
    );
    updates = (await WindowsUpdateClient.connect(channel: channel))!;
    app = AppServices(storage: _Store(events));
    app.windowsUpdates = updates;
  });
  tearDown(() async {
    // Each test settles its barrier before teardown; close remains idempotent.
    await app.close();
    messenger.setMockMethodCallHandler(channel, null);
  });

  test(
    'application close waits for update thread before durable store',
    () async {
      final settled = Completer<Object?>();
      final entered = Completer<void>();
      messenger.setMockMethodCallHandler(channel, (call) async {
        events.add(call.method);
        if (call.method == 'close') {
          entered.complete();
          await settled.future;
          events.add('update-settled');
        }
        return null;
      });
      var closed = false;
      final closing = app.close().then((_) => closed = true);
      await entered.future;
      await Future<void>.delayed(Duration.zero);
      expect(closed, isFalse);
      expect(events, isNot(contains('store-close')));
      expect(events.where((event) => event == 'close'), hasLength(1));
      settled.complete(null);
      await closing;
      expect(
        events.indexOf('update-settled'),
        lessThan(events.indexOf('store-close')),
      );
      expect(closed, isTrue);
    },
  );

  test(
    'resume joins cancelled update before reopening application jobs',
    () async {
      final operation = Completer<Object?>();
      final entered = Completer<void>(), drainEntered = Completer<void>();
      final settled = Completer<Object?>();
      messenger.setMockMethodCallHandler(channel, (call) async {
        events.add(call.method);
        if (call.method == 'stage') {
          entered.complete();
          return operation.future;
        }
        if (call.method == 'cancel') {
          if (!operation.isCompleted) {
            operation.completeError(PlatformException(code: 'cancelled'));
          }
        }
        if (call.method == 'drain') {
          drainEntered.complete();
          await settled.future;
          events.add('update-settled');
        }
        return null;
      });
      final work = updates.stage(
        Uint8List(3),
        Uint8List(32),
        CancellationToken(),
      );
      final rejected = expectLater(work, throwsA(isA<AppFailure>()));
      await entered.future;
      app.interruptLifecycle();
      await rejected;
      var resumed = false;
      final resume = app.recoverAfterResume().then((_) => resumed = true);
      await drainEntered.future;
      expect(resumed, isFalse);
      expect(app.coordinator.acquireIdle, throwsA(isA<AppFailure>()));
      await expectLater(
        updates.inspect(Uint8List(32), 3, CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      expect(events.where((event) => event == 'inspect'), isEmpty);
      settled.complete(null);
      await resume;
      expect(app.lifecycleFailure, isNull);
      final lease = app.coordinator.acquireIdle();
      lease.release();
      expect(events.where((event) => event == 'stage'), hasLength(1));
    },
  );

  test(
    'update cancellation during drain keeps application jobs suspended',
    () async {
      final drainEntered = Completer<void>();
      final settled = Completer<Object?>();
      messenger.setMockMethodCallHandler(channel, (call) async {
        events.add(call.method);
        if (call.method == 'drain') {
          if (!drainEntered.isCompleted) drainEntered.complete();
          await settled.future;
        }
        return null;
      });
      app.interruptLifecycle();
      final recovery = app.recoverAfterResume();
      await drainEntered.future;
      // Only the updater generation changes. AppServices' lifecycle boundary
      // is unchanged, so it must observe the client's explicit failed recovery.
      await updates.cancel();
      settled.complete(null);
      await recovery;
      expect(app.lifecycleFailure?.kind, FailureKind.recovery);
      expect(
        app.coordinator.acquireIdle,
        throwsA(
          isA<AppFailure>().having(
            (failure) => failure.kind,
            'kind',
            FailureKind.conflict,
          ),
        ),
      );
      await expectLater(
        updates.inspect(Uint8List(32), 3, CancellationToken()),
        throwsA(
          isA<AppFailure>().having(
            (failure) => failure.kind,
            'kind',
            FailureKind.cancelled,
          ),
        ),
      );
      expect(events.where((event) => event == 'inspect'), isEmpty);
      await app.recoverAfterResume();
      expect(app.lifecycleFailure, isNull);
      app.coordinator.acquireIdle().release();
      expect(events.where((event) => event == 'drain'), hasLength(2));
    },
  );

  test(
    'failed update settlement keeps lifecycle recovery visible and retryable',
    () async {
      var fail = true;
      messenger.setMockMethodCallHandler(channel, (call) async {
        events.add(call.method);
        if (call.method == 'drain' && fail) {
          throw PlatformException(
            code: 'recovery',
            message: 'private native detail',
          );
        }
        return null;
      });
      app.interruptLifecycle();
      await app.recoverAfterResume();
      expect(app.lifecycleFailure?.kind, FailureKind.recovery);
      expect(app.coordinator.acquireIdle, throwsA(isA<AppFailure>()));
      await expectLater(
        updates.inspect(Uint8List(32), 3, CancellationToken()),
        throwsA(isA<AppFailure>()),
      );
      fail = false;
      await app.recoverAfterResume();
      expect(app.lifecycleFailure, isNull);
      app.coordinator.acquireIdle().release();
      expect(events.where((event) => event == 'drain'), hasLength(2));
      expect(events.where((event) => event == 'inspect'), isEmpty);
    },
  );
}
