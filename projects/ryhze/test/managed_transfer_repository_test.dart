import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/content_transfer_backend.dart';
import 'package:ryhze/features/installations/managed_content_manifest.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_installation_repository.dart';
import 'package:ryhze/features/installations/managed_transfer_record.dart';
import 'package:ryhze/features/installations/managed_transfer_repository.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

// Synthetic integrity/authority metadata is not a production provider approval.
String hex(int value, int length) =>
    value.toRadixString(16).padLeft(length, '0');
final root = '${hex(7, 16)}:${hex(8, 32)}';
final directory = '${hex(7, 16)}:${hex(9, 32)}';
final source = '${hex(7, 16)}:${hex(10, 32)}';
const scope = AccountScope.verified('fixture-authority', 'Account-A');

ManagedManifestProvenance provenance({
  String item = 'Game',
  int size = 100,
  DateTime? observed,
}) {
  final bytes = Uint8List.fromList(
    utf8.encode(
      jsonEncode({
        'version': 1,
        'provider': 'fixture',
        'item': item,
        'namespace': 'Games',
        'packageId': 'package',
        'packageVersion': '1',
        'payloadSize': size,
        'payloadSha256': 'b' * 64,
        'files': [
          {
            'path': 'game.exe',
            'size': size,
            'sha256': 'c' * 64,
            'executable': true,
          },
        ],
      }),
    ),
  );
  final time = observed ?? DateTime.utc(2026, 10, 12);
  return ManagedManifestProvenance.create(
    authority: 'fixture',
    policy: 'distribution',
    sourceManifestSha256: crypto.sha256.convert(bytes).toString(),
    normalizedBytes: bytes,
    observedAt: time,
    expiresAt: time.add(const Duration(minutes: 5)),
  );
}

ManagedStageIntent intent({
  String item = 'Game',
  int id = 1,
  int slot = 0,
  int size = 100,
}) {
  final manifest = provenance(item: item, size: size);
  final operation = hex(id, 64);
  return ManagedStageIntent(
    operationId: operation,
    item: ProviderItemRef('fixture', item, namespace: 'Games'),
    packageId: 'package',
    version: '1',
    manifestSha256: manifest.manifest.sha256,
    sourceRelative: 'transfer-$slot/$operation-0.bin',
    expectedSize: size,
    expectedSha256: 'b' * 64,
    slot: slot,
  );
}

ContentTransferRequest observation(
  ManagedTransferLease lease, {
  int bytes = 10,
  bool writing = false,
  String? sourceIdentity,
}) => ContentTransferRequest(
  slot: lease.record.request.slot,
  operationId: lease.record.operationId,
  expectedSize: lease.record.intent.expectedSize,
  expectedSha256: lease.record.intent.expectedSha256,
  attempt: lease.record.request.attempt,
  rootIdentity: root,
  directoryIdentity: directory,
  sourceIdentity: sourceIdentity ?? source,
  observedBytes: bytes,
  observedSha256: writing
      ? ''
      : bytes == lease.record.intent.expectedSize
      ? 'b' * 64
      : 'd' * 64,
);

ContentTransferReceipt discarded(ContentTransferRequest request) =>
    ContentTransferReceipt(
      state: ContentTransferState.discarded,
      rootIdentity: request.rootIdentity,
      directoryIdentity: request.directoryIdentity,
      sourceIdentity: request.sourceIdentity,
      sourceRelative: request.sourceRelative,
      observedBytes: request.observedBytes,
      observedSha256: request.observedSha256,
    );

Matcher failure(FailureKind kind) =>
    throwsA(isA<AppFailure>().having((e) => e.kind, 'kind', kind));

class Store extends MemoryTransactionStore {
  bool failCommit = false;
  void Function()? afterCommit;
  @override
  Future<void> transact(void Function(StoreData) update) async {
    await super.transact((draft) {
      update(draft);
      if (failCommit) throw const AppFailure(FailureKind.storage);
    });
    final callback = afterCommit;
    afterCommit = null;
    callback?.call();
  }
}

void main() {
  late Store store;
  late SessionController session;
  late ManagedTransferRepository repository;
  late ManagedInstallationRepository staging;
  late JobRepository jobs;
  setUp(() {
    store = Store();
    session = SessionController()..replace(scope);
    repository = ManagedTransferRepository(
      store,
      session,
      deviceId: 'device-A',
    );
    staging = ManagedInstallationRepository(
      store,
      session,
      deviceId: 'device-A',
    );
    jobs = JobRepository(store);
  });
  tearDown(() async {
    repository.dispose();
    staging.dispose();
    jobs.dispose();
    session.dispose();
    await store.close();
  });

  Future<ManagedTransferAttempt> prepare({
    ManagedStageIntent? request,
    CancellationToken? token,
  }) {
    final candidate = request ?? intent();
    return repository.prepareTransfer(
      candidate,
      provenance(item: candidate.item.item, size: candidate.expectedSize),
      'approved-source-v1',
      token ?? CancellationToken(),
      jobs: jobs,
      checkAdmission: () {},
    );
  }

  Future<ManagedTransferLease> receiving() async {
    final prepared = await prepare();
    return repository.begin(
      prepared.lease,
      prepared.job.intentId!,
      CancellationToken(),
      checkAuthority: () {},
    );
  }

  test(
    'incoming inventory and exact queue job commit together before any effect',
    () async {
      store.failCommit = true;
      await expectLater(prepare(), failure(FailureKind.storage));
      expect(repository.readCurrent(), isEmpty);
      expect(jobs.read(scope), isEmpty);
      store.failCommit = false;
      final admitted = await prepare();
      expect(admitted.lease.record.state, ManagedTransferState.prepared);
      expect(admitted.job.kind, 'managedContentTransfer');
      expect(admitted.job.intentId, admitted.lease.record.attemptId);
      expect(admitted.lease.record.request.directoryIdentity, isEmpty);
      expect(jobs.read(scope), hasLength(1));
      final repeated = await prepare();
      expect(repeated.lease.record.revision, 1);
      expect(jobs.read(scope), hasLength(1));
    },
  );

  test('admission rejects local scope and URL-like source keys', () async {
    await expectLater(
      repository.prepareTransfer(
        intent(),
        provenance(),
        'https://example.invalid/token',
        CancellationToken(),
        jobs: jobs,
        checkAdmission: () {},
      ),
      failure(FailureKind.invalidData),
    );
    session.signOut();
    await expectLater(prepare(), failure(FailureKind.denied));
    expect(repository.readCurrent(), isEmpty);
  });

  test(
    'CAS denies duplicate begin, stale observations and forged native identity',
    () async {
      final admitted = await prepare();
      var active = await repository.begin(
        admitted.lease,
        admitted.job.intentId!,
        CancellationToken(),
        checkAuthority: () {},
      );
      await expectLater(
        repository.begin(
          admitted.lease,
          admitted.job.intentId!,
          CancellationToken(),
          checkAuthority: () {},
        ),
        failure(FailureKind.conflict),
      );
      active = await repository.observe(
        active,
        observation(active),
        CancellationToken(),
      );
      await expectLater(
        repository.observe(
          admitted.lease,
          observation(active),
          CancellationToken(),
        ),
        failure(FailureKind.conflict),
      );
      await expectLater(
        repository.observe(
          active,
          observation(active, sourceIdentity: '${hex(7, 16)}:${hex(11, 32)}'),
          CancellationToken(),
        ),
        failure(FailureKind.conflict),
      );
      expect(
        repository
            .findCurrent(active.record.operationId)!
            .request
            .observedBytes,
        10,
      );
    },
  );

  test('writing directory can be persisted before transport and replayed prefix is unverified', () async {
    var active = await receiving();
    active = await repository.observe(
      active,
      observation(active, bytes: 0, writing: true),
      CancellationToken(),
    );
    expect(active.record.request.directoryIdentity, directory);
    expect(active.record.request.sourceIdentity, source);
    active = await repository.observe(
      active,
      observation(active, bytes: 20, writing: true),
      CancellationToken(),
    );
    await expectLater(
      repository.observe(
        active,
        active.record.request,
        CancellationToken(),
        complete: true,
      ),
      failure(FailureKind.recovery),
    );
    expect(active.record.state, ManagedTransferState.receiving);
  });

  test('postcommit account replacement cancels exact admission without losing reservation', () async {
    store.afterCommit = () => session.replace(
      const AccountScope.verified('fixture-authority', 'Account-B'),
    );
    await expectLater(prepare(), failure(FailureKind.cancelled));
    expect(repository.readCurrent(), isEmpty);
    expect(jobs.read(scope).single.state, JobState.cancelled);
    session.replace(scope);
    expect(
      repository.readCurrent().single.state,
      ManagedTransferState.recoveryRequired,
    );
  });

  test(
    'safety recovery settles original account but cannot publish success there',
    () async {
      final active = await receiving();
      session.replace(
        const AccountScope.verified('fixture-authority', 'Account-B'),
      );
      await expectLater(
        repository.observe(
          active,
          observation(active, bytes: 100),
          CancellationToken(),
          complete: true,
        ),
        failure(FailureKind.cancelled),
      );
      final recovered = await repository.requireRecovery(
        active,
        observation: observation(active),
      );
      expect(recovered.record.scope.key, scope.key);
      expect(recovered.record.state, ManagedTransferState.recoveryRequired);
      expect(repository.readCurrent(), isEmpty);
    },
  );

  test(
    'slots and exact items are reserved across account boundaries',
    () async {
      await prepare();
      session.replace(
        const AccountScope.verified('fixture-authority', 'Account-B'),
      );
      await expectLater(
        prepare(request: intent(item: 'Other', id: 2)),
        failure(FailureKind.conflict),
      );
      await expectLater(
        prepare(request: intent(id: 2, slot: 1)),
        failure(FailureKind.conflict),
      );
      await prepare(request: intent(item: 'game', id: 2, slot: 1));
      expect(repository.readCurrent(), hasLength(1));
    },
  );

  test(
    'incoming reserves both retry copies plus staging within eight GiB',
    () async {
      await prepare(request: intent(size: managedStageMaximumBytes));
      await prepare(
        request: intent(
          item: 'Two',
          id: 2,
          slot: 1,
          size: managedStageMaximumBytes,
        ),
      );
      await expectLater(
        prepare(
          request: intent(
            item: 'Three',
            id: 3,
            slot: 2,
            size: managedStageMaximumBytes,
          ),
        ),
        failure(FailureKind.conflict),
      );
      expect(repository.readCurrent(), hasLength(2));
      expect(jobs.read(scope), hasLength(2));
    },
  );

  test('fresh retry retains exact old candidate and refuses an unremoved third copy', () async {
    var active = await receiving();
    active = await repository.requireRecovery(
      active,
      observation: observation(active),
    );
    final retry = await repository.prepareRetry(
      active,
      active.record.request,
      CancellationToken(),
      jobs: jobs,
      checkAdmission: () {},
      strongEtag: '"version-1"',
    );
    expect(retry.lease.record.attempt, 1);
    expect(retry.lease.record.request.attempt, 1);
    expect(retry.lease.record.previous!.attempt, 0);
    expect(
      retry.lease.record.stagingIntent.sourceRelative.endsWith('-1.bin'),
      isTrue,
    );
    expect(retry.lease.record.request.sourceIdentity, isEmpty);
    expect(retry.job.checkpoint, 0);
    active = await repository.begin(
      retry.lease,
      retry.job.intentId!,
      CancellationToken(),
      checkAuthority: () {},
    );
    active = await repository.requireRecovery(
      active,
      observation: observation(
        active,
        sourceIdentity: '${hex(7, 16)}:${hex(11, 32)}',
      ),
    );
    await expectLater(
      repository.prepareRetry(
        active,
        active.record.request,
        CancellationToken(),
        jobs: jobs,
        checkAdmission: () {},
      ),
      failure(FailureKind.recovery),
    );
    await expectLater(
      repository.clearPrevious(
        active,
        discarded(active.record.previous!),
        CancellationToken(),
      ),
      failure(FailureKind.conflict),
    );
    final discard = await repository.prepareDiscard(
      active,
      CancellationToken(),
      jobs: jobs,
      checkAdmission: () {},
    );
    active = await repository.beginDiscard(discard.lease, CancellationToken());
    active = await repository.clearPrevious(
      active,
      discarded(active.record.previous!),
      CancellationToken(),
    );
    active = await repository.reconcileDiscard(
      active,
      discarded(active.record.request),
      CancellationToken(),
    );
    expect(active.record.previous, isNull);
    expect(active.record.state, ManagedTransferState.discarded);
  });

  test(
    'fresh absence allows zero-byte retry or release without adopting a file',
    () async {
      final original = await prepare();
      var recovered = await repository.requireRecovery(original.lease);
      final retry = await repository.prepareRetry(
        recovered,
        recovered.record.request,
        CancellationToken(),
        jobs: jobs,
        checkAdmission: () {},
        strongEtag: '"obsolete"',
      );
      expect(retry.lease.record.previous, isNull);
      expect(retry.lease.record.strongEtag, isNull);
      expect(retry.lease.record.request.directoryIdentity, isEmpty);
      recovered = await repository.requireRecovery(retry.lease);
      final discard = await repository.prepareDiscard(
        recovered,
        CancellationToken(),
        jobs: jobs,
        checkAdmission: () {},
      );
      final active = await repository.beginDiscard(
        discard.lease,
        CancellationToken(),
      );
      final released = await repository.reconcileDiscard(
        active,
        discarded(active.record.request),
        CancellationToken(),
      );
      expect(released.record.reservesContent, isFalse);
      await prepare(request: intent(item: 'Other', id: 2));
    },
  );

  test('weak entity tags and incomplete success fail closed', () async {
    final active = await receiving();
    await expectLater(
      repository.observe(
        active,
        observation(active),
        CancellationToken(),
        strongEtag: 'W/"weak"',
      ),
      failure(FailureKind.invalidData),
    );
    await expectLater(
      repository.observe(
        active,
        observation(active),
        CancellationToken(),
        complete: true,
      ),
      failure(FailureKind.recovery),
    );
    final complete = await repository.observe(
      active,
      observation(active, bytes: 100),
      CancellationToken(),
      complete: true,
    );
    expect(complete.record.state, ManagedTransferState.downloaded);
  });

  test('source keys and strong tags consume their entire bounded input', () {
    for (final invalid in ['source\n', 'source\r\n', 'source\u0000']) {
      expect(
        () => validateManagedTransferSourceKey(invalid),
        failure(FailureKind.invalidData),
      );
    }
    for (final invalid in ['"tag"\n', '"tag"\r\n', '"${'x' * 255}"']) {
      expect(
        () => validateManagedTransferEtag(invalid),
        failure(FailureKind.invalidData),
      );
    }
    expect(
      () => validateManagedTransferEtag('"${'x' * 254}"'),
      returnsNormally,
    );
  });

  test(
    'a reused empty native slot persists only read-only ownership observations',
    () async {
      final candidate = intent();
      final request = ContentTransferRequest(
        slot: candidate.slot,
        operationId: candidate.operationId,
        expectedSize: candidate.expectedSize,
        expectedSha256: candidate.expectedSha256,
        rootIdentity: root,
        directoryIdentity: directory,
      );
      final result = await repository.prepareTransfer(
        candidate,
        provenance(),
        'source-v1',
        CancellationToken(),
        jobs: jobs,
        checkAdmission: () {},
        initialObservation: request,
      );
      expect(result.lease.record.request.directoryIdentity, directory);
      expect(result.lease.record.request.sourceIdentity, isEmpty);
      expect(result.lease.record.state, ManagedTransferState.prepared);
    },
  );

  test(
    'completed recovery observation settles its original job atomically',
    () async {
      var active = await receiving();
      final job = jobs.read(scope).single;
      await jobs.transition(scope, job, JobState.running);
      await jobs.transition(scope, job, JobState.recoveryRequired);
      active = await repository.requireRecovery(active);
      store.failCommit = true;
      await expectLater(
        repository.observe(
          active,
          observation(active, bytes: 100),
          CancellationToken(),
          complete: true,
          jobs: jobs,
        ),
        failure(FailureKind.storage),
      );
      expect(
        repository.findCurrent(active.record.operationId)!.state,
        ManagedTransferState.recoveryRequired,
      );
      expect(jobs.read(scope).single.state, JobState.recoveryRequired);
      store.failCommit = false;
      final result = await repository.observe(
        active,
        observation(active, bytes: 100),
        CancellationToken(),
        complete: true,
        jobs: jobs,
      );
      expect(result.record.state, ManagedTransferState.downloaded);
      expect(jobs.read(scope).single.state, JobState.completed);
      expect(jobs.read(scope).single.key, job.key);
      expect(jobs.read(scope).single.checkpoint, 100);
      expect(staging.readCurrent(), isEmpty);
    },
  );

  test('read-only completed inspection does not grant stage without fresh authorized admission', () async {
    final active = await receiving();
    final downloaded = await repository.observe(
      active,
      observation(active, bytes: 100),
      CancellationToken(),
      complete: true,
    );
    final transferJob = jobs.read(scope).single;
    await jobs.transition(scope, transferJob, JobState.running);
    await jobs.transition(
      scope,
      transferJob,
      JobState.completed,
      checkpoint: 100,
    );
    for (final revision in [null, downloaded.record.revision - 1]) {
      await expectLater(
        staging.prepareStage(
          downloaded.record.stagingIntent,
          ManagedStageObservation(
            operationId: active.record.operationId,
            rootIdentity: root,
            sourceIdentity: source,
            state: ManagedStageObservationState.checkpoint,
            verifiedBytes: 0,
            verifiedChunks: 0,
          ),
          CancellationToken(),
          jobs: jobs,
          checkAdmission: () {},
          manifest: provenance(),
          transferRevision: revision,
        ),
        failure(FailureKind.conflict),
      );
      expect(staging.readCurrent(), isEmpty);
      expect(jobs.read(scope), hasLength(1));
      expect(
        repository.findCurrent(downloaded.record.operationId)!.state,
        ManagedTransferState.downloaded,
      );
    }
    // The staging service obtains a fresh grant, so timestamps may differ.
    final stage = await staging.prepareStage(
      downloaded.record.stagingIntent,
      ManagedStageObservation(
        operationId: active.record.operationId,
        rootIdentity: root,
        sourceIdentity: source,
        state: ManagedStageObservationState.checkpoint,
        verifiedBytes: 0,
        verifiedChunks: 0,
      ),
      CancellationToken(),
      jobs: jobs,
      checkAdmission: () {},
      manifest: provenance(observed: DateTime.utc(2026, 10, 12, 0, 1)),
      transferRevision: downloaded.record.revision,
    );
    expect(stage.lease.record.state, ManagedInstallationState.prepared);
    final marked = repository.findCurrent(downloaded.record.operationId)!;
    expect(marked.state, ManagedTransferState.stagingAdmitted);
    expect(marked.revision, downloaded.record.revision + 1);
  });

  test('unrelated local stage cannot enter a reserved incoming slot', () async {
    await prepare();
    final other = intent(item: 'Other', id: 2);
    await expectLater(
      staging.prepare(
        other,
        ManagedStageObservation(
          operationId: other.operationId,
          state: ManagedStageObservationState.checkpoint,
          rootIdentity: root,
          sourceIdentity: source,
          verifiedBytes: 0,
          verifiedChunks: 0,
        ),
        CancellationToken(),
      ),
      throwsA(isA<AppFailure>()),
    );
    expect(staging.readCurrent(), isEmpty);
  });

  test('discard admission and completion retain job lineage and require exact receipt', () async {
    var active = await receiving();
    active = await repository.requireRecovery(
      active,
      observation: observation(active),
    );
    final attempt = await repository.prepareDiscard(
      active,
      CancellationToken(),
      jobs: jobs,
      checkAdmission: () {},
    );
    active = await repository.beginDiscard(
      attempt.lease,
      CancellationToken(),
      attemptId: attempt.job.intentId,
    );
    final wrong = ContentTransferReceipt(
      state: ContentTransferState.discarded,
      rootIdentity: root,
      directoryIdentity: directory,
      sourceIdentity: source,
      sourceRelative: 'another.bin',
      observedBytes: 10,
      observedSha256: 'd' * 64,
    );
    await expectLater(
      repository.reconcileDiscard(active, wrong, CancellationToken()),
      failure(FailureKind.recovery),
    );
    final done = await repository.reconcileDiscard(
      active,
      discarded(active.record.request),
      CancellationToken(),
    );
    expect(done.record.state, ManagedTransferState.discarded);
    expect(done.record.reservesContent, isFalse);
    await prepare(request: intent(item: 'Other', id: 2));
  });

  test('restart downgrades interrupted work without replay and preserves unknown fields', () async {
    final active = await receiving();
    await store.transact((draft) {
      final envelope = draft['managedTransfers'] as Map;
      envelope['futureEnvelope'] = {'retain': true};
      final record =
          (envelope['records'] as Map)[active.record.operationId] as Map;
      record['futureRecord'] = ['exact', 7];
      (record['request'] as Map)['futureRequest'] = true;
    });
    expect(await repository.recoverInterrupted(), 1);
    expect(await repository.recoverInterrupted(), 0);
    final read = repository.readCurrent().single;
    expect(read.state, ManagedTransferState.recoveryRequired);
    final lease = repository.leaseCurrent(read.operationId)!;
    await repository.observe(lease, observation(lease), CancellationToken());
    final envelope = store.read()['managedTransfers'] as Map;
    final record = (envelope['records'] as Map)[read.operationId] as Map;
    expect(envelope['futureEnvelope'], {'retain': true});
    expect(record['futureRecord'], ['exact', 7]);
    expect((record['request'] as Map)['futureRequest'], isTrue);
  });

  test(
    'future optional inventory is preserved and blocks only managed admission',
    () async {
      await store.transact((draft) {
        draft['managedTransfers'] = {
          'version': 2,
          'opaque': ['retain'],
        };
      });
      await store.transact((draft) {
        (draft['preferences'] as Map)['reduceMotion'] = true;
      });
      await expectLater(prepare(), failure(FailureKind.unsupported));
      final candidate = intent();
      await expectLater(
        staging.prepare(
          candidate,
          ManagedStageObservation(
            operationId: candidate.operationId,
            state: ManagedStageObservationState.checkpoint,
            rootIdentity: root,
            sourceIdentity: source,
            verifiedBytes: 0,
            verifiedChunks: 0,
          ),
          CancellationToken(),
        ),
        failure(FailureKind.unsupported),
      );
      expect(store.read()['managedTransfers'], {
        'version': 2,
        'opaque': ['retain'],
      });
      expect((store.read()['preferences'] as Map)['reduceMotion'], isTrue);
    },
  );

  test('a future staging inventory appearing after admission prevents transfer effects', () async {
    final prepared = await prepare();
    await store.transact((draft) {
      draft['managedInstallations'] = {'version': 99, 'opaque': 'retain'};
    });
    await expectLater(
      repository.begin(
        prepared.lease,
        prepared.job.intentId!,
        CancellationToken(),
        checkAuthority: () {},
      ),
      failure(FailureKind.unsupported),
    );
    final recovered = await repository.requireRecovery(prepared.lease);
    expect(recovered.record.state, ManagedTransferState.recoveryRequired);
    expect(store.read()['managedInstallations'], {
      'version': 99,
      'opaque': 'retain',
    });
  });

  test(
    'admission headroom is consumed by observations instead of charged twice',
    () async {
      final prepared = await prepare();
      final envelope = Map<String, Object?>.from(
        store.read()['managedTransfers'] as Map,
      );
      var low = 0, high = 1024 * 1024;
      while (low < high) {
        final mid = (low + high + 1) ~/ 2;
        envelope['retainedUnknown'] = 'x' * mid;
        try {
          validateManagedTransfers(envelope);
          low = mid;
        } on AppFailure catch (failure) {
          expect({
            FailureKind.conflict,
            FailureKind.invalidData,
          }, contains(failure.kind));
          high = mid - 1;
        }
      }
      envelope['retainedUnknown'] = 'x' * low;
      await store.transact((draft) {
        draft['managedTransfers'] = envelope;
      });
      var lease = await repository.begin(
        prepared.lease,
        prepared.job.intentId!,
        CancellationToken(),
        checkAuthority: () {},
      );
      lease = await repository.observe(
        lease,
        observation(lease, bytes: 0, writing: true),
        CancellationToken(),
        strongEtag: '"${'\\' * 254}"',
      );
      lease = await repository.observe(
        lease,
        observation(lease, bytes: 100),
        CancellationToken(),
        complete: true,
      );
      expect(lease.record.state, ManagedTransferState.downloaded);
      expect(
        (store.read()['managedTransfers'] as Map)['retainedUnknown'],
        'x' * low,
      );
    },
  );
}
