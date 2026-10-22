import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/installations/installation_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/features/store/catalog_repository.dart';

class _Store extends MemoryTransactionStore {
  _Store(this.events, {this.closeFailure});
  final List<String> events;
  final Object? closeFailure;
  int closes = 0;

  @override
  Future<void> close() async {
    closes++;
    events.add('store-close');
    await super.close();
    if (closeFailure != null) throw closeFailure!;
  }
}

class _Inspection implements InstallationRepository {
  _Inspection(this.failure);
  final Object failure;
  final done = Completer<RepositoryRead<InstallationObservation>>();
  late CancellationToken token;

  @override
  Future<RepositoryRead<InstallationObservation>> inspect(
    CancellationToken cancellation,
  ) {
    token = cancellation;
    token.onCancel(() => throw failure);
    return done.future;
  }

  void settle() => done.complete(
    RepositoryRead(
      const <InstallationObservation>[],
      quality: ReadQuality.complete,
      observedAt: DateTime.utc(2026),
    ),
  );
}

class _Catalog implements CatalogRepository {
  final done = Completer<RepositoryRead<EntertainmentTitle>>();
  late CancellationToken token;

  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) {
    token = cancellation;
    return done.future;
  }

  void settle() => done.complete(
    RepositoryRead(
      const <EntertainmentTitle>[],
      quality: ReadQuality.complete,
      observedAt: DateTime.utc(2026),
    ),
  );
}

class _Operation implements JobOperation {
  _Operation(this.events);
  final List<String> events;
  final entered = Completer<void>();
  final done = Completer<void>();
  late CancellationToken token;

  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken cancellation,
    Future<void> Function(int) checkpoint,
  ) async {
    token = cancellation;
    entered.complete();
    await done.future;
    events.add('operation-settled');
    return JobOutcome.completed;
  }

  @override
  Future<JobOutcome> reconcile(JobRecord job, CancellationToken cancellation) =>
      throw UnimplementedError('This fixture never reconciles');
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test(
    'interruption reaches remaining owners after inspection cancel throws',
    () async {
      final failure = StateError('synthetic inspection cancellation');
      final inspection = _Inspection(failure);
      final catalog = _Catalog();
      final app = AppServices(
        storage: MemoryTransactionStore(),
        catalogRepository: catalog,
      );
      app.installations.attach(inspection);
      final inspecting = app.installations.inspect();
      final browsing = app.discovery.load(EntertainmentCategory.games);
      try {
        expect(app.interruptLifecycle, throwsA(same(failure)));
        expect(inspection.token.isCancelled, isTrue);
        expect(catalog.token.isCancelled, isTrue);
        expect(
          app.discovery.state(EntertainmentCategory.games).loading,
          isFalse,
        );
        expect(app.coordinator.acquireIdle, throwsA(isA<AppFailure>()));
      } finally {
        inspection.settle();
        catalog.settle();
        await Future.wait([inspecting, browsing]);
        await app.close();
      }
    },
  );

  test(
    'account replacement clears other account UI after cancellation throws',
    () async {
      final failures = <FlutterErrorDetails>[];
      final previousHandler = FlutterError.onError;
      FlutterError.onError = failures.add;
      addTearDown(() => FlutterError.onError = previousHandler);
      final failure = StateError('synthetic old account cancellation');
      final inspection = _Inspection(failure);
      final app = AppServices.memory();
      app.session.replace(
        const AccountScope.verified('fixture', 'old-account'),
      );
      await app.store.toggle('private-favourite');
      final favourite = app.store.favourite('private-favourite');
      app.navigation.selectLibrary(true);
      app.navigation.setQuery('private-search');
      app.library.setQuery('private-library-search');
      app.installations.attach(inspection);
      final inspecting = app.installations.inspect();
      try {
        // ChangeNotifier reports listener failures after the scope is replaced.
        // The composition listener must finish resetting its other controllers.
        app.session.signOut();
        await Future<void>.delayed(Duration.zero);
        expect(app.session.scope.isLocal, isTrue);
        expect(app.navigation.libraryDestination, isFalse);
        expect(app.navigation.query, isEmpty);
        expect(app.library.query, isEmpty);
        expect(favourite.value, isFalse);
        expect(failures.map((details) => details.exception), [same(failure)]);
      } finally {
        inspection.settle();
        await inspecting;
        await app.close();
      }
    },
  );

  test('close joins real work and disposes every controller before reporting cancellation', () async {
    final events = <String>[];
    final storage = _Store(events);
    final operation = _Operation(events);
    final app = AppServices(
      storage: storage,
      jobOperations: {'fixture': operation},
    );
    final failure = StateError('synthetic session cancellation');
    app.session.beginWork().onCancel(() => throw failure);
    final scope = app.session.scope;
    await app.jobs.enqueue(scope, 'fixture', 'item');
    final running = app.coordinator.drain(scope);
    await operation.entered.future;
    final closing = app.close();
    var observed = false;
    final rejected = expectLater(closing, throwsA(same(failure))).then((_) {
      observed = true;
    });
    try {
      await Future<void>.delayed(Duration.zero);
      expect(observed, isFalse);
      expect(storage.closes, 0);
      expect(operation.token.isCancelled, isTrue);
      expect(app.coordinator.acquireIdle, throwsA(isA<AppFailure>()));
      expect(() => app.navigation.addListener(() {}), throwsFlutterError);
      var opened = false;
      await app.overlays.run(() async {
        opened = true;
        return true;
      });
      expect(opened, isFalse);
    } finally {
      operation.done.complete();
      await running;
      await rejected;
    }
    expect(storage.closes, 1);
    expect(events, ['operation-settled', 'store-close']);
    expect(app.jobs.read(scope).single.state, JobState.recoveryRequired);
    expect(app.close(), same(closing));
    await expectLater(app.close(), throwsA(same(failure)));
  });

  test(
    'close retains first cancellation failure after failed store close',
    () async {
      final first = StateError('synthetic cancellation');
      final storage = _Store(
        [],
        closeFailure: StateError('synthetic store close'),
      );
      final app = AppServices(storage: storage);
      app.session.beginWork().onCancel(() => throw first);
      await expectLater(app.close(), throwsA(same(first)));
      expect(storage.closes, 1);
    },
  );

  test(
    'reentrant close shares one future and one durable store close',
    () async {
      final storage = _Store([]);
      final app = AppServices(storage: storage);
      Future<void>? reentrant;
      app.session.beginWork().onCancel(() => reentrant = app.close());
      final closing = app.close();
      expect(reentrant, same(closing));
      await closing;
      expect(storage.closes, 1);
      expect(app.dispose, returnsNormally);
      await app.close();
      expect(storage.closes, 1);
    },
  );
}
