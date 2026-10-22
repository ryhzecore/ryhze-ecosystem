import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/managed_content_service.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

// Explicit synthetic backend. Real Windows filesystem/codec suites establish
// native effects separately; these tests target transaction and lifecycle order.
class _Backend implements ContentStagingBackend {
  @override
  final deviceId = '${'1' * 16}:${'2' * 32}';
  final sourceId = '${'1' * 16}:${'3' * 32}';
  int stages = 0, inspections = 0;
  bool copied = false, closed = false;
  bool failDrain = false;
  Completer<void>? hold;
  Completer<void>? drainHold;
  final started = Completer<void>();
  final cancelled = Completer<void>();
  void Function()? beforeEffect;
  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    beforeEffect?.call();
    stages++;
    if (!started.isCompleted) started.complete();
    await hold?.future;
    copied = true;
    return receipt(request, true);
  }

  ContentStagingReceipt receipt(
    ContentStagingRequest request,
    bool staged,
  ) => ContentStagingReceipt(
    state: staged ? ContentStagingState.staged : ContentStagingState.checkpoint,
    verifiedBytes: staged ? request.size : 0,
    verifiedChunks: staged
        ? (request.size + managedStageChunkBytes - 1) ~/ managedStageChunkBytes
        : 0,
    rootIdentity: deviceId,
    sourceIdentity: sourceId,
  );
  @override
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    inspections++;
    return receipt(request, copied);
  }

  @override
  Future<void> cancel() async {
    if (!cancelled.isCompleted) cancelled.complete();
  }

  @override
  Future<void> drain() async {
    if (stages > 0) await hold?.future;
    await drainHold?.future;
    if (failDrain) throw const AppFailure(FailureKind.network);
  }

  @override
  Future<void> close() async {
    await cancel();
    await drain();
    closed = true;
  }
}

ManagedStageIntent _intent() => ManagedStageIntent(
  operationId: 'a' * 64,
  item: ProviderItemRef('fixture', 'game', namespace: 'exact'),
  packageId: 'fixture-package',
  version: '1',
  manifestSha256: 'b' * 64,
  sourceRelative: 'fixture.bin',
  expectedSize: 600000,
  expectedSha256: 'c' * 64,
  slot: 0,
);

// Transaction tests deliberately assert durable semantics through this fixture;
// filesystem durability is covered by the JournalStore regression suites.
class _DurableFixtureStore extends MemoryTransactionStore {
  @override
  bool get durable => true;
}

class _FailingStore extends _DurableFixtureStore {
  bool fail = false;
  @override
  Future<void> transact(void Function(StoreData) change) {
    if (fail) return Future.error(const AppFailure(FailureKind.storage));
    return super.transact(change);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final reconciling in [false, true]) {
    test(
      'failed native settlement closes queue admission; reconcile=$reconciling',
      () async {
        final backend = _Backend();
        final app = AppServices(
          storage: _DurableFixtureStore(),
          contentStaging: backend,
        );
        final job = await app.managedContent!.prepareLocalStage(
          _intent(),
          CancellationToken(),
          coordinator: app.coordinator,
        );
        if (reconciling) {
          await app.jobs.transition(app.session.scope, job, JobState.running);
          await app.jobs.transition(
            app.session.scope,
            job,
            JobState.recoveryRequired,
          );
        }
        await app.jobs.enqueue(app.session.scope, 'unavailable', 'sentinel');
        backend.failDrain = true;
        if (reconciling) {
          await app.coordinator.reconcile(app.session.scope, job);
        } else {
          await app.coordinator.drain(app.session.scope);
        }
        await app.coordinator.drain(app.session.scope);
        final records = app.jobs.read(app.session.scope);
        expect(records.first.state, JobState.recoveryRequired);
        expect(records.last.state, JobState.queued);
        expect(app.coordinator.acquireIdle, throwsA(isA<AppFailure>()));
        await app.recoverAfterResume();
        expect(app.lifecycleFailure?.kind, FailureKind.recovery);
        expect(app.coordinator.acquireIdle, throwsA(isA<AppFailure>()));
        backend.failDrain = false;
        await app.recoverAfterResume();
        expect(app.lifecycleFailure, isNull);
        await app.coordinator.drain(app.session.scope);
        expect(app.jobs.read(app.session.scope).last.state, JobState.blocked);
        await app.close();
      },
    );
  }
  test(
    'new suspension during native drain cannot be undone by older resume',
    () async {
      final backend = _Backend()..drainHold = Completer<void>();
      final app = AppServices(
        storage: _DurableFixtureStore(),
        contentStaging: backend,
      );
      await app.jobs.enqueue(app.session.scope, 'unavailable', 'queued');
      app.interruptLifecycle();
      final oldResume = app.recoverAfterResume();
      await Future<void>.delayed(Duration.zero);
      app.interruptLifecycle();
      backend.drainHold!.complete();
      await oldResume;
      await app.coordinator.drain(app.session.scope);
      expect(app.jobs.read(app.session.scope).single.state, JobState.queued);
      await app.recoverAfterResume();
      await app.coordinator.drain(app.session.scope);
      expect(app.jobs.read(app.session.scope).single.state, JobState.blocked);
      await app.close();
    },
  );
  test(
    'future managed envelope preserves the healthy application store',
    () async {
      final store = _DurableFixtureStore();
      await store.transact((draft) {
        // Version 2 is the supported manifested envelope and must validate its
        // records. An unknown future envelope remains opaque and unusable.
        draft['managedInstallations'] = {'version': 3, 'opaque': 'preserved'};
      });
      var connected = false;
      final app = await AppServices.bootstrap(
        openStore: () async => store,
        connectContentStaging: () async {
          connected = true;
          return _Backend();
        },
        connectLifecycle: () async => null,
        connectSecurity: () async => null,
        connectActivation: () async => null,
        connectUpdates: () async => null,
        connectTaskbar: () async => null,
        connectNotifications: () async => null,
        connectInstallations: () async => null,
      );
      expect(connected, isFalse);
      expect(app.storage, same(store));
      expect(app.storageFailure, isNull);
      expect(app.managedContent, isNull);
      expect(store.read()['managedInstallations'], {
        'version': 3,
        'opaque': 'preserved',
      });
      await app.close();
    },
  );
  test('intent and running inventory precede effects; inventory precedes job completion', () async {
    final backend = _Backend();
    final app = AppServices(
      storage: _DurableFixtureStore(),
      contentStaging: backend,
    );
    backend.beforeEffect = () {
      expect(
        app.managedInstallations!.readCurrent().single.state,
        ManagedInstallationState.staging,
      );
      expect(app.jobs.read(app.session.scope).single.state, JobState.running);
    };
    await app.managedContent!.prepareLocalStage(
      _intent(),
      CancellationToken(),
      coordinator: app.coordinator,
    );
    expect(backend.stages, 0);
    expect(
      app.managedInstallations!.readCurrent().single.state,
      ManagedInstallationState.prepared,
    );
    await app.coordinator.drain(app.session.scope);
    expect(
      app.managedInstallations!.readCurrent().single.state,
      ManagedInstallationState.staged,
    );
    expect(app.jobs.read(app.session.scope).single.state, JobState.completed);
    expect(app.jobs.read(app.session.scope).single.checkpoint, 600000);
    expect(app.activity.read(app.session.scope).length, 1);
    expect(app.library.entries, isEmpty);
    await app.close();
    expect(backend.closed, isTrue);
  });

  test('durable intent write failure cannot invoke staging', () async {
    final backend = _Backend();
    final store = _FailingStore()..fail = true;
    final app = AppServices(storage: store, contentStaging: backend);
    await expectLater(
      app.managedContent!.prepareLocalStage(
        _intent(),
        CancellationToken(),
        coordinator: app.coordinator,
      ),
      throwsA(isA<AppFailure>()),
    );
    expect(backend.stages, 0);
    expect(app.jobs.read(app.session.scope), isEmpty);
    store.fail = false;
    await app.close();
  });

  test('interruption retains recovery; explicit inspect observes completion without another copy', () async {
    final backend = _Backend()..hold = Completer<void>();
    final app = AppServices(
      storage: _DurableFixtureStore(),
      contentStaging: backend,
    );
    await app.managedContent!.prepareLocalStage(
      _intent(),
      CancellationToken(),
      coordinator: app.coordinator,
    );
    final running = app.coordinator.drain(app.session.scope);
    await backend.started.future;
    app.interruptLifecycle();
    var resumed = false;
    final resume = app.recoverAfterResume().then((_) => resumed = true);
    await Future<void>.delayed(Duration.zero);
    expect(resumed, isFalse);
    backend.hold!.complete();
    await Future.wait([running, resume]);
    expect(
      app.managedInstallations!.readCurrent().single.state,
      ManagedInstallationState.recoveryRequired,
    );
    final job = app.jobs.read(app.session.scope).single;
    expect(job.state, JobState.recoveryRequired);
    await app.coordinator.reconcile(app.session.scope, job);
    expect(backend.stages, 1);
    expect(
      app.managedInstallations!.readCurrent().single.state,
      ManagedInstallationState.staged,
    );
    expect(app.jobs.read(app.session.scope).single.state, JobState.completed);
    await app.close();
  });

  test(
    'account replacement hides inventory and suppresses late completion',
    () async {
      final backend = _Backend()..hold = Completer<void>();
      final app = AppServices(
        storage: _DurableFixtureStore(),
        contentStaging: backend,
      );
      final oldScope = app.session.scope;
      await app.managedContent!.prepareLocalStage(
        _intent(),
        CancellationToken(),
        coordinator: app.coordinator,
      );
      final running = app.coordinator.drain(oldScope);
      await backend.started.future;
      app.session.replace(const AccountScope.verified('fixture', 'different'));
      expect(app.managedInstallations!.readCurrent(), isEmpty);
      backend.hold!.complete();
      await running;
      expect(app.managedInstallations!.readCurrent(), isEmpty);
      expect(app.jobs.read(oldScope).single.state, JobState.recoveryRequired);
      expect(app.jobs.read(app.session.scope), isEmpty);
      await app.close();
    },
  );

  test(
    'close awaits real worker and durable recovery before releasing storage',
    () async {
      final backend = _Backend()..hold = Completer<void>();
      final store = _DurableFixtureStore();
      final app = AppServices(storage: store, contentStaging: backend);
      await app.managedContent!.prepareLocalStage(
        _intent(),
        CancellationToken(),
        coordinator: app.coordinator,
      );
      final running = app.coordinator.drain(app.session.scope);
      await backend.started.future;
      var closed = false;
      final closing = app.close().then((_) => closed = true);
      await Future<void>.delayed(Duration.zero);
      expect(closed, isFalse);
      backend.hold!.complete();
      await Future.wait([running, closing]);
      expect(closed, isTrue);
      expect(backend.closed, isTrue);
    },
  );

  test(
    'bootstrap downgrades interrupted metadata and never replays native work',
    () async {
      final store = _DurableFixtureStore();
      final backend = _Backend();
      final app = AppServices(storage: store, contentStaging: backend);
      await app.managedContent!.prepareLocalStage(
        _intent(),
        CancellationToken(),
        coordinator: app.coordinator,
      );
      await app.managedContent!.recoverInterrupted();
      expect(
        app.managedInstallations!.readCurrent().single.state,
        ManagedInstallationState.recoveryRequired,
      );
      await app.coordinator.drain(app.session.scope);
      expect(backend.stages, 0);
      expect(
        app.jobs.read(app.session.scope).single.state,
        JobState.recoveryRequired,
      );
      expect(ManagedContentService.jobKind, 'managedContentStage');
      await app.close();
    },
  );

  test('session-only storage cannot admit native effects', () async {
    final backend = _Backend();
    final app = AppServices(
      storage: MemoryTransactionStore(),
      contentStaging: backend,
    );
    expect(
      () => app.managedContent!.prepareLocalStage(
        _intent(),
        CancellationToken(),
        coordinator: app.coordinator,
      ),
      throwsA(isA<AppFailure>()),
    );
    expect(backend.inspections, 0);
    expect(backend.stages, 0);
    await app.close();
  });
}
