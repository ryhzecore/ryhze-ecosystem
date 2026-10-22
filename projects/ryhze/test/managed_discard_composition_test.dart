import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/content_discard_backend.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/managed_installation.dart';

// Synthetic effects exercise the real composition/lifecycle ownership. Native
// filesystem safety and physical Windows power acceptance remain separate.
final _root = '${'1' * 16}:${'2' * 32}';
final _source = '${'1' * 16}:${'3' * 32}';
final _id = 'a' * 64;

class _Store extends MemoryTransactionStore {
  _Store(this.events);
  final List<String> events;
  @override
  bool get durable => true;
  @override
  Future<void> close() async {
    events.add('store-close');
    await super.close();
  }
}

class _Backend implements ContentStagingBackend, ContentDiscardBackend {
  _Backend(this.events);
  final List<String> events;
  @override
  String get deviceId => _root;
  @override
  bool discardAvailable = true;
  Completer<void>? planHold;
  final planning = Completer<void>();
  bool failDrain = false;
  int deletions = 0;
  @override
  Future<ContentDiscardPlanReceipt> planDiscard(
    ContentStagingRequest request,
    CancellationToken token,
  ) async {
    events.add('plan-start');
    if (!planning.isCompleted) planning.complete();
    await planHold?.future;
    events.add('plan-settled');
    return ContentDiscardPlanReceipt(
      state: ContentStagingState.staged,
      plan: Uint8List.fromList([1, 2, 3]),
      verifiedBytes: 3,
      verifiedChunks: 1,
    );
  }

  @override
  Future<ContentDiscardReceipt> inspectDiscard(
    Uint8List plan,
    CancellationToken token,
  ) async => const ContentDiscardReceipt(
    state: ContentDiscardState.ready,
    removedBytes: 0,
    removedFiles: 0,
  );
  @override
  Future<ContentDiscardReceipt> discard(
    Uint8List plan,
    CancellationToken token,
  ) async {
    deletions++;
    return const ContentDiscardReceipt(
      state: ContentDiscardState.completed,
      removedBytes: 3,
      removedFiles: 2,
    );
  }

  @override
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken token,
  ) => throw StateError('No staging effects expected');
  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken token,
  ) => throw StateError('No staging effects expected');
  @override
  Future<void> cancel() async {
    events.add('cancel');
  }

  @override
  Future<void> drain() async {
    events.add('drain');
    await planHold?.future;
    if (failDrain) throw const AppFailure(FailureKind.recovery);
  }

  @override
  Future<void> close() async {
    events.add('backend-close');
  }
}

Future<void> _seed(AppServices app) async {
  final repo = app.managedInstallations!;
  final intent = ManagedStageIntent(
    operationId: _id,
    item: ProviderItemRef('fixture', 'game', namespace: 'test'),
    packageId: 'fixture',
    version: '1',
    manifestSha256: 'b' * 64,
    sourceRelative: 'fixture.bin',
    expectedSize: 3,
    expectedSha256: 'c' * 64,
    slot: 0,
  );
  ManagedStageObservation observation(int bytes) => ManagedStageObservation(
    operationId: _id,
    state: bytes == 0
        ? ManagedStageObservationState.checkpoint
        : ManagedStageObservationState.staged,
    verifiedBytes: bytes,
    verifiedChunks: bytes == 0 ? 0 : 1,
    rootIdentity: _root,
    sourceIdentity: _source,
  );
  var lease = await repo.prepare(intent, observation(0), CancellationToken());
  lease = await repo.beginStage(lease, CancellationToken());
  await repo.reconcile(lease, observation(3), CancellationToken());
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test(
    'close joins discard planning before shared channel and durable store',
    () async {
      final events = <String>[];
      final backend = _Backend(events)..planHold = Completer<void>();
      final app = AppServices(storage: _Store(events), contentStaging: backend);
      await _seed(app);
      final review = app.managedContentController!.reviewRemoval(_id);
      await backend.planning.future;
      final closing = app.close();
      await Future<void>.delayed(Duration.zero);
      expect(events, isNot(contains('backend-close')));
      expect(events, isNot(contains('store-close')));
      backend.planHold!.complete();
      await review;
      await closing;
      expect(
        events.indexOf('plan-settled'),
        lessThan(events.indexOf('backend-close')),
      );
      expect(
        events.indexOf('backend-close'),
        lessThan(events.indexOf('store-close')),
      );
      expect(events.where((e) => e == 'backend-close'), hasLength(1));
      expect(events.where((e) => e == 'store-close'), hasLength(1));
      expect(backend.deletions, 0);
    },
  );
  test('settlement failure still closes shared channel before store', () async {
    final events = <String>[];
    final backend = _Backend(events)..failDrain = true;
    final app = AppServices(storage: _Store(events), contentStaging: backend);
    await expectLater(app.close(), throwsA(isA<AppFailure>()));
    expect(
      events.indexOf('backend-close'),
      lessThan(events.indexOf('store-close')),
    );
    expect(events.where((e) => e == 'backend-close'), hasLength(1));
    expect(backend.deletions, 0);
  });
  test(
    'suspend invalidates visible confirmation and requires fresh review',
    () async {
      final events = <String>[];
      final backend = _Backend(events);
      final app = AppServices(storage: _Store(events), contentStaging: backend);
      await _seed(app);
      final controller = app.managedContentController!;
      final old = (await controller.reviewRemoval(_id))!;
      expect(controller.isRemovalCurrent(old), isTrue);
      var invalidationObserved = false;
      controller.addListener(() {
        if (controller.removalConfirmation == null) invalidationObserved = true;
      });
      app.interruptLifecycle();
      expect(invalidationObserved, isTrue);
      expect(controller.removalConfirmation, isNull);
      await controller.confirmRemoval(old);
      expect(backend.deletions, 0);
      await app.recoverAfterResume();
      expect(app.lifecycleFailure, isNull);
      expect(controller.isRemovalCurrent(old), isFalse);
      expect(await controller.reviewRemoval(_id), isNotNull);
      expect(backend.deletions, 0);
      await app.close();
    },
  );
  test('optional discard absence preserves existing staging service', () async {
    final events = <String>[];
    final app = AppServices(
      storage: _Store(events),
      contentStaging: _Backend(events)..discardAvailable = false,
    );
    expect(app.managedContent, isNotNull);
    expect(app.managedDiscard, isNull);
    await app.close();
    expect(events.where((e) => e == 'backend-close'), hasLength(1));
  });
}
