import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/features/activity/activity_repository.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/platform/windows/windows_taskbar.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.tests/taskbar');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final calls = <MethodCall>[];
  setUp(() {
    calls.clear();
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'probe') {
        return {'schemaVersion': 1, 'progress': true};
      }
      return {'state': 'queued'};
    });
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  Future<WindowsTaskbarClient> connect() async =>
      (await WindowsTaskbarClient.connect(channel: channel))!;
  Future<void> flush() => Future<void>.delayed(Duration.zero);
  String getLastState() => (calls.last.arguments as Map)['state'] as String;

  test(
    'taskbar is independently probed with a strict versioned schema',
    () async {
      for (final response in [
        null,
        {'schemaVersion': 1, 'progress': 'true'},
        {'schemaVersion': 2, 'progress': true},
        {'schemaVersion': 1, 'progress': true, 'launch': true},
      ]) {
        messenger.setMockMethodCallHandler(channel, (_) async => response);
        expect(await WindowsTaskbarClient.connect(channel: channel), isNull);
      }
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return {'schemaVersion': 1, 'progress': false};
      });
      final client = await connect();
      expect(client.available, isFalse);
      expect(
        await client.update(WindowsTaskbarState.indeterminate),
        WindowsTaskbarResult.unavailable,
      );
      expect(calls.map((c) => c.method), ['probe']);
    },
  );

  test(
    'native shell acceptance is distinct from visible application',
    () async {
      final client = await connect();
      expect(
        await client.update(WindowsTaskbarState.indeterminate),
        WindowsTaskbarResult.queued,
      );
      expect(calls.last.arguments, {'state': 'indeterminate'});
      for (final response in [
        {'state': '100-percent'},
        {'state': 'applied', 'account': 'untrusted'},
        null,
      ]) {
        messenger.setMockMethodCallHandler(channel, (_) async => response);
        await expectLater(
          client.update(WindowsTaskbarState.none),
          throwsA(isA<AppFailure>()),
        );
      }
    },
  );

  test('projection never interprets an opaque checkpoint as a percentage', () {
    final job = JobRecord(
      key: 'fixture',
      kind: 'fixture',
      item: 'private-item',
      state: JobState.running,
      checkpoint: 99999,
      updatedAt: DateTime.utc(2026),
    );
    final warning = ActivityRecord(
      id: 'fixture-warning',
      kind: ActivityKind.recovery,
      occurredAt: DateTime.utc(2026),
      count: 1,
    );
    expect(
      WindowsTaskbarBinding.project([job], []),
      WindowsTaskbarState.indeterminate,
    );
    expect(
      WindowsTaskbarBinding.project([job], [warning]),
      WindowsTaskbarState.attention,
    );
    expect(
      WindowsTaskbarBinding.project([job], [warning], suspended: true),
      WindowsTaskbarState.none,
    );
    expect(WindowsTaskbarBinding.project([], []), WindowsTaskbarState.none);
  });

  test('durable job transitions and activity acknowledgement drive real channel status', () async {
    final store = MemoryTransactionStore();
    final session = SessionController();
    final activity = ActivityRepository(store);
    final jobs = JobRepository(store, activity: activity);
    final binding = WindowsTaskbarBinding(
      await connect(),
      jobs,
      activity,
      session,
    );
    await flush();
    expect(getLastState(), 'none');
    final job = await jobs.enqueue(session.scope, 'fixture', 'private-item');
    await flush();
    expect(getLastState(), 'indeterminate');
    await jobs.transition(session.scope, job, JobState.running);
    await jobs.transition(session.scope, job, JobState.failed);
    await flush();
    expect(getLastState(), 'attention');
    await activity.acknowledge(
      session.scope,
      activity.read(session.scope).single.id,
    );
    await flush();
    expect(getLastState(), 'none');
    expect(
      calls
          .where((call) => call.method == 'set')
          .every((call) => (call.arguments as Map).keys.single == 'state'),
      isTrue,
    );
    expect(calls.toString(), isNot(contains('private-item')));
    await binding.close();
    jobs.dispose();
    activity.dispose();
    session.dispose();
    await store.close();
  });

  test('suspend and account replacement clear only shell status without altering jobs', () async {
    final store = MemoryTransactionStore();
    final session = SessionController()
      ..replace(const AccountScope.verified('fixture', 'first'));
    final firstScope = session.scope;
    final activity = ActivityRepository(store);
    final jobs = JobRepository(store, activity: activity);
    final binding = WindowsTaskbarBinding(
      await connect(),
      jobs,
      activity,
      session,
    );
    await jobs.enqueue(firstScope, 'fixture', 'one');
    await flush();
    expect(getLastState(), 'indeterminate');
    binding.setSuspended(true);
    await flush();
    expect(getLastState(), 'none');
    expect(jobs.read(firstScope).single.state, JobState.queued);
    binding.setSuspended(false);
    await flush();
    expect(getLastState(), 'indeterminate');
    session.signOut();
    await flush();
    expect(getLastState(), 'none');
    await binding.close();
    final count = calls.length;
    session.replace(firstScope);
    await jobs.enqueue(firstScope, 'fixture', 'two');
    await flush();
    expect(
      calls.length,
      count,
      reason: 'closed binding cannot emit from stale listeners',
    );
    jobs.dispose();
    activity.dispose();
    session.dispose();
    await store.close();
  });

  test(
    'late shell completion is followed by the newest account state',
    () async {
      final store = MemoryTransactionStore();
      final session = SessionController()
        ..replace(const AccountScope.verified('fixture', 'first'));
      final activity = ActivityRepository(store);
      final jobs = JobRepository(store, activity: activity);
      final client = await connect();
      final entered = Completer<void>();
      final release = Completer<Object?>();
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        if ((call.arguments as Map)['state'] == 'indeterminate' &&
            !entered.isCompleted) {
          entered.complete();
          return release.future;
        }
        return {'state': 'applied'};
      });
      final binding = WindowsTaskbarBinding(client, jobs, activity, session);
      await jobs.enqueue(session.scope, 'fixture', 'one');
      await entered.future;
      session.signOut();
      release.complete({'state': 'applied'});
      await flush();
      expect(getLastState(), 'none');
      await binding.close();
      jobs.dispose();
      activity.dispose();
      session.dispose();
      await store.close();
    },
  );

  test(
    'optional taskbar failure cannot fail a durable job transition',
    () async {
      final store = MemoryTransactionStore();
      final session = SessionController();
      final activity = ActivityRepository(store);
      final jobs = JobRepository(store, activity: activity);
      final client = await connect();
      messenger.setMockMethodCallHandler(
        channel,
        (_) async => throw PlatformException(
          code: 'unavailable',
          message: 'private native details',
        ),
      );
      final binding = WindowsTaskbarBinding(client, jobs, activity, session);
      final job = await jobs.enqueue(session.scope, 'fixture', 'one');
      await jobs.transition(session.scope, job, JobState.running);
      await jobs.transition(session.scope, job, JobState.completed);
      await flush();
      expect(jobs.read(session.scope).single.state, JobState.completed);
      await binding.close();
      jobs.dispose();
      activity.dispose();
      session.dispose();
      await store.close();
    },
  );
}
