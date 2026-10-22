import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_installation_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

import 'support/journal_fixture.dart';

String _hex(int value, int length) =>
    value.toRadixString(16).padLeft(length, '0');
final _rootIdentity = '${_hex(7, 16)}:${_hex(8, 32)}';
final _sourceIdentity = '${_hex(7, 16)}:${_hex(9, 32)}';

ManagedStageIntent _intent({
  int operation = 1,
  String item = 'Title',
  String namespace = 'Games',
  int slot = 0,
  int size = managedStageChunkBytes + 7,
  String source = 'payloads/source.bin',
}) => ManagedStageIntent(
  operationId: _hex(operation, 64),
  item: ProviderItemRef('Provider', item, namespace: namespace),
  packageId: 'Package',
  version: '1.0.0+Exact',
  manifestSha256: _hex(2, 64),
  sourceRelative: source,
  expectedSize: size,
  expectedSha256: _hex(3, 64),
  slot: slot,
);

ManagedStageObservation _observation(
  ManagedStageIntent intent, {
  ManagedStageObservationState state = ManagedStageObservationState.checkpoint,
  int bytes = 0,
  int chunks = 0,
  String? rootIdentity,
  String? sourceIdentity,
}) => ManagedStageObservation(
  operationId: intent.operationId,
  state: state,
  verifiedBytes: bytes,
  verifiedChunks: chunks,
  rootIdentity: rootIdentity ?? _rootIdentity,
  sourceIdentity: sourceIdentity ?? _sourceIdentity,
);

Matcher _failure(FailureKind kind) =>
    throwsA(isA<AppFailure>().having((value) => value.kind, 'kind', kind));

void main() {
  late MemoryTransactionStore store;
  late SessionController session;
  late ManagedInstallationRepository repository;
  setUp(() {
    store = MemoryTransactionStore();
    session = SessionController();
    repository = ManagedInstallationRepository(
      store,
      session,
      deviceId: 'device-A',
    );
  });
  tearDown(() async {
    repository.dispose();
    session.dispose();
    await store.close();
  });

  test(
    'prepare requires a native zero checkpoint and exact identities',
    () async {
      final intent = _intent();
      final cancellation = CancellationToken();
      await expectLater(
        repository.prepare(
          intent,
          _observation(intent, sourceIdentity: ''),
          cancellation,
        ),
        _failure(FailureKind.recovery),
      );
      await expectLater(
        repository.prepare(
          intent,
          _observation(intent, bytes: managedStageChunkBytes, chunks: 1),
          cancellation,
        ),
        _failure(FailureKind.recovery),
      );
      expect(repository.readCurrent(), isEmpty);
      final lease = await repository.prepare(
        intent,
        _observation(intent),
        cancellation,
      );
      expect(lease.record.state, ManagedInstallationState.prepared);
      expect(lease.record.intent.item.namespace, 'Games');
      expect(lease.record.intent.version, '1.0.0+Exact');
      expect(lease.record.rootIdentity, _rootIdentity);
      expect(lease.record.owningInstaller, 'ryhze');
      final duplicate = await repository.prepare(
        intent,
        _observation(intent),
        cancellation,
      );
      expect(duplicate.record.revision, lease.record.revision);
      expect(repository.readCurrent(), hasLength(1));
    },
  );

  test('CAS admits one worker and refuses old lease success', () async {
    final intent = _intent();
    final token = CancellationToken();
    final prepared = await repository.prepare(
      intent,
      _observation(intent),
      token,
    );
    final duplicate = repository.leaseCurrent(intent.operationId)!;
    final active = await repository.beginStage(prepared, token);
    await expectLater(
      repository.beginStage(duplicate, token),
      _failure(FailureKind.conflict),
    );
    await expectLater(
      repository.reconcile(
        prepared,
        _observation(
          intent,
          state: ManagedStageObservationState.staged,
          bytes: intent.expectedSize,
          chunks: intent.expectedChunks,
        ),
        token,
      ),
      _failure(FailureKind.conflict),
    );
    final staged = await repository.reconcile(
      active,
      _observation(
        intent,
        state: ManagedStageObservationState.staged,
        bytes: intent.expectedSize,
        chunks: intent.expectedChunks,
      ),
      token,
    );
    expect(staged.record.state, ManagedInstallationState.staged);
    expect(staged.record.revision, 3);
    await expectLater(
      repository.beginStage(staged, token),
      _failure(FailureKind.recovery),
    );
  });

  test(
    'reservations span accounts and exact namespaces stay distinct',
    () async {
      final token = CancellationToken();
      final first = _intent();
      await repository.prepare(first, _observation(first), token);
      session.replace(const AccountScope.verified('Provider', 'Other-account'));
      expect(repository.readCurrent(), isEmpty);
      expect(repository.findCurrent(first.operationId), isNull);
      final sameItem = _intent(operation: 2, slot: 1);
      await expectLater(
        repository.prepare(sameItem, _observation(sameItem), token),
        _failure(FailureKind.conflict),
      );
      final sameSlot = _intent(operation: 3, item: 'Other');
      await expectLater(
        repository.prepare(sameSlot, _observation(sameSlot), token),
        _failure(FailureKind.conflict),
      );
      final distinct = _intent(operation: 4, namespace: 'games', slot: 1);
      await repository.prepare(distinct, _observation(distinct), token);
      expect(repository.readCurrent().single.intent.item.namespace, 'games');
      final sameOperation = _intent(operation: 1, item: 'Other', slot: 2);
      await expectLater(
        repository.prepare(sameOperation, _observation(sameOperation), token),
        _failure(FailureKind.conflict),
      );
    },
  );

  test('partial cancellation retains slot; explicit pre-effect cancellation releases', () async {
    final token = CancellationToken();
    final first = _intent();
    final prepared = await repository.prepare(
      first,
      _observation(first),
      token,
    );
    final cancelled = await repository.cancelPrepared(prepared, token);
    expect(cancelled.record.reservesContent, isFalse);
    final second = _intent(operation: 2);
    var active = await repository.prepare(second, _observation(second), token);
    active = await repository.beginStage(active, token);
    await expectLater(
      repository.cancelPrepared(active, token),
      _failure(FailureKind.recovery),
    );
    final interrupted = await repository.reconcile(
      active,
      _observation(
        second,
        state: ManagedStageObservationState.cancelled,
        bytes: managedStageChunkBytes,
        chunks: 1,
      ),
      token,
    );
    expect(interrupted.record.state, ManagedInstallationState.recoveryRequired);
    final replacement = _intent(operation: 3, item: 'Other');
    await expectLater(
      repository.prepare(replacement, _observation(replacement), token),
      _failure(FailureKind.conflict),
    );
    await expectLater(
      repository.beginStage(interrupted, token),
      _failure(FailureKind.recovery),
    );
  });

  test(
    'staged is reserved and never becomes an installed or entitlement fact',
    () async {
      final token = CancellationToken();
      final intent = _intent();
      var lease = await repository.prepare(intent, _observation(intent), token);
      lease = await repository.beginStage(lease, token);
      lease = await repository.reconcile(
        lease,
        _observation(
          intent,
          state: ManagedStageObservationState.staged,
          bytes: intent.expectedSize,
          chunks: intent.expectedChunks,
        ),
        token,
      );
      expect(lease.record.reservesContent, isTrue);
      final replacement = _intent(operation: 2, item: 'Other');
      await expectLater(
        repository.prepare(replacement, _observation(replacement), token),
        _failure(FailureKind.conflict),
      );
      expect((store.read()['accounts'] as Map), isEmpty);
    },
  );

  test('explicit reinspection can lower verified counts without discarding metadata', () async {
    final token = CancellationToken();
    final intent = _intent();
    var lease = await repository.prepare(intent, _observation(intent), token);
    lease = await repository.beginStage(lease, token);
    lease = await repository.reconcile(
      lease,
      _observation(
        intent,
        state: ManagedStageObservationState.cancelled,
        bytes: managedStageChunkBytes,
        chunks: 1,
      ),
      token,
    );
    expect(lease.record.verifiedBytes, managedStageChunkBytes);
    lease = await repository.reconcile(
      lease,
      _observation(intent, state: ManagedStageObservationState.recovery),
      token,
    );
    expect(lease.record.state, ManagedInstallationState.recoveryRequired);
    expect(lease.record.verifiedBytes, 0);
    expect(lease.record.sourceIdentity, _sourceIdentity);
    lease = await repository.reconcile(
      lease,
      _observation(
        intent,
        state: ManagedStageObservationState.staged,
        bytes: intent.expectedSize,
        chunks: intent.expectedChunks,
      ),
      token,
    );
    expect(lease.record.state, ManagedInstallationState.staged);
  });

  test(
    'replacement source identity and incomplete claimed success stay recovery',
    () async {
      final token = CancellationToken();
      final intent = _intent();
      var lease = await repository.prepare(intent, _observation(intent), token);
      lease = await repository.beginStage(lease, token);
      lease = await repository.reconcile(
        lease,
        _observation(
          intent,
          state: ManagedStageObservationState.staged,
          sourceIdentity: '${_hex(7, 16)}:${_hex(99, 32)}',
          bytes: intent.expectedSize,
          chunks: intent.expectedChunks,
        ),
        token,
      );
      expect(lease.record.state, ManagedInstallationState.recoveryRequired);
      expect(lease.record.sourceIdentity, _sourceIdentity);
      expect(lease.record.verifiedBytes, 0);
      lease = await repository.reconcile(
        lease,
        _observation(
          intent,
          state: ManagedStageObservationState.staged,
          bytes: managedStageChunkBytes,
          chunks: 1,
        ),
        token,
      );
      expect(lease.record.state, ManagedInstallationState.recoveryRequired);
      await expectLater(
        repository.reconcile(
          lease,
          _observation(
            intent,
            state: ManagedStageObservationState.staged,
            bytes: intent.expectedSize,
            chunks: 1,
          ),
          token,
        ),
        _failure(FailureKind.invalidData),
      );
    },
  );

  test(
    'old account completion is rejected but safety downgrade remains possible',
    () async {
      final token = CancellationToken();
      final intent = _intent();
      var lease = await repository.prepare(intent, _observation(intent), token);
      lease = await repository.beginStage(lease, token);
      session.replace(const AccountScope.verified('Provider', 'Next'));
      await expectLater(
        repository.reconcile(
          lease,
          _observation(
            intent,
            state: ManagedStageObservationState.staged,
            bytes: intent.expectedSize,
            chunks: intent.expectedChunks,
          ),
          token,
        ),
        _failure(FailureKind.cancelled),
      );
      final recovery = await repository.requireRecovery(lease);
      expect(recovery.record.state, ManagedInstallationState.recoveryRequired);
      expect(repository.readCurrent(), isEmpty);
      session.signOut();
      expect(
        repository.readCurrent().single.state,
        ManagedInstallationState.recoveryRequired,
      );
      await expectLater(
        repository.beginStage(lease, token),
        _failure(FailureKind.cancelled),
      );
    },
  );

  test(
    'queued account change and cancellation cannot admit durable work',
    () async {
      final gated = _ControlledStore();
      final scoped = ManagedInstallationRepository(
        gated,
        session,
        deviceId: 'device-A',
      );
      final intent = _intent();
      gated.holdNext();
      final pending = scoped.prepare(
        intent,
        _observation(intent),
        CancellationToken(),
      );
      final rejected = expectLater(pending, _failure(FailureKind.cancelled));
      await gated.entered.future;
      session.replace(const AccountScope.verified('Provider', 'Other'));
      gated.release.complete();
      await rejected;
      expect(scoped.readCurrent(), isEmpty);
      final token = CancellationToken();
      gated.holdNext();
      final second = scoped.prepare(intent, _observation(intent), token);
      final cancelled = expectLater(second, _failure(FailureKind.cancelled));
      await gated.entered.future;
      token.cancel();
      gated.release.complete();
      await cancelled;
      expect(scoped.readCurrent(), isEmpty);
      scoped.dispose();
      await gated.close();
    },
  );

  test(
    'write failure leaves prepared lease and cannot grant stage start',
    () async {
      final controlled = _ControlledStore();
      final owned = ManagedInstallationRepository(
        controlled,
        session,
        deviceId: 'device-A',
      );
      final intent = _intent();
      final token = CancellationToken();
      final lease = await owned.prepare(intent, _observation(intent), token);
      controlled.failNext = true;
      await expectLater(
        owned.beginStage(lease, token),
        _failure(FailureKind.storage),
      );
      expect(
        owned.findCurrent(intent.operationId)!.state,
        ManagedInstallationState.prepared,
      );
      expect(
        owned.findCurrent(intent.operationId)!.revision,
        lease.record.revision,
      );
      owned.dispose();
      await controlled.close();
    },
  );

  test('known envelope and nested extensions survive transitions', () async {
    final intent = _intent();
    final token = CancellationToken();
    var lease = await repository.prepare(intent, _observation(intent), token);
    await store.transact((draft) {
      final envelope = draft['managedInstallations'] as Map;
      envelope['futureEnvelope'] = {'retained': true};
      final record = (envelope['records'] as Map)[intent.operationId] as Map;
      record['futureRecord'] = ['preserved'];
      (record['account'] as Map)['futureAccount'] = 7;
      (record['intent'] as Map)['futureIntent'] = {'retain': 'exact'};
    });
    lease = await repository.beginStage(lease, token);
    await repository.requireRecovery(lease);
    final envelope = store.read()['managedInstallations'] as Map;
    final record = (envelope['records'] as Map)[intent.operationId] as Map;
    expect(envelope['futureEnvelope'], {'retained': true});
    expect(record['futureRecord'], ['preserved']);
    expect((record['account'] as Map)['futureAccount'], 7);
    expect((record['intent'] as Map)['futureIntent'], {'retain': 'exact'});
  });

  test(
    'unknown envelope is preserved while managed operations are unavailable',
    () async {
      final future = {
        'version': 3,
        'future': {
          'opaque': ['preserve'],
        },
      };
      validateManagedInstallations(future);
      await store.transact((draft) => draft['managedInstallations'] = future);
      expect(() => repository.readCurrent(), _failure(FailureKind.unsupported));
      final intent = _intent();
      await expectLater(
        repository.prepare(intent, _observation(intent), CancellationToken()),
        _failure(FailureKind.unsupported),
      );
      await store.transact((draft) => draft['unrelated'] = true);
      expect(store.read()['managedInstallations'], future);
      expect(store.read()['unrelated'], isTrue);
    },
  );

  test(
    'restart durably marks interrupted records across accounts without replay',
    () async {
      final Directory root = await createJournalFixture('ryhze-managed-');
      var journal = await JournalStore.open(root);
      var durable = ManagedInstallationRepository(
        journal,
        session,
        deviceId: 'device-A',
      );
      final token = CancellationToken();
      final first = _intent();
      var active = await durable.prepare(first, _observation(first), token);
      active = await durable.beginStage(active, token);
      session.replace(const AccountScope.verified('Provider', 'Other'));
      final second = _intent(operation: 2, item: 'Other', slot: 1);
      await durable.prepare(second, _observation(second), token);
      durable.dispose();
      await journal.close();
      journal = await JournalStore.open(root);
      durable = ManagedInstallationRepository(
        journal,
        session,
        deviceId: 'device-A',
      );
      expect(await durable.recoverInterrupted(), 2);
      expect(await durable.recoverInterrupted(), 0);
      expect(
        durable.readCurrent().single.state,
        ManagedInstallationState.recoveryRequired,
      );
      session.signOut();
      final recovered = durable.leaseCurrent(first.operationId)!;
      expect(recovered.record.state, ManagedInstallationState.recoveryRequired);
      expect(recovered.record.intent.sameRequest(first), isTrue);
      expect(recovered.record.sourceIdentity, _sourceIdentity);
      await expectLater(
        durable.beginStage(recovered, token),
        _failure(FailureKind.recovery),
      );
      durable.dispose();
      await journal.close();
      journal = await JournalStore.open(root);
      durable = ManagedInstallationRepository(
        journal,
        session,
        deviceId: 'device-A',
      );
      expect(
        durable.readCurrent().single.state,
        ManagedInstallationState.recoveryRequired,
      );
      durable.dispose();
      await journal.close();
      await root.delete(recursive: true);
    },
  );

  test(
    'bootstrap invalidates a running lease before a late native result',
    () async {
      final token = CancellationToken();
      final intent = _intent();
      var lease = await repository.prepare(intent, _observation(intent), token);
      lease = await repository.beginStage(lease, token);
      expect(await repository.recoverInterrupted(), 1);
      await expectLater(
        repository.reconcile(
          lease,
          _observation(
            intent,
            state: ManagedStageObservationState.staged,
            bytes: intent.expectedSize,
            chunks: intent.expectedChunks,
          ),
          token,
        ),
        _failure(FailureKind.conflict),
      );
      expect(
        repository.findCurrent(intent.operationId)!.state,
        ManagedInstallationState.recoveryRequired,
      );
    },
  );

  test(
    'recovery without current-device interrupted records makes no write',
    () async {
      final counted = _ControlledStore();
      final current = ManagedInstallationRepository(
        counted,
        session,
        deviceId: 'device-A',
      );
      final otherDevice = ManagedInstallationRepository(
        counted,
        session,
        deviceId: 'device-B',
      );
      expect(await current.recoverInterrupted(), 0);
      expect(counted.transactionCount, 0);
      final intent = _intent();
      await otherDevice.prepare(
        intent,
        _observation(intent),
        CancellationToken(),
      );
      final writes = counted.transactionCount;
      expect(await current.recoverInterrupted(), 0);
      expect(counted.transactionCount, writes);
      expect(
        otherDevice.findCurrent(intent.operationId)!.state,
        ManagedInstallationState.prepared,
      );
      expect(await otherDevice.recoverInterrupted(), 1);
      final afterRecovery = counted.transactionCount;
      expect(await otherDevice.recoverInterrupted(), 0);
      expect(counted.transactionCount, afterRecovery);
      current.dispose();
      otherDevice.dispose();
      await counted.close();
    },
  );

  test(
    'prepare committed before account replacement is durably downgraded',
    () async {
      final controlled = _ControlledStore();
      final owned = ManagedInstallationRepository(
        controlled,
        session,
        deviceId: 'device-A',
      );
      final intent = _intent();
      controlled.afterCommit = () => session.replace(
        const AccountScope.verified('Provider', 'Replacement'),
      );
      await expectLater(
        owned.prepare(intent, _observation(intent), CancellationToken()),
        _failure(FailureKind.cancelled),
      );
      expect(owned.readCurrent(), isEmpty);
      session.signOut();
      final record = owned.readCurrent().single;
      expect(record.state, ManagedInstallationState.recoveryRequired);
      expect(record.revision, 2);
      expect(record.verifiedBytes, 0);
      owned.dispose();
      await controlled.close();
    },
  );

  test(
    'stage-start commit followed by cancellation retains recovery reservation',
    () async {
      final controlled = _ControlledStore();
      final owned = ManagedInstallationRepository(
        controlled,
        session,
        deviceId: 'device-A',
      );
      final intent = _intent();
      final token = CancellationToken();
      final prepared = await owned.prepare(intent, _observation(intent), token);
      controlled.afterCommit = token.cancel;
      await expectLater(
        owned.beginStage(prepared, token),
        _failure(FailureKind.cancelled),
      );
      final record = owned.readCurrent().single;
      expect(record.state, ManagedInstallationState.recoveryRequired);
      expect(record.revision, 3);
      final replacement = _intent(operation: 2, item: 'Other');
      await expectLater(
        owned.prepare(
          replacement,
          _observation(replacement),
          CancellationToken(),
        ),
        _failure(FailureKind.conflict),
      );
      owned.dispose();
      await controlled.close();
    },
  );
}

class _ControlledStore implements TransactionStore {
  final _delegate = MemoryTransactionStore();
  int transactionCount = 0;
  void Function()? afterCommit;
  bool failNext = false;
  bool _hold = false;
  late Completer<void> entered;
  late Completer<void> release;
  void holdNext() {
    _hold = true;
    entered = Completer<void>();
    release = Completer<void>();
  }

  @override
  bool get durable => false;
  @override
  StoreData read() => _delegate.read();
  @override
  Future<void> transact(void Function(StoreData) update) async {
    transactionCount++;
    if (_hold) {
      _hold = false;
      entered.complete();
      await release.future;
    }
    if (failNext) {
      failNext = false;
      throw const AppFailure(FailureKind.storage);
    }
    await _delegate.transact(update);
    final completion = afterCommit;
    afterCommit = null;
    completion?.call();
  }

  @override
  Future<void> close() => _delegate.close();
}
