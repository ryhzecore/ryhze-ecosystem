// These synthetic adapters establish no production authority.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/content_discard_backend.dart';
import 'package:ryhze/features/installations/managed_content_admission.dart';
import 'package:ryhze/features/installations/managed_content_manifest.dart';
import 'package:ryhze/features/installations/managed_content_service.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_installation_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

final initialTime = DateTime.utc(2026, 10, 11);
const scope = AccountScope.verified('fixture-account-issuer', 'ExactAccount');
Map<String, Object?> file(
  String path, {
  int size = 100,
  bool executable = false,
}) => {
  'path': path,
  'size': size,
  'sha256': 'c' * 64,
  'executable': executable,
};
Uint8List manifest(
  String item, {
  int size = 100,
  List<Map<String, Object?>>? files,
}) => Uint8List.fromList(
  utf8.encode(
    jsonEncode(<String, Object?>{
      'version': 1,
      'provider': 'fixture',
      'item': item,
      'namespace': 'ExactNamespace',
      'packageId': 'fixture-package',
      'packageVersion': '1',
      'payloadSize': size,
      'payloadSha256': 'b' * 64,
      'files': files ?? [file('bin/game.exe', size: size, executable: true)],
    }),
  ),
);
ManagedStageIntent intent({
  String item = 'Game-A',
  int slot = 0,
  int size = 100,
}) => ManagedStageIntent(
  operationId: (slot == 0 ? 'a' : 'd') * 64,
  item: ProviderItemRef('fixture', item, namespace: 'ExactNamespace'),
  packageId: 'fixture-package',
  version: '1',
  manifestSha256: crypto.sha256.convert(manifest(item, size: size)).toString(),
  sourceRelative: 'fixture.bin',
  expectedSize: size,
  expectedSha256: 'b' * 64,
  slot: slot,
);
Matcher failure(FailureKind kind) =>
    isA<AppFailure>().having((e) => e.kind, 'kind', kind);

class ReentrantIntent extends ManagedStageIntent {
  ReentrantIntent(ManagedStageIntent original, this.onCompare)
    : super(
        operationId: original.operationId,
        item: original.item,
        packageId: original.packageId,
        version: original.version,
        manifestSha256: original.manifestSha256,
        sourceRelative: original.sourceRelative,
        expectedSize: original.expectedSize,
        expectedSha256: original.expectedSha256,
        slot: original.slot,
      );
  final void Function() onCompare;
  @override
  bool sameRequest(ManagedStageIntent other) {
    onCompare();
    return super.sameRequest(other);
  }
}

class Store extends MemoryTransactionStore {
  @override
  bool get durable => true;
  int transactions = 0;
  int? gateAt;
  Completer<void>? gate;
  final entered = Completer<void>();
  void Function()? afterCommit;
  @override
  Future<void> transact(void Function(StoreData) change) async {
    transactions++;
    if (transactions == gateAt) {
      if (!entered.isCompleted) entered.complete();
      await gate!.future;
    }
    await super.transact(change);
    final callback = afterCommit;
    afterCommit = null;
    callback?.call();
  }
}

class Backend implements ContentStagingBackend {
  @override
  final deviceId = '${'1' * 16}:${'2' * 32}';
  final sourceId = '${'1' * 16}:${'3' * 32}';
  int stages = 0, inspections = 0, cancellations = 0, closes = 0;
  Object? closeFailure, drainFailure;
  bool copied = false, completeCopy = true;
  Completer<void>? stageGate;
  final stageEntered = Completer<void>();
  CancellationToken? stageCancellation;
  ContentStagingReceipt receipt(
    ContentStagingRequest request,
  ) => ContentStagingReceipt(
    state: copied ? ContentStagingState.staged : ContentStagingState.checkpoint,
    verifiedBytes: copied ? request.size : 0,
    verifiedChunks: copied
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
    cancellation.check();
    return receipt(request);
  }

  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    stages++;
    stageCancellation = cancellation;
    if (!stageEntered.isCompleted) stageEntered.complete();
    await stageGate?.future;
    cancellation.check();
    copied = completeCopy;
    return receipt(request);
  }

  @override
  Future<void> drain() async {
    await stageGate?.future;
    final failure = drainFailure;
    if (failure != null) throw failure;
  }

  @override
  Future<void> cancel() async {
    cancellations++;
  }

  @override
  Future<void> close() async {
    closes++;
    await drain();
    final failure = closeFailure;
    if (failure != null) throw failure;
  }
}

class Authority implements ManagedContentAuthority {
  Authority(this.now);
  final DateTime Function() now;
  @override
  bool available = true;
  @override
  String authority = 'fixture-content-issuer';
  @override
  String policy = 'fixture-policy-1';
  int _generation = 1;
  void Function()? onReadGeneration;
  @override
  int get generation {
    onReadGeneration?.call();
    return _generation;
  }

  final listeners = <void Function()>[];
  int requests = 0;
  bool fixture = false;
  AccessState state = AccessState.verified;
  AccountScope? wrongScope;
  String sourceHash = 'f' * 64;
  Duration grantAge = Duration.zero;
  Uint8List? replacementBytes;
  Completer<void>? readGate;
  final readEntered = Completer<void>();
  CancellationToken? readCancellation;
  void Function()? onAuthorize;
  @override
  void addInvalidationListener(void Function() listener) =>
      listeners.add(listener);
  @override
  void removeInvalidationListener(void Function() listener) =>
      listeners.remove(listener);
  void revoke() {
    available = false;
    _generation++;
    for (final listener in List<void Function()>.of(listeners)) {
      listener();
    }
  }

  @override
  Future<ManagedContentGrant> authorize(
    ManagedContentAccessRequest request,
    CancellationToken cancellation,
  ) async {
    requests++;
    cancellation.check();
    readCancellation = cancellation;
    if (!readEntered.isCompleted) readEntered.complete();
    onAuthorize?.call();
    await readGate?.future;
    cancellation.check();
    return ManagedContentGrant(
      scope: wrongScope ?? request.scope,
      sessionGeneration: request.sessionGeneration,
      deviceId: request.deviceId,
      provenance: ManagedManifestProvenance.create(
        authority: authority,
        policy: policy,
        sourceManifestSha256: sourceHash,
        normalizedBytes:
            replacementBytes ??
            manifest(
              request.intent.item.item,
              size: request.intent.expectedSize,
            ),
        observedAt: now().subtract(grantAge),
        expiresAt: now().add(const Duration(seconds: 30)),
      ),
      state: state,
      fixture: fixture,
    );
  }
}

class TrackingSession extends SessionController {
  final active = <CancellationToken>{};
  @override
  CancellationToken beginWork() {
    final token = super.beginWork();
    active.add(token);
    return token;
  }

  @override
  void finishWork(CancellationToken token) {
    active.remove(token);
    super.finishWork(token);
  }
}

class Fixture {
  Fixture({bool unavailable = false}) {
    session.replace(scope);
    authority = Authority(() => now);
    jobs = JobRepository(store);
    inventory = ManagedInstallationRepository(
      store,
      session,
      deviceId: backend.deviceId,
    );
    service = ManagedContentService(
      inventory,
      backend,
      jobs,
      session,
      authority: unavailable
          ? const UnavailableManagedContentAuthority()
          : authority,
      now: () {
        onNow?.call();
        return now;
      },
    );
    coordinator = JobCoordinator(jobs, {
      ManagedContentService.jobKind: service,
    });
    session.addListener(coordinator.cancelActive);
  }
  DateTime now = initialTime;
  void Function()? onNow;
  final store = Store();
  final backend = Backend();
  final session = TrackingSession();
  Object? expectedCloseFailure;
  late final Authority authority;
  late final JobRepository jobs;
  late final ManagedInstallationRepository inventory;
  late final ManagedContentService service;
  late final JobCoordinator coordinator;
  Future<JobRecord> prepare({ManagedStageIntent? request}) =>
      service.prepareAuthorizedLocalStage(
        request ?? intent(),
        CancellationToken(),
        coordinator: coordinator,
      );
  Map get envelope => store.read()['managedInstallations'] as Map;
  Future<void> close() async {
    try {
      service.cancelPending();
      coordinator.cancelActive();
    } catch (error) {
      if (!identical(error, expectedCloseFailure)) rethrow;
    } finally {
      for (final gate in [store.gate, backend.stageGate, authority.readGate]) {
        if (gate != null && !gate.isCompleted) gate.complete();
      }
    }
    await coordinator.close();
    try {
      await service.close();
    } catch (error) {
      if (!identical(error, expectedCloseFailure)) rethrow;
    }
    inventory.dispose();
    jobs.dispose();
    session.dispose();
    await store.close();
  }
}

void main() {
  test(
    'default refuses authority before inspection, inventory or queue effects',
    () async {
      final f = Fixture(unavailable: true);
      addTearDown(f.close);
      await expectLater(f.prepare(), throwsA(failure(FailureKind.unsupported)));
      expect(f.backend.inspections, 0);
      expect(f.backend.stages, 0);
      expect(f.store.read()['managedInstallations'], isNull);
      expect(f.jobs.read(f.session.scope), isEmpty);
    },
  );
  for (final rejected in [
    'fixture',
    'revoked',
    'wrong-account',
    'manifest-binding',
  ]) {
    test(
      'rejects $rejected before native inspection or durable admission',
      () async {
        final f = Fixture();
        addTearDown(f.close);
        switch (rejected) {
          case 'fixture':
            f.authority.fixture = true;
          case 'revoked':
            f.authority.state = AccessState.revoked;
          case 'wrong-account':
            f.authority.wrongScope = const AccountScope.verified(
              'fixture-account-issuer',
              'other',
            );
          case 'manifest-binding':
            f.authority.replacementBytes = manifest('Other');
        }
        await expectLater(f.prepare(), throwsA(failure(FailureKind.denied)));
        expect(f.backend.inspections, 0);
        expect(f.store.transactions, 0);
      },
    );
  }
  test(
    'provenance, inventory and exact job share one durable initial commit',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      final job = await f.prepare();
      expect(f.store.transactions, 1);
      expect(f.envelope['version'], 2);
      final record = f.inventory.readCurrent().single;
      expect(record.manifest!.sourceManifestSha256, 'f' * 64);
      expect(record.manifest!.manifest.files.single.path, 'bin/game.exe');
      expect(record.state, ManagedInstallationState.prepared);
      expect(f.jobs.read(f.session.scope).single.key, job.key);
      expect(f.backend.stages, 0);
      final original = f.envelope;
      f.now = f.now.add(const Duration(seconds: 1));
      expect((await f.prepare()).key, job.key);
      expect(f.envelope, original); // Fresh grant observation is not rewritten.
    },
  );
  test('same operation cannot change source provenance or switch to inert admission', () async {
    final f = Fixture();
    addTearDown(f.close);
    await f.prepare();
    f.authority.sourceHash = 'e' * 64;
    await expectLater(f.prepare(), throwsA(failure(FailureKind.conflict)));
    await expectLater(
      f.service.prepareLocalStage(
        intent(),
        CancellationToken(),
        coordinator: f.coordinator,
      ),
      throwsA(failure(FailureKind.conflict)),
    );
    expect(
      f.inventory.readCurrent().single.manifest!.sourceManifestSha256,
      'f' * 64,
    );
  });
  test('global cleanup reserve rejects another record atomically', () async {
    final f = Fixture();
    addTearDown(f.close);
    await f.prepare();
    final previous = f.store.read();
    await expectLater(
      f.prepare(request: intent(item: 'Game-B', slot: 1)),
      throwsA(failure(FailureKind.conflict)),
    );
    expect(f.store.read(), previous);
    expect(f.backend.stages, 0);
  });
  test('accepted capacity boundary spends its reserve through recovery, retry and max-plan cleanup', () async {
    final f = Fixture();
    addTearDown(f.close);
    final request = intent(size: managedStageMaximumBytes);
    await f.prepare(request: request);
    final candidate = jsonDecode(jsonEncode(f.envelope)) as Map;
    final raw = (candidate['records'] as Map).values.single as Map;
    raw['revision'] = 999999999; // The first mutation crosses to ten digits.
    raw['updatedAt'] = '0000-01-01T00:00:00Z';
    expect(raw.containsKey('attempt'), isFalse);
    expect(raw.containsKey('resumeFloorBytes'), isFalse);
    // Discover the public validator boundary independently of its reserve math.
    var lower = 0, upper = 512 * 1024;
    while (lower < upper) {
      final middle = (lower + upper + 1) ~/ 2;
      candidate['boundaryPadding'] = 'p' * middle;
      try {
        validateManagedInstallations(candidate);
        lower = middle;
      } on AppFailure catch (error) {
        expect(error.kind, FailureKind.conflict);
        upper = middle - 1;
      }
    }
    candidate['boundaryPadding'] = 'p' * lower;
    await f.store.transact((data) {
      data['managedInstallations'] = candidate;
    });
    final admitted = f.store.read();
    candidate['boundaryPadding'] = 'p' * (lower + 1);
    await expectLater(
      f.store.transact((data) {
        data['managedInstallations'] = candidate;
      }),
      throwsA(failure(FailureKind.invalidData)),
    );
    expect(f.store.read(), admitted);
    f.backend.completeCopy = false;
    await f.coordinator.drain(f.session.scope);
    expect(
      f.inventory.readCurrent().single.state,
      ManagedInstallationState.recoveryRequired,
    );
    await f.service.inspectRecord(request.operationId, CancellationToken());
    f.backend.completeCopy = true;
    await f.service.resumeRecord(
      request.operationId,
      CancellationToken(),
      coordinator: f.coordinator,
    );
    var lease = f.inventory.leaseCurrent(request.operationId)!;
    expect(lease.record.verifiedBytes, managedStageMaximumBytes);
    expect(lease.record.verifiedChunks, 4096);
    expect(lease.record.attempt, 1);
    final plan = Uint8List(contentDiscardMaximumPlanBytes);
    final first = await f.inventory.prepareDiscard(
      lease,
      plan,
      managedStageMaximumBytes,
      4096,
      CancellationToken(),
      jobs: f.jobs,
      checkAdmission: () {},
    );
    await f.inventory.abortDiscard(first, jobs: f.jobs);
    // Exercise the last allowed retry counter and maximum removal counters.
    await f.store.transact((data) {
      final record =
          ((data['managedInstallations'] as Map)['records'] as Map)
                  .values
                  .single
              as Map;
      (record['discard'] as Map)['attempt'] = 998;
    });
    lease = f.inventory.leaseCurrent(request.operationId)!;
    final retried = await f.inventory.prepareDiscard(
      lease,
      plan,
      managedStageMaximumBytes,
      4096,
      CancellationToken(),
      jobs: f.jobs,
      checkAdmission: () {},
    );
    expect(retried.lease.record.discard!.attempt, 999);
    lease = await f.inventory.beginDiscard(
      retried.lease,
      retried.job.intentId!,
      CancellationToken(),
    );
    await f.inventory.reconcileDiscard(
      lease,
      const ContentDiscardReceipt(
        state: ContentDiscardState.completed,
        removedBytes: managedStageMaximumBytes,
        removedFiles: 4097,
      ),
      CancellationToken(),
    );
    expect(
      f.inventory.readCurrent().single.state,
      ManagedInstallationState.discarded,
    );
    expect(f.envelope['boundaryPadding'], 'p' * lower);
    expect(
      utf8.encode(jsonEncode(f.envelope)).length,
      lessThanOrEqualTo(1024 * 1024),
    );
    // No payload is copied/deleted here: backend and opaque plan are synthetic.
  });
  test(
    'legacy unknown reserved names refuse promotion without reinterpretation',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      await f.service.prepareLocalStage(
        intent(),
        CancellationToken(),
        coordinator: f.coordinator,
      );
      await f.store.transact((data) {
        final record =
            ((data['managedInstallations'] as Map)['records'] as Map)
                    .values
                    .single
                as Map;
        record['recordVersion'] = 2;
        record['managedManifest'] = {'unknown': 'preserved'};
      });
      final previous = f.store.read();
      expect(f.inventory.readCurrent().single.manifest, isNull);
      await expectLater(
        f.prepare(request: intent(item: 'Game-B', slot: 1)),
        throwsA(failure(FailureKind.unsupported)),
      );
      expect(f.store.read(), previous);
      expect(f.envelope['version'], 1);
    },
  );
  test(
    'unknown future record stays preserved and disables managed capability',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      await f.prepare();
      await f.store.transact((data) {
        final record =
            ((data['managedInstallations'] as Map)['records'] as Map)
                    .values
                    .single
                as Map;
        record['recordVersion'] = 3;
        record['future'] = 'preserved';
      });
      expect(supportsManagedInstallations(f.envelope), isFalse);
      expect(
        () => f.inventory.readCurrent(),
        throwsA(failure(FailureKind.unsupported)),
      );
      expect(
        (f.envelope['records'] as Map).values.single['future'],
        'preserved',
      );
    },
  );
  for (final corruption in [
    'missing-marker',
    'missing-manifest',
    'null-marker',
    'null-manifest',
    'noninteger-marker',
    'noninteger-provenance',
  ]) {
    test('v2 $corruption never falls back to inert staging', () async {
      final f = Fixture();
      addTearDown(f.close);
      await f.prepare();
      await f.store.transact((data) {
        final record =
            ((data['managedInstallations'] as Map)['records'] as Map)
                    .values
                    .single
                as Map;
        switch (corruption) {
          case 'missing-marker':
            record.remove('recordVersion');
          case 'missing-manifest':
            record.remove('managedManifest');
          case 'null-marker':
            record['recordVersion'] = null;
          case 'null-manifest':
            record['managedManifest'] = null;
          case 'noninteger-marker':
            record['recordVersion'] = 2.0;
          case 'noninteger-provenance':
            (record['managedManifest'] as Map)['version'] = 1.0;
        }
      });
      final previous = f.store.read();
      expect(supportsManagedInstallations(f.envelope), isFalse);
      expect(
        () => f.inventory.readCurrent(),
        throwsA(failure(FailureKind.unsupported)),
      );
      expect(f.store.read(), previous);
      expect(f.backend.stages, 0);
    });
  }
  test('same-revision provenance mutation invalidates an already acquired CAS lease', () async {
    final f = Fixture();
    addTearDown(f.close);
    await f.prepare();
    final lease = f.inventory.leaseCurrent(intent().operationId)!;
    await f.store.transact((data) {
      final record =
          ((data['managedInstallations'] as Map)['records'] as Map)
                  .values
                  .single
              as Map;
      (record['managedManifest'] as Map)['extension'] = 'changed';
    });
    await expectLater(
      f.inventory.beginStage(
        lease,
        CancellationToken(),
        attemptId: lease.record.attemptId,
        checkAuthority: () {},
      ),
      throwsA(failure(FailureKind.conflict)),
    );
  });
  test(
    'postcommit revocation cancels exact queued job and recovers inventory',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      f.store.afterCommit = f.authority.revoke;
      await expectLater(f.prepare(), throwsA(failure(FailureKind.cancelled)));
      expect(
        f.inventory.readCurrent().single.state,
        ManagedInstallationState.recoveryRequired,
      );
      expect(f.jobs.read(f.session.scope).single.state, JobState.cancelled);
      expect(f.backend.stages, 0);
    },
  );
  test(
    'queued provenance cannot authorize execution after revocation',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      await f.prepare();
      f.authority.revoke();
      await f.coordinator.drain(f.session.scope);
      expect(f.backend.stages, 0);
      expect(
        f.inventory.readCurrent().single.state,
        ManagedInstallationState.recoveryRequired,
      );
      expect(
        f.jobs.read(f.session.scope).single.state,
        isNot(JobState.completed),
      );
    },
  );
  test(
    'revocation during delayed beginStage is checked inside the transaction',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      await f.prepare();
      f.store.gateAt = 3;
      f.store.gate = Completer<void>();
      final run = f.coordinator.drain(f.session.scope);
      await f.store.entered.future;
      f.authority.revoke();
      f.store.gate!.complete();
      await run;
      expect(f.backend.stages, 0);
      expect(
        f.inventory.readCurrent().single.state,
        ManagedInstallationState.recoveryRequired,
      );
    },
  );
  test(
    'account change during initial transaction wait leaves no admitted job',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      f.store.gateAt = 1;
      f.store.gate = Completer<void>();
      final prepared = expectLater(
        f.prepare(),
        throwsA(failure(FailureKind.cancelled)),
      );
      await f.store.entered.future;
      f.session.replace(
        const AccountScope.verified('fixture-account-issuer', 'other'),
      );
      f.store.gate!.complete();
      await prepared;
      expect(f.store.read()['managedInstallations'], isNull);
      expect(f.jobs.read(scope), isEmpty);
      expect(f.backend.stages, 0);
    },
  );
  test(
    'close cancels authority read and waits for its actual settlement',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      f.authority.readGate = Completer<void>();
      final prepared = expectLater(
        f.prepare(),
        throwsA(failure(FailureKind.cancelled)),
      );
      await f.authority.readEntered.future;
      var settled = false;
      final closing = f.service.close().then((_) => settled = true);
      await Future<void>.value();
      expect(f.authority.readCancellation!.isCancelled, isTrue);
      expect(settled, isFalse);
      expect(f.backend.inspections, 0);
      f.authority.readGate!.complete();
      await prepared;
      await closing;
      expect(settled, isTrue);
      expect(f.authority.listeners, isEmpty);
    },
  );
  for (final closeOwner in ['admission', 'content-service']) {
    test(
      'throwing read cancellation still settles $closeOwner and closes the backend',
      () async {
        final f = Fixture();
        addTearDown(f.close);
        final expected = StateError('synthetic authority cancellation failure');
        f.expectedCloseFailure = expected;
        f.authority.readGate = Completer<void>();
        f.authority.onAuthorize = () {
          f.authority.readCancellation!.onCancel(() => throw expected);
        };
        if (closeOwner == 'content-service') {
          f.backend.closeFailure = StateError('later backend close failure');
        }
        final prepared = expectLater(
          f.prepare(),
          throwsA(failure(FailureKind.cancelled)),
        );
        await f.authority.readEntered.future;
        var settled = false;
        final closing = closeOwner == 'admission'
            ? f.service.admissions.close()
            : f.service.close();
        final checkedClose = expectLater(
          closing,
          throwsA(same(expected)),
        ).whenComplete(() => settled = true);
        try {
          await Future<void>.delayed(Duration.zero);
          expect(f.authority.readCancellation!.isCancelled, isTrue);
          expect(settled, isFalse);
          expect(f.backend.closes, 0);
          expect(f.backend.inspections, 0);
        } finally {
          if (!f.authority.readGate!.isCompleted) {
            f.authority.readGate!.complete();
          }
        }
        await prepared;
        await checkedClose;
        expect(settled, isTrue);
        expect(f.authority.listeners, isEmpty);
        if (closeOwner == 'admission') {
          await expectLater(f.service.close(), throwsA(same(expected)));
        }
        expect(f.backend.closes, 1);
        expect(f.backend.cancellations, greaterThanOrEqualTo(1));
        expect(f.session.active, isEmpty);
      },
    );
  }
  test(
    'throwing live-ticket cancellation still releases admission ownership',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      final expected = StateError('synthetic ticket cancellation failure');
      f.expectedCloseFailure = expected;
      final ticket = await f.service.admissions.admit(
        intent(),
        CancellationToken(),
      );
      final guard = ticket.claim(
        intent: intent(),
        session: f.session,
        deviceId: f.backend.deviceId,
      );
      guard.cancellation.onCancel(() => throw expected);
      await expectLater(f.service.admissions.close(), throwsA(same(expected)));
      expect(f.session.active, isEmpty);
      expect(guard.check, throwsA(failure(FailureKind.cancelled)));
      // An unreleased ticket would cause conflict before the closed check.
      await expectLater(
        f.service.admissions.admit(intent(), CancellationToken()),
        throwsA(failure(FailureKind.cancelled)),
      );
    },
  );
  test(
    'prepare cleanup failure releases queue admission and all session work',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      final expected = StateError(
        'synthetic successful-prepare cleanup failure',
      );
      f.authority.onAuthorize = () {
        f.authority.readCancellation!.onCancel(() => throw expected);
      };
      await expectLater(f.prepare(), throwsA(same(expected)));
      expect(f.backend.inspections, 1);
      expect(f.backend.stages, 0);
      expect(f.session.active, isEmpty);
      final idle = f.coordinator.acquireIdle();
      idle.check();
      idle.release();
      // The cancelled proof also releases its independent admission slot.
      f.authority.onAuthorize = null;
      final ticket = await f.service.admissions.admit(
        intent(),
        CancellationToken(),
      );
      ticket.close();
      expect(f.session.active, isEmpty);
    },
  );
  test(
    'interrupt continues every cancellation owner after a listener throws',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      final expected = StateError('synthetic interrupt callback failure');
      f.authority.readGate = Completer<void>();
      f.authority.onAuthorize = () {
        f.authority.readCancellation!.onCancel(() => throw expected);
      };
      final prepared = expectLater(
        f.prepare(),
        throwsA(failure(FailureKind.cancelled)),
      );
      await f.authority.readEntered.future;
      try {
        expect(f.service.interrupt, throwsA(same(expected)));
        expect(f.session.active, hasLength(2));
        expect(f.session.active.every((token) => token.isCancelled), isTrue);
        expect(f.backend.cancellations, 1);
        expect(
          () => f.service.prepareLocalStage(
            intent(),
            CancellationToken(),
            coordinator: f.coordinator,
          ),
          throwsA(failure(FailureKind.cancelled)),
        );
      } finally {
        if (!f.authority.readGate!.isCompleted) {
          f.authority.readGate!.complete();
        }
      }
      await prepared;
      expect(f.session.active, isEmpty);
      final idle = f.coordinator.acquireIdle();
      idle.release();
    },
  );
  test(
    'failed settlement suspends the queue even when cancellation throws',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      final first = StateError('synthetic native drain failure');
      f.backend.drainFailure = first;
      f.authority.onAuthorize = () {
        f.authority.readCancellation!.onCancel(
          () => throw StateError('later synthetic cancellation failure'),
        );
      };
      try {
        await expectLater(f.prepare(), throwsA(same(first)));
        expect(
          f.coordinator.acquireIdle,
          throwsA(failure(FailureKind.conflict)),
        );
        expect(f.backend.cancellations, greaterThanOrEqualTo(1));
        expect(f.backend.stages, 0);
        expect(f.session.active, isEmpty);
      } finally {
        f.backend.drainFailure = null;
      }
    },
  );
  for (final closeOwner in ['admission', 'content-service']) {
    test(
      'synchronous authority callback closes $closeOwner only after actual read settles',
      () async {
        final f = Fixture();
        addTearDown(f.close);
        f.authority.readGate = Completer<void>();
        late Future<void> closing;
        var settled = false;
        f.authority.onAuthorize = () {
          closing = closeOwner == 'admission'
              ? f.service.admissions.close()
              : f.service.close();
          unawaited(
            closing.then((_) {
              settled = true;
            }),
          );
        };
        final prepared = expectLater(
          f.prepare(),
          throwsA(failure(FailureKind.cancelled)),
        );
        await f.authority.readEntered.future;
        // Drain microtasks without releasing the held underlying source result.
        await Future<void>.delayed(Duration.zero);
        expect(f.authority.readCancellation!.isCancelled, isTrue);
        expect(settled, isFalse);
        expect(f.backend.inspections, 0);
        f.authority.readGate!.complete();
        await prepared;
        await closing;
        expect(settled, isTrue);
        expect(f.store.read()['managedInstallations'], isNull);
      },
    );
    test(
      'reentrant cancellation observes the published $closeOwner close future',
      () async {
        final f = Fixture();
        addTearDown(f.close);
        final ticket = await f.service.admissions.admit(
          intent(),
          CancellationToken(),
        );
        final guard = ticket.claim(
          intent: intent(),
          session: f.session,
          deviceId: f.backend.deviceId,
        );
        Future<void> close() => closeOwner == 'admission'
            ? f.service.admissions.close()
            : f.service.close();
        Future<void>? reentrant;
        final remove = guard.cancellation.onCancel(() {
          reentrant = close();
        });
        addTearDown(remove);
        final closing = close();
        expect(identical(reentrant, closing), isTrue);
        await closing;
      },
    );
  }
  for (final callback in ['source-generation', 'clock']) {
    test(
      'post-authorize $callback getter cannot retain a ticket after closing',
      () async {
        final f = Fixture();
        addTearDown(f.close);
        late Future<void> closing;
        f.authority.onAuthorize = () {
          void close() {
            f.authority.onReadGeneration = null;
            f.onNow = null;
            closing = f.service.admissions.close();
          }

          if (callback == 'source-generation') {
            f.authority.onReadGeneration = close;
          } else {
            f.onNow = close;
          }
        };
        await expectLater(
          f.service.admissions.admit(intent(), CancellationToken()),
          throwsA(failure(FailureKind.cancelled)),
        );
        await closing;
        // A late retained ticket would make this return conflict instead.
        await expectLater(
          f.service.admissions.admit(intent(), CancellationToken()),
          throwsA(failure(FailureKind.cancelled)),
        );
        expect(f.authority.listeners, isEmpty);
        expect(f.backend.inspections, 0);
      },
    );
  }
  test('tickets are tied to exact request, session identity and device, then one-use', () async {
    final f = Fixture();
    addTearDown(f.close);
    final ticket = await f.service.admissions.admit(
      intent(),
      CancellationToken(),
    );
    addTearDown(ticket.close);
    final other = SessionController()..replace(scope);
    addTearDown(other.dispose);
    expect(
      () => ticket.claim(
        intent: intent(),
        session: other,
        deviceId: f.backend.deviceId,
      ),
      throwsA(failure(FailureKind.conflict)),
    );
    expect(
      () => ticket.claim(
        intent: intent(item: 'Other'),
        session: f.session,
        deviceId: f.backend.deviceId,
      ),
      throwsA(failure(FailureKind.conflict)),
    );
    expect(
      () => ticket.claim(
        intent: intent(),
        session: f.session,
        deviceId: '${'4' * 16}:${'5' * 32}',
      ),
      throwsA(failure(FailureKind.conflict)),
    );
    final guard = ticket.claim(
      intent: intent(),
      session: f.session,
      deviceId: f.backend.deviceId,
    );
    expect(
      () => ticket.claim(
        intent: intent(),
        session: f.session,
        deviceId: f.backend.deviceId,
      ),
      throwsA(failure(FailureKind.conflict)),
    );
    guard.close();
    expect(guard.cancellation.isCancelled, isTrue);
  });
  for (final callback in ['authority-getter', 'request-comparison']) {
    test('a $callback cannot claim the same ticket reentrantly', () async {
      final f = Fixture();
      addTearDown(f.close);
      final ticket = await f.service.admissions.admit(
        intent(),
        CancellationToken(),
      );
      addTearDown(ticket.close);
      ManagedContentAdmissionGuard? nested;
      Object? nestedFailure;
      void reenter() {
        f.authority.onReadGeneration = null;
        try {
          nested = ticket.claim(
            intent: intent(),
            session: f.session,
            deviceId: f.backend.deviceId,
          );
        } catch (error) {
          nestedFailure = error;
        }
      }

      if (callback == 'authority-getter') {
        f.authority.onReadGeneration = reenter;
      }
      final request = callback == 'request-comparison'
          ? ReentrantIntent(intent(), reenter)
          : intent();
      final outer = ticket.claim(
        intent: request,
        session: f.session,
        deviceId: f.backend.deviceId,
      );
      expect(nested, isNull);
      expect(nestedFailure, failure(FailureKind.conflict));
      outer.check();
      expect(
        () => ticket.claim(
          intent: intent(),
          session: f.session,
          deviceId: f.backend.deviceId,
        ),
        throwsA(failure(FailureKind.conflict)),
      );
      outer.close();
    });
  }
  testWidgets(
    'expiry timer cancels the actual token without a later result poll',
    (tester) async {
      final f = Fixture();
      try {
        final ticket = await f.service.admissions.admit(
          intent(),
          CancellationToken(),
        );
        final guard = ticket.claim(
          intent: intent(),
          session: f.session,
          deviceId: f.backend.deviceId,
        );
        try {
          await tester.pump(const Duration(seconds: 31));
          expect(guard.cancellation.isCancelled, isTrue);
          expect(guard.check, throwsA(failure(FailureKind.cancelled)));
        } finally {
          guard.close();
        }
      } finally {
        // Settle futures in the same fake-async zone that created the fixture.
        await f.close();
      }
    },
    timeout: const Timeout(Duration(seconds: 30)),
  );
  for (final closeOwner in ['guard', 'admission-service']) {
    testWidgets(
      'expiry listener failure stays owned until $closeOwner closes',
      (tester) async {
        final f = Fixture();
        final first = StateError('first synthetic expiry cancellation failure');
        if (closeOwner == 'admission-service') f.expectedCloseFailure = first;
        try {
          final ticket = await f.service.admissions.admit(
            intent(),
            CancellationToken(),
          );
          final guard = ticket.claim(
            intent: intent(),
            session: f.session,
            deviceId: f.backend.deviceId,
          );
          var laterListenerRan = false;
          guard.cancellation.onCancel(() => throw first);
          guard.cancellation.onCancel(
            () =>
                throw StateError('later synthetic expiry cancellation failure'),
          );
          guard.cancellation.onCancel(() => laterListenerRan = true);
          await tester.pump(const Duration(seconds: 31));
          expect(tester.takeException(), isNull);
          expect(guard.cancellation.isCancelled, isTrue);
          expect(laterListenerRan, isTrue);
          expect(guard.check, throwsA(same(first)));
          if (closeOwner == 'guard') {
            expect(guard.close, throwsA(same(first)));
            expect(f.session.active, isEmpty);
            final next = await f.service.admissions.admit(
              intent(),
              CancellationToken(),
            );
            next.close();
          } else {
            await expectLater(
              f.service.admissions.close(),
              throwsA(same(first)),
            );
            await expectLater(
              f.service.admissions.admit(intent(), CancellationToken()),
              throwsA(failure(FailureKind.cancelled)),
            );
          }
          expect(f.session.active, isEmpty);
          expect(tester.takeException(), isNull);
        } finally {
          await f.close();
        }
      },
      timeout: const Timeout(Duration(seconds: 30)),
    );
  }
  test('exact maximum grant age rejects synchronously before a zero timer could act', () async {
    final f = Fixture();
    addTearDown(f.close);
    f.authority.grantAge = ManagedContentAdmissionService.maximumGrantAge;
    await expectLater(f.prepare(), throwsA(failure(FailureKind.denied)));
    expect(f.backend.inspections, 0);
    expect(f.store.transactions, 0);
  });
  test('one microsecond before maximum age permits immediate claim at the checked clock', () async {
    final f = Fixture();
    addTearDown(f.close);
    f.authority.grantAge =
        ManagedContentAdmissionService.maximumGrantAge -
        const Duration(microseconds: 1);
    final ticket = await f.service.admissions.admit(
      intent(),
      CancellationToken(),
    );
    final guard = ticket.claim(
      intent: intent(),
      session: f.session,
      deviceId: f.backend.deviceId,
    );
    guard.check();
    guard.close();
    expect(guard.cancellation.isCancelled, isTrue);
  });
  test(
    'source invalidation cancels the actual native-work token, then settles',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      await f.prepare();
      f.backend.stageGate = Completer<void>();
      final run = f.coordinator.drain(f.session.scope);
      await f.backend.stageEntered.future;
      f.authority.revoke();
      expect(f.backend.stageCancellation!.isCancelled, isTrue);
      f.backend.stageGate!.complete();
      await run;
      expect(f.backend.copied, isFalse);
      expect(
        f.inventory.readCurrent().single.state,
        ManagedInstallationState.recoveryRequired,
      );
    },
  );
  test(
    'sign-out settles a guard which closes itself during cancellation',
    () async {
      final f = Fixture();
      addTearDown(f.close);
      final ticket = await f.service.admissions.admit(
        intent(),
        CancellationToken(),
      );
      final guard = ticket.claim(
        intent: intent(),
        session: f.session,
        deviceId: f.backend.deviceId,
      );
      guard.cancellation.onCancel(guard.close);
      expect(f.session.signOut, returnsNormally);
      expect(guard.cancellation.isCancelled, isTrue);
      expect(f.session.scope.isLocal, isTrue);
    },
  );
  test('interrupt closes local admission before token callbacks can reenter inert preparation', () async {
    final f = Fixture();
    addTearDown(f.close);
    final ticket = await f.service.admissions.admit(
      intent(),
      CancellationToken(),
    );
    addTearDown(ticket.close);
    final guard = ticket.claim(
      intent: intent(),
      session: f.session,
      deviceId: f.backend.deviceId,
    );
    Future<JobRecord>? nested;
    Object? nestedFailure;
    final remove = guard.cancellation.onCancel(() {
      try {
        nested = f.service.prepareLocalStage(
          intent(),
          CancellationToken(),
          coordinator: f.coordinator,
        );
      } catch (error) {
        nestedFailure = error;
      }
    });
    addTearDown(remove);
    f.service.interrupt();
    // Consume an unexpected future before asserting, keeping the failing case bounded.
    if (nested != null) {
      await expectLater(nested, throwsA(failure(FailureKind.cancelled)));
    }
    expect(nested, isNull);
    expect(nestedFailure, failure(FailureKind.cancelled));
    expect(f.backend.inspections, 0);
    expect(f.store.transactions, 0);
  });
  for (final revoked in [false, true]) {
    test(
      'explicit manifested resume revalidates authority; revoked=$revoked',
      () async {
        final f = Fixture();
        addTearDown(f.close);
        await f.prepare();
        f.backend.completeCopy = false;
        await f.coordinator.drain(f.session.scope);
        await f.service.inspectRecord(
          intent().operationId,
          CancellationToken(),
        );
        expect(f.service.canResume(intent().operationId), isTrue);
        final originalProvenance = f.inventory
            .readCurrent()
            .single
            .manifest!
            .recordIdentity;
        if (revoked) f.authority.revoke();
        f.backend.completeCopy = true;
        final resumed = f.service.resumeRecord(
          intent().operationId,
          CancellationToken(),
          coordinator: f.coordinator,
        );
        if (revoked) {
          await expectLater(resumed, throwsA(failure(FailureKind.unsupported)));
          expect(f.backend.stages, 1);
          expect(f.authority.requests, 2);
          expect(f.inventory.readCurrent().single.attempt, 0);
        } else {
          await resumed;
          expect(f.backend.stages, 2);
          expect(f.authority.requests, 4);
          expect(
            f.inventory.readCurrent().single.state,
            ManagedInstallationState.staged,
          );
          expect(f.inventory.readCurrent().single.attempt, 1);
        }
        expect(
          f.inventory.readCurrent().single.manifest!.recordIdentity,
          originalProvenance,
        );
      },
    );
  }
  test('inspection and durable exact-owned cleanup remain possible after rights loss', () async {
    final f = Fixture();
    addTearDown(f.close);
    await f.prepare();
    await f.coordinator.drain(f.session.scope);
    expect(f.backend.stages, 1);
    expect(f.authority.requests, 2);
    f.authority.revoke();
    await f.service.inspectRecord(intent().operationId, CancellationToken());
    expect(f.authority.requests, 2);
    final lease = f.inventory.leaseCurrent(intent().operationId)!;
    await f.inventory.prepareDiscard(
      lease,
      Uint8List(512 * 1024),
      100,
      1,
      CancellationToken(),
      jobs: f.jobs,
      checkAdmission: () {},
    );
    expect(
      f.inventory.readCurrent().single.state,
      ManagedInstallationState.discardPrepared,
    );
    // Opaque synthetic plan only: no native deletion is claimed by this test.
  });

  final invalid = <String, List<Map<String, Object?>>>{
    'traversal': [file('../a')],
    'stream': [file('a:stream')],
    'reserved': [file('CON.txt')],
    'case': [file('A'), file('a')],
    'file-directory': [file('assets'), file('assets/a')],
    'directory-file': [file('assets/a'), file('assets')],
    'undeclared exe': [file('game.exe')],
    'expansion': [
      file('a', size: managedManifestMaximumPayloadBytes),
      file('b', size: 1),
    ],
    'entries': List.generate(257, (i) => file('a$i')),
  };
  for (final entry in invalid.entries) {
    test('normalized manifest rejects ${entry.key}', () {
      expect(
        () => ManagedContentManifest.decode(
          manifest('Game-A', files: entry.value),
        ),
        throwsA(failure(FailureKind.invalidData)),
      );
    });
  }
  test(
    'manifest bounds and unambiguous normalized encoding precede parsing',
    () {
      expect(
        () => ManagedContentManifest.decode(Uint8List(65537)),
        throwsA(failure(FailureKind.invalidData)),
      );
      final raw = utf8
          .decode(manifest('Game-A'))
          .replaceFirst('"version":1', '"version":0,"version":1');
      expect(
        () =>
            ManagedContentManifest.decode(Uint8List.fromList(utf8.encode(raw))),
        throwsA(failure(FailureKind.invalidData)),
      );
    },
  );
}
