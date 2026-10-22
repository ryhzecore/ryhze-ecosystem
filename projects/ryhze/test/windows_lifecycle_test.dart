import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/platform/windows/windows_lifecycle.dart';

class _HeldOperation implements JobOperation {
  final started = Completer<void>();
  final release = Completer<void>();
  late CancellationToken token;
  int executions = 0;
  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken cancellation,
    Future<void> Function(int) checkpoint,
  ) async {
    token = cancellation;
    executions++;
    await checkpoint(1);
    started.complete();
    await release.future;
    return JobOutcome.completed;
  }

  @override
  Future<JobOutcome> reconcile(
    JobRecord job,
    CancellationToken cancellation,
  ) async => JobOutcome.unknown;
}

class _FailingStore extends MemoryTransactionStore {
  bool fail = false;
  bool closed = false;
  Completer<void>? blockedWrite;
  Completer<void>? writeEntered;
  @override
  Future<void> transact(void Function(StoreData) update) async {
    if (blockedWrite != null) {
      writeEntered?.complete();
      await blockedWrite!.future;
    }
    if (fail) throw StateError('private synthetic native I/O detail');
    await super.transact(update);
  }

  @override
  Future<void> close() async {
    await super.close();
    closed = true;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.tests/windows.lifecycle');
  const codec = StandardMethodCodec();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  Future<Object?> event(
    Object? arguments, {
    String method = 'lifecycle',
  }) async {
    final reply = await messenger.handlePlatformMessage(
      channel.name,
      codec.encodeMethodCall(MethodCall(method, arguments)),
      null,
    );
    return reply == null ? null : codec.decodeEnvelope(reply);
  }

  Map<String, Object> snapshot({int sequence = 0, String state = 'active'}) => {
    'schemaVersion': 1,
    'powerEvents': true,
    'sequence': sequence,
    'state': state,
  };
  Future<WindowsLifecycleClient> connect() async {
    final client = await WindowsLifecycleClient.connect(channel: channel);
    expect(client, isNotNull);
    addTearDown(client!.dispose);
    return client;
  }

  setUp(() {
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'connect');
      expect(call.arguments, isNull);
      return snapshot();
    });
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test(
    'optional bridge failure keeps the existing store and browsing services',
    () async {
      final store = MemoryTransactionStore();
      final services = await AppServices.bootstrap(
        openStore: () async => store,
        connectLifecycle: () async =>
            throw StateError('private bridge details'),
      );
      addTearDown(services.close);
      expect(services.storage, same(store));
      expect(services.storageFailure, isNull);
      expect(services.workspace.writable, isTrue);
      expect(services.windowsPowerEventsAvailable, isFalse);
    },
  );

  test('a pre-connect interruption snapshot gates jobs immediately', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (_) async => snapshot(sequence: 1, state: 'suspended'),
    );
    final client = await connect();
    final services = AppServices.memory()..attachWindowsLifecycle(client);
    addTearDown(services.close);
    final scope = services.session.scope;
    await services.jobs.enqueue(scope, 'unconfigured', 'queued');
    await services.coordinator.drain(scope);
    expect(services.jobs.read(scope).single.state, JobState.queued);
    await event({'sequence': 2, 'state': 'resumed'});
    await services.recoverAfterResume();
    await services.coordinator.drain(scope);
    expect(services.jobs.read(scope).single.state, JobState.blocked);
  });

  test(
    'strict schema and stale messages cannot replace newer native state',
    () async {
      final client = await connect();
      await event({'sequence': 4, 'state': 'suspended'});
      await event({'sequence': 3, 'state': 'resumed'});
      await event({'sequence': 4, 'state': 'resumed'});
      expect(client.value.state, WindowsLifecycleState.suspended);
      for (final invalid in [
        {'sequence': -1, 'state': 'resumed'},
        {'sequence': 5.0, 'state': 'resumed'},
        {'sequence': 5, 'state': 'execute'},
        {'sequence': 5, 'state': 'resumed', 'command': 'ignored'},
      ]) {
        await expectLater(event(invalid), throwsA(isA<PlatformException>()));
        expect(client.value.sequence, 4);
      }
      await event({'sequence': 5, 'state': 'resumed'});
      expect(client.value.state, WindowsLifecycleState.resumed);
    },
  );

  test(
    'an event racing connect cannot be overwritten by its older snapshot',
    () async {
      messenger.setMockMethodCallHandler(channel, (_) async {
        await event({'sequence': 2, 'state': 'resumed'});
        return snapshot(sequence: 1, state: 'suspended');
      });
      final client = await connect();
      expect(client.value.sequence, 2);
      expect(client.value.state, WindowsLifecycleState.resumed);
    },
  );

  test(
    'unavailable or malformed host does not claim power-event capability',
    () async {
      for (final result in [
        null,
        {...snapshot(), 'schemaVersion': 1.0},
        {...snapshot(), 'powerEvents': false},
        {...snapshot(), 'extra': true},
        {...snapshot(), 'state': 'unknown'},
        {...snapshot(), 'state': 'suspended'},
      ]) {
        messenger.setMockMethodCallHandler(channel, (_) async => result);
        expect(await WindowsLifecycleClient.connect(channel: channel), isNull);
      }
      messenger.setMockMethodCallHandler(channel, null);
      expect(await WindowsLifecycleClient.connect(channel: channel), isNull);
    },
  );

  test('repeated power cycles wait for checkpoints then reopen only latest recovery', () async {
    final operation = _HeldOperation();
    final client = await connect();
    final services = AppServices(
      storage: MemoryTransactionStore(),
      jobOperations: {'fixture': operation},
    )..attachWindowsLifecycle(client);
    addTearDown(services.close);
    final scope = services.session.scope;
    await services.jobs.enqueue(scope, 'fixture', 'active');
    final drain = services.coordinator.drain(scope);
    await operation.started.future;
    await event({'sequence': 1, 'state': 'suspended'});
    expect(operation.token.isCancelled, isTrue);
    await event({'sequence': 2, 'state': 'resumed'});
    final earlierRecovery = services.recoverAfterResume();
    await Future<void>.delayed(Duration.zero);
    await event({'sequence': 3, 'state': 'suspended'});
    await event({'sequence': 4, 'state': 'resumed'});
    final latestRecovery = services.recoverAfterResume();
    expect(latestRecovery, isNot(same(earlierRecovery)));
    await services.jobs.enqueue(scope, 'unconfigured', 'after-resume');
    operation.release.complete();
    await drain;
    await latestRecovery;
    expect(operation.executions, 1);
    expect(services.jobs.read(scope).first.state, JobState.recoveryRequired);
    expect(services.jobs.read(scope).first.checkpoint, 1);
    await services.coordinator.drain(scope);
    expect(services.jobs.read(scope).last.state, JobState.blocked);
    expect(services.lifecycleFailure, isNull);
  });

  test(
    'account replacement during native recovery reconciles the new scope only',
    () async {
      final operation = _HeldOperation();
      final client = await connect();
      final services = AppServices(
        storage: MemoryTransactionStore(),
        jobOperations: {'fixture': operation},
      )..attachWindowsLifecycle(client);
      addTearDown(services.close);
      final original = services.session.scope;
      const replacement = AccountScope.verified('fixture', 'replacement');
      await services.jobs.enqueue(original, 'fixture', 'original');
      final other = await services.jobs.enqueue(
        replacement,
        'fixture',
        'replacement',
      );
      await services.jobs.transition(replacement, other, JobState.running);
      final drain = services.coordinator.drain(original);
      await operation.started.future;
      await event({'sequence': 1, 'state': 'suspended'});
      await event({'sequence': 2, 'state': 'resumed'});
      await Future<void>.delayed(Duration.zero);
      services.session.replace(replacement);
      operation.release.complete();
      await drain;
      await services.recoverAfterResume();
      expect(
        services.jobs.read(replacement).single.state,
        JobState.recoveryRequired,
      );
      expect(
        services.workspace.events.single.id,
        isNot(services.activity.read(original).single.id),
      );
      expect(operation.executions, 1);
    },
  );

  test('failed native recovery stays blocked and exposes only a safe retry message', () async {
    final store = _FailingStore();
    final client = await connect();
    final services = AppServices(storage: store)
      ..attachWindowsLifecycle(client);
    addTearDown(services.close);
    final scope = services.session.scope;
    final job = await services.jobs.enqueue(scope, 'fixture', 'interrupted');
    await services.jobs.transition(scope, job, JobState.running);
    await event({'sequence': 1, 'state': 'suspended'});
    store.fail = true;
    await event({'sequence': 2, 'state': 'resumed'});
    await services.recoverAfterResume();
    expect(services.lifecycleFailure?.kind, FailureKind.recovery);
    expect(services.lifecycleFailure.toString(), isNot(contains('private')));
    store.fail = false;
    await services.recoverAfterResume();
    expect(services.lifecycleFailure, isNull);
    expect(services.jobs.read(scope).single.state, JobState.recoveryRequired);
  });

  test('scope replacement after failed recovery reopens only the recovered new scope', () async {
    final store = _FailingStore();
    final services = AppServices(storage: store);
    addTearDown(services.close);
    final original = services.session.scope;
    const replacement = AccountScope.verified('fixture', 'replacement');
    for (final scope in [original, replacement]) {
      final job = await services.jobs.enqueue(scope, 'fixture', 'interrupted');
      await services.jobs.transition(scope, job, JobState.running);
    }
    services.interruptLifecycle();
    store.fail = true;
    await services.recoverAfterResume();
    expect(services.lifecycleFailure?.kind, FailureKind.recovery);
    store.fail = false;
    services.session.replace(replacement);
    await Future<void>.delayed(Duration.zero);
    expect(services.lifecycleFailure, isNull);
    expect(services.jobs.read(original).single.state, JobState.running);
    expect(
      services.jobs.read(replacement).single.state,
      JobState.recoveryRequired,
    );
    await services.jobs.enqueue(replacement, 'unconfigured', 'new-work');
    await services.coordinator.drain(replacement);
    expect(services.jobs.read(replacement).last.state, JobState.blocked);
  });

  test('critical resume recovers without an earlier suspend message', () async {
    final client = await connect();
    final services = AppServices.memory()..attachWindowsLifecycle(client);
    addTearDown(services.close);
    final scope = services.session.scope;
    final job = await services.jobs.enqueue(scope, 'fixture', 'interrupted');
    await services.jobs.transition(scope, job, JobState.running);
    await event({'sequence': 1, 'state': 'resumed'});
    await services.recoverAfterResume();
    expect(services.jobs.read(scope).single.state, JobState.recoveryRequired);
  });

  test(
    'failure from an older recovery cannot publish across a new interruption',
    () async {
      final store = _FailingStore();
      final client = await connect();
      final services = AppServices(storage: store)
        ..attachWindowsLifecycle(client);
      addTearDown(services.close);
      final scope = services.session.scope;
      final job = await services.jobs.enqueue(scope, 'fixture', 'interrupted');
      await services.jobs.transition(scope, job, JobState.running);
      await event({'sequence': 1, 'state': 'suspended'});
      store.blockedWrite = Completer<void>();
      store.writeEntered = Completer<void>();
      store.fail = true;
      await event({'sequence': 2, 'state': 'resumed'});
      final earlierRecovery = services.recoverAfterResume();
      await store.writeEntered!.future;
      await event({'sequence': 3, 'state': 'suspended'});
      var notifications = 0;
      services.addListener(() => notifications++);
      store.blockedWrite!.complete();
      await earlierRecovery;
      expect(services.lifecycleFailure, isNull);
      expect(notifications, 0);
      store.blockedWrite = null;
      store.writeEntered = null;
      store.fail = false;
      await services.jobs.enqueue(scope, 'unconfigured', 'after-interruption');
      await services.coordinator.drain(scope);
      expect(services.jobs.read(scope).last.state, JobState.queued);
      await event({'sequence': 4, 'state': 'resumed'});
      await services.recoverAfterResume();
      await services.coordinator.drain(scope);
      expect(services.jobs.read(scope).last.state, JobState.blocked);
    },
  );

  test(
    'close joins failed work and still releases its durable store',
    () async {
      final store = _FailingStore();
      final operation = _HeldOperation();
      final services = AppServices(
        storage: store,
        jobOperations: {'fixture': operation},
      );
      await services.jobs.enqueue(services.session.scope, 'fixture', 'active');
      final drain = services.coordinator.drain(services.session.scope);
      final drainFailure = expectLater(drain, throwsStateError);
      await operation.started.future;
      store.fail = true;
      final close = services.close();
      final closeFailure = expectLater(close, throwsStateError);
      expect(services.close(), same(close));
      expect(store.closed, isFalse);
      operation.release.complete();
      await drainFailure;
      await closeFailure;
      expect(store.closed, isTrue);
    },
  );
}
