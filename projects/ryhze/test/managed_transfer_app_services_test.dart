import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/activity/activity_repository.dart';
import 'package:ryhze/features/collections/collection_repository.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/installations/content_transfer_backend.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/features/store/catalog_repository.dart';

import 'managed_transfer_fixture.dart';

// Composition-only fixtures: no provider authority, network or native effect.
// The native suites and real HTTP tests establish those boundaries separately.
class _TrackedStore extends MemoryTransactionStore {
  _TrackedStore(this.events);
  final List<String> events;
  bool closed = false;
  @override
  bool get durable => true;
  @override
  Future<void> close() async {
    await super.close();
    closed = true;
    events.add('store-closed');
  }
}

class _HeldTransferBackend extends TransferBackend {
  _HeldTransferBackend(this.events);
  final List<String> events;
  Completer<void>? drainEntered;
  void holdDrain() {
    drainEntered = Completer<void>();
    drainGate = Completer<void>();
  }

  void releaseDrain() {
    final gate = drainGate;
    if (gate != null && !gate.isCompleted) gate.complete();
  }

  @override
  Future<void> drain() async {
    final entered = drainEntered;
    if (entered != null && !entered.isCompleted) entered.complete();
    await super.drain();
    events.add('native-drained');
  }

  @override
  Future<void> close() async {
    await super.close();
    events.add('transfer-closed');
  }
}

Future<AppServices> _bootstrap(
  _TrackedStore store,
  Future<ContentTransferBackend?> Function() transfer,
) => AppServices.bootstrap(
  openStore: () async => store,
  catalogRepository: LocalCatalogRepository(),
  connectContentTransfer: transfer,
  connectContentStaging: () async => null,
  connectArtwork: () async => null,
  connectLifecycle: () async => null,
  connectSecurity: () async => null,
  connectActivation: () async => null,
  connectUpdates: () async => null,
  connectTaskbar: () async => null,
  connectNotifications: () async => null,
  connectInstallations: () async => null,
);

Future<void> _seedActivity(_TrackedStore store, AccountScope scope) =>
    store.transact((draft) {
      ActivityRepository.recordIn(
        draft,
        scope,
        'retained-prior-operation',
        ActivityKind.recovery,
        DateTime.utc(2026, 10, 10),
      );
    });

ManagedStageIntent _intent() => ManagedStageIntent(
  operationId: 'a' * 64,
  item: ProviderItemRef('test-provider', 'game'),
  packageId: 'test-only-package',
  version: '1',
  manifestSha256: 'b' * 64,
  sourceRelative: 'transfer-0/${'a' * 64}-0.bin',
  expectedSize: 3,
  expectedSha256: 'c' * 64,
  slot: 0,
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test(
    'optional transfer channel failure preserves durable browsing and Activity',
    () async {
      final events = <String>[];
      final store = _TrackedStore(events);
      const scope = AccountScope.local();
      await _seedActivity(store, scope);
      await CollectionRepository(store)
          .setSaved(scope, const SavedTitle('kept', 'Kept title'), true);
      final app = await _bootstrap(store, () async {
        throw const AppFailure(FailureKind.unsupported);
      });
      addTearDown(app.close);
      expect(app.storage, same(store));
      expect(app.storageFailure, isNull);
      expect(app.managedTransfer, isNull);
      expect(app.transferController, isNull);
      expect(
        app.discovery.state(EntertainmentCategory.games).titles,
        isNotEmpty,
      );
      expect(app.collections.read(scope).single.id, 'kept');
      expect(app.activity.read(scope).single.kind, ActivityKind.recovery);
      await app.savePreferences(const Preferences(reduceMotion: true));
      expect(app.preferences.read().reduceMotion, isTrue);
      expect(store.closed, isFalse);
    },
  );

  test(
    'future transfer envelope remains opaque and never connects native adapter',
    () async {
      final store = _TrackedStore([]);
      const future = {
        'version': 99,
        'opaque': 'preserve exact future transfer data',
      };
      await store.transact((draft) {
        draft['managedTransfers'] = future;
      });
      var connects = 0;
      final app = await _bootstrap(store, () async {
        connects++;
        return TransferBackend();
      });
      addTearDown(app.close);
      expect(connects, 0);
      expect(app.storage, same(store));
      expect(app.storageFailure, isNull);
      expect(app.managedTransfer, isNull);
      expect(store.read()['managedTransfers'], future);
      await app.savePreferences(const Preferences(privatePreviews: false));
      expect(store.read()['managedTransfers'], future);
      expect(
        app.discovery.state(EntertainmentCategory.games).titles,
        isNotEmpty,
      );
    },
  );

  test(
    'native transfer presence does not configure production content authority',
    () async {
      final backend = _HeldTransferBackend([]);
      final store = _TrackedStore([]);
      final app = AppServices(storage: store, contentTransfer: backend);
      addTearDown(app.close);
      app.session.replace(
        const AccountScope.verified('test-issuer', 'test-account'),
      );
      expect(app.managedTransfer, isNotNull);
      expect(app.transferController, isNotNull);
      expect(app.managedTransfer!.available, isFalse);
      expect(app.managedTransfer!.authority.available, isFalse);
      await expectLater(
        app.managedTransfer!.prepare(
          _intent(),
          CancellationToken(),
          coordinator: app.coordinator,
        ),
        throwsA(isA<AppFailure>()),
      );
      expect(backend.inspections, 0);
      expect(backend.begins, 0);
      expect(backend.appends, 0);
      expect(app.jobs.read(app.session.scope), isEmpty);
      expect(app.transfers!.readCurrent(), isEmpty);
    },
  );

  for (final boundary in ['close', 'suspend', 'session-change']) {
    test(
      '$boundary waits for actual transfer drain before closing store and retains prior Activity',
      () async {
        final events = <String>[];
        final store = _TrackedStore(events);
        final backend = _HeldTransferBackend(events);
        const oldScope = AccountScope.verified(
          'test-issuer',
          'OriginalAccount',
        );
        const newScope = AccountScope.verified('test-issuer', 'OtherAccount');
        await _seedActivity(store, oldScope);
        final originalActivity = ActivityRepository(store)
            .read(oldScope)
            .single;
        final app = AppServices(storage: store, contentTransfer: backend);
        app.session.replace(oldScope);
        addTearDown(() async {
          backend.releaseDrain();
          await app.close();
        });
        backend.holdDrain();
        Future<void>? recovering;
        if (boundary == 'suspend') {
          app.interruptLifecycle();
          expect(backend.cancels, greaterThan(0));
          expect(app.coordinator.acquireIdle, throwsA(isA<AppFailure>()));
          recovering = app.recoverAfterResume();
        } else if (boundary == 'session-change') {
          recovering = app.recoverAfterResume();
          await backend.drainEntered!.future;
          final beforeCancel = backend.cancels;
          final oldWork = app.session.beginWork();
          app.session.replace(newScope);
          expect(oldWork.isCancelled, isTrue);
          expect(backend.cancels, greaterThan(beforeCancel));
          expect(app.activity.read(newScope), isEmpty);
          expect(app.activity.read(oldScope).single.id, originalActivity.id);
        }
        if (boundary != 'close') await backend.drainEntered!.future;
        final closing = app.close();
        var closed = false;
        unawaited(
          closing.then((_) {
            closed = true;
          }),
        );
        await backend.drainEntered!.future;
        await Future<void>.delayed(Duration.zero);
        expect(closed, isFalse);
        expect(store.closed, isFalse);
        expect(events, isNot(contains('store-closed')));
        backend.releaseDrain();
        await recovering;
        await closing;
        expect(store.closed, isTrue);
        expect(
          events.indexOf('native-drained'),
          lessThan(events.indexOf('store-closed')),
        );
        expect(
          events.indexOf('transfer-closed'),
          lessThan(events.indexOf('store-closed')),
        );
        final retained = ActivityRepository(store).read(oldScope).single;
        expect(retained.id, originalActivity.id);
        expect(retained.kind, originalActivity.kind);
        expect(retained.occurredAt, originalActivity.occurredAt);
        expect(retained.count, originalActivity.count);
        expect(backend.begins, 0);
        expect(backend.appends, 0);
      },
    );
  }
}
