import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/domain/app_route.dart';
import 'package:ryhze/features/activity/activity_repository.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/platform/windows/windows_notifications.dart';

const route = AppRoute(
  AppDestination.gamesLibrary,
  itemId: 'title',
  providerId: 'fixture',
);

class Fixture {
  final store = MemoryTransactionStore();
  final session = SessionController();
  late final activity = ActivityRepository(store);
  WindowsNotificationsBinding? binding;
  int tick = 0;
  Future<void> record(String reference, {AppRoute? destination = route}) async {
    await store.transact(
      (draft) => ActivityRepository.recordIn(
        draft,
        session.scope,
        reference,
        ActivityKind.failed,
        DateTime.utc(2026, 1, 1).add(Duration(seconds: tick++)),
        route: destination,
      ),
    );
    activity.changed();
  }

  Future<void> close() async {
    await binding?.close();
    activity.dispose();
    session.dispose();
    await store.close();
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.tests/notifications');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final calls = <MethodCall>[];
  setUp(() {
    calls.clear();
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'probe') {
        return {'schemaVersion': 1, 'notifications': true};
      }
      return {'state': call.method == 'clear' ? 'cleared' : 'submitted'};
    });
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  Future<WindowsNotificationsClient> connect() async =>
      (await WindowsNotificationsClient.connect(channel: channel))!;
  Future<void> flush() => Future<void>.delayed(Duration.zero);
  List<MethodCall> shows() => calls.where((c) => c.method == 'show').toList();

  test(
    'durable production job outcome emits actionable Activity notification',
    () async {
      final fixture = Fixture();
      fixture.binding = WindowsNotificationsBinding(
        await connect(),
        fixture.activity,
        fixture.session,
        enabled: true,
      );
      final jobs = JobRepository(fixture.store, activity: fixture.activity);
      final job = await jobs.enqueue(
        fixture.session.scope,
        'fixture',
        'private-item',
      );
      await jobs.transition(fixture.session.scope, job, JobState.running);
      await jobs.transition(fixture.session.scope, job, JobState.completed);
      await flush();
      expect(shows().length, 1);
      expect((shows().single.arguments as Map)['uri'], 'ryhze://app/activity');
      expect((shows().single.arguments as Map)['kind'], 'completed');
      expect((shows().single.arguments as Map).keys.toSet(), {
        'id',
        'kind',
        'uri',
        'private',
      });
      expect(
        fixture.activity.read(fixture.session.scope).single.acknowledged,
        isFalse,
      );
      jobs.dispose();
      await fixture.close();
    },
  );

  test(
    'strict independent installed capability and submission results',
    () async {
      for (final value in [
        null,
        {'schemaVersion': 2, 'notifications': true},
        {'schemaVersion': 1, 'notifications': 'true'},
        {'schemaVersion': 1, 'notifications': true, 'installed': true},
      ]) {
        messenger.setMockMethodCallHandler(channel, (_) async => value);
        expect(
          await WindowsNotificationsClient.connect(channel: channel),
          isNull,
        );
      }
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return call.method == 'probe'
            ? {'schemaVersion': 1, 'notifications': false}
            : {'state': 'unavailable'};
      });
      final unavailable = await connect();
      expect(unavailable.available, isFalse);
      expect(
        await unavailable.submit(
          id: 'fixture',
          kind: ActivityKind.failed,
          route: route,
          privatePreview: true,
        ),
        WindowsNotificationResult.unavailable,
      );
      expect(shows(), isEmpty);
      expect(
        await unavailable.clear(),
        isFalse,
        reason: 'disabled banners can still retain old previews',
      );
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => {'schemaVersion': 1, 'notifications': true},
      );
      final client = await connect();
      for (final state in WindowsNotificationResult.values) {
        messenger.setMockMethodCallHandler(
          channel,
          (_) async => {'state': state.name},
        );
        expect(
          await client.submit(
            id: 'fixture',
            kind: ActivityKind.failed,
            route: route,
            privatePreview: true,
          ),
          state,
        );
      }
      for (final result in [
        null,
        {'state': 'delivered'},
        {'state': 'submitted', 'text': 'private'},
      ]) {
        messenger.setMockMethodCallHandler(channel, (_) async => result);
        await expectLater(
          client.submit(
            id: 'fixture',
            kind: ActivityKind.failed,
            route: route,
            privatePreview: true,
          ),
          throwsA(isA<AppFailure>()),
        );
      }
    },
  );

  test(
    'only validated navigation and closed summary data cross the boundary',
    () async {
      final client = await connect();
      await client.submit(
        id: 'fixture',
        kind: ActivityKind.handoff,
        route: route,
        privatePreview: false,
      );
      expect(calls.last.arguments, {
        'id': 'fixture',
        'kind': 'handoff',
        'uri': 'ryhze://app/games/library?item=title&provider=fixture',
        'private': false,
      });
      final count = calls.length;
      for (final badRoute in [
        const AppRoute(AppDestination.gamesLibrary, itemId: 'missing-provider'),
        const AppRoute(AppDestination.gamesStore, itemId: '../escape'),
        const AppRoute(AppDestination.filmsStore, providerId: 'bad'),
      ]) {
        await expectLater(
          client.submit(
            id: 'fixture',
            kind: ActivityKind.failed,
            route: badRoute,
            privatePreview: true,
          ),
          throwsA(isA<AppFailure>()),
        );
      }
      for (final id in ['', 'line\nbreak', List.filled(8193, 'a').join()]) {
        await expectLater(
          client.submit(
            id: id,
            kind: ActivityKind.failed,
            route: route,
            privatePreview: true,
          ),
          throwsA(isA<AppFailure>()),
        );
      }
      expect(calls.length, count);
    },
  );

  test('startup history and null routes stay quiet; new repeated activity can notify', () async {
    final fixture = Fixture();
    await fixture.record('old');
    fixture.binding = WindowsNotificationsBinding(
      await connect(),
      fixture.activity,
      fixture.session,
      enabled: true,
    );
    await flush();
    expect(calls.where((c) => c.method == 'clear').length, 1);
    expect(shows(), isEmpty);
    await fixture.record('no-route', destination: null);
    await fixture.record('new');
    await flush();
    expect(shows().length, 1);
    expect((shows().single.arguments as Map)['private'], isTrue);
    fixture.activity.changed();
    await flush();
    expect(
      shows().length,
      1,
      reason: 'ordinary repository repaint does not resubmit',
    );
    await fixture.record('new');
    await flush();
    expect(shows().length, 2);
    expect(
      shows()[0].arguments,
      shows()[1].arguments,
      reason: 'stable id enables native coalescing',
    );
    final record = fixture.activity
        .read(fixture.session.scope)
        .firstWhere((r) => r.count == 2);
    await fixture.activity.acknowledge(fixture.session.scope, record.id);
    await flush();
    expect(shows().length, 2);
    await fixture.close();
    expect(
      calls.where((c) => c.method == 'clear').length,
      1,
      reason: 'normal exit preserves protocol toasts for cold launch',
    );
  });

  test(
    'opt-in and privacy changes clear history without replaying old events',
    () async {
      final fixture = Fixture();
      fixture.binding = WindowsNotificationsBinding(
        await connect(),
        fixture.activity,
        fixture.session,
      );
      await fixture.record('disabled');
      await flush();
      expect(shows(), isEmpty);
      fixture.binding!.configure(enabled: true, privatePreviews: false);
      await flush();
      expect(shows(), isEmpty);
      await fixture.record('enabled');
      await flush();
      expect((shows().single.arguments as Map)['private'], isFalse);
      fixture.binding!.configure(enabled: true, privatePreviews: true);
      await flush();
      await fixture.record('private');
      await flush();
      expect((shows().last.arguments as Map)['private'], isTrue);
      fixture.binding!.configure(enabled: false, privatePreviews: true);
      await fixture.record('disabled-again');
      await flush();
      expect(shows().length, 2);
      expect(calls.where((c) => c.method == 'clear').length, 4);
      await fixture.close();
    },
  );

  test('account replacement clears a late submitted toast before new scope delivery', () async {
    final fixture = Fixture();
    final client = await connect();
    final entered = Completer<void>();
    final release = Completer<Object?>();
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'show' && !entered.isCompleted) {
        entered.complete();
        return release.future;
      }
      return {'state': call.method == 'clear' ? 'cleared' : 'submitted'};
    });
    fixture.binding = WindowsNotificationsBinding(
      client,
      fixture.activity,
      fixture.session,
      enabled: true,
    );
    await fixture.record('first');
    await entered.future;
    await fixture.record('old-pending');
    fixture.session.replace(const AccountScope.verified('fixture', 'second'));
    await fixture.record('new-scope');
    release.complete({'state': 'submitted'});
    await flush();
    final sequence = calls.where((c) => c.method != 'probe').toList();
    expect(sequence.map((c) => c.method), ['clear', 'show', 'clear', 'show']);
    expect((sequence.last.arguments as Map)['id'], contains('new-scope'));
    expect(
      shows().any(
        (c) => (c.arguments as Map)['id'].toString().contains('old-pending'),
      ),
      isFalse,
    );
    expect(
      fixture.activity.read(fixture.session.scope).single.acknowledged,
      isFalse,
    );
    await fixture.close();
  });

  test(
    'pending queue is bounded and acknowledgement removes an unsent event',
    () async {
      final fixture = Fixture();
      final client = await connect();
      final entered = Completer<void>();
      final release = Completer<Object?>();
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        if (call.method == 'show' && !entered.isCompleted) {
          entered.complete();
          return release.future;
        }
        return {'state': call.method == 'clear' ? 'cleared' : 'submitted'};
      });
      fixture.binding = WindowsNotificationsBinding(
        client,
        fixture.activity,
        fixture.session,
        enabled: true,
      );
      await fixture.record('first');
      await entered.future;
      for (var i = 0; i < 30; i++) {
        await fixture.record('pending-$i');
      }
      final acknowledged = fixture.activity.read(fixture.session.scope).first;
      await fixture.activity.acknowledge(
        fixture.session.scope,
        acknowledged.id,
      );
      release.complete({'state': 'submitted'});
      await flush();
      expect(
        shows().length,
        16,
        reason: 'one admitted plus 16 queued minus acknowledged',
      );
      expect(
        shows().any(
          (c) => (c.arguments as Map)['id'].toString().contains('pending-29'),
        ),
        isFalse,
      );
      await fixture.close();
    },
  );

  test(
    'close joins late submission and pending privacy clear without new calls',
    () async {
      final fixture = Fixture();
      final client = await connect();
      final entered = Completer<void>();
      final release = Completer<Object?>();
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        if (call.method == 'show' && !entered.isCompleted) {
          entered.complete();
          return release.future;
        }
        return {'state': call.method == 'clear' ? 'cleared' : 'submitted'};
      });
      fixture.binding = WindowsNotificationsBinding(
        client,
        fixture.activity,
        fixture.session,
        enabled: true,
      );
      await fixture.record('first');
      await entered.future;
      fixture.binding!.configure(enabled: false, privatePreviews: true);
      var closed = false;
      final closing = fixture.binding!.close().then((_) {
        closed = true;
      });
      await flush();
      expect(closed, isFalse);
      release.complete({'state': 'submitted'});
      await closing;
      expect(calls.last.method, 'clear');
      final count = calls.length;
      await fixture.record('after-close');
      fixture.session.signOut();
      await flush();
      expect(calls.length, count);
      await fixture.close();
    },
  );

  test('optional OS failure neither retries unchanged activity nor acknowledges it', () async {
    final fixture = Fixture();
    final client = await connect();
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      throw PlatformException(
        code: 'native-failure',
        message: 'private native diagnostics',
      );
    });
    fixture.binding = WindowsNotificationsBinding(
      client,
      fixture.activity,
      fixture.session,
      enabled: true,
    );
    await fixture.record('persisted');
    await flush();
    expect(shows().length, 1);
    fixture.activity.changed();
    await flush();
    expect(shows().length, 1);
    expect(
      fixture.activity.read(fixture.session.scope).single.acknowledged,
      isFalse,
    );
    await fixture.close();
  });
}
