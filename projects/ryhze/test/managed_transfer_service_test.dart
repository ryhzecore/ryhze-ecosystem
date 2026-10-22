import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/features/installations/managed_content_service.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_installation_repository.dart';
import 'package:ryhze/features/installations/managed_transfer_controller.dart';
import 'package:ryhze/features/installations/managed_transfer_record.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/session/session_controller.dart';

import 'managed_transfer_fixture.dart';

Matcher failure(FailureKind kind) =>
    throwsA(isA<AppFailure>().having((error) => error.kind, 'kind', kind));

Future<TransferFixture> fixture({int size = 600000}) async {
  final result = await TransferFixture.create(size: size);
  addTearDown(result.close);
  return result;
}

/// The service publishes idle after completing preparation, before its caller's
/// await resumes. Exercise that actual notification boundary without replacing
/// the service or inventing a production scheduling hook.
bool Function() afterPreparation(TransferFixture f, void Function() callback) {
  var fired = false;
  void listener() {
    if (!fired && !f.service.busy) {
      fired = true;
      callback();
    }
  }

  f.service.addListener(listener);
  addTearDown(() => f.service.removeListener(listener));
  return () => fired;
}

Future<void> interruptPrefix(TransferFixture f, {int length = 4}) async {
  f.onRequest = (request) => f.respondPrefix(request, length);
  await f.prepare();
  final running = f.run();
  await f.backend.appended.future;
  f.service.cancelPending();
  await running;
  expect(
    f.repository.readCurrent().single.state,
    ManagedTransferState.recoveryRequired,
  );
  expect(f.backend.files.values.single.length, length);
}

({
  TransferStagingBackend backend,
  ManagedInstallationRepository inventory,
  ManagedContentService service,
  JobCoordinator coordinator,
})
stagingFor(TransferFixture f) {
  final backend = TransferStagingBackend(f.backend);
  final inventory = ManagedInstallationRepository(
    f.store,
    f.session,
    deviceId: backend.deviceId,
  );
  final service = ManagedContentService(
    inventory,
    backend,
    f.jobs,
    f.session,
    authority: f.authority,
    now: () => transferNow,
  );
  final coordinator = JobCoordinator(f.jobs, {
    'managedContentTransfer': f.service,
    'managedContentTransferDiscard': f.service,
    ManagedContentService.jobKind: service,
  });
  addTearDown(() async {
    await coordinator.close();
    await service.close();
    inventory.dispose();
  });
  return (
    backend: backend,
    inventory: inventory,
    service: service,
    coordinator: coordinator,
  );
}

void main() {
  for (final action in ['download', 'retry', 'stage', 'discard']) {
    test(
      'cancelling after $action preparation retires only its unstarted job',
      () async {
        final f = await fixture(size: 16);
        if (action == 'retry') {
          await interruptPrefix(f);
        } else if (action != 'download') {
          await f.prepare();
          await f.run();
        }
        final staging = action == 'stage' ? stagingFor(f) : null;
        final coordinator = staging?.coordinator ?? f.coordinator;
        final controller = ManagedTransferController(
          service: f.service,
          coordinator: coordinator,
          session: f.session,
          staging: staging?.service,
        );
        addTearDown(controller.dispose);
        if (action == 'discard') {
          await controller.reviewRemoval(f.intent.operationId);
          expect(controller.failure, isNull);
        }
        final requests = f.requests;
        final begins = f.backend.begins;
        final fired = afterPreparation(f, controller.cancel);
        switch (action) {
          case 'download':
            await controller.download(f.intent);
          case 'retry':
            await controller.retry(f.intent.operationId);
          case 'stage':
            await controller.stage(f.intent.operationId);
          case 'discard':
            await controller.confirmRemoval();
        }
        expect(fired(), isTrue);
        expect(controller.failure?.kind, FailureKind.cancelled);
        expect(f.jobs.read(transferScope).last.state, JobState.cancelled);
        expect(
          f.jobs
              .read(transferScope)
              .where((job) => job.state == JobState.queued),
          isEmpty,
        );
        await coordinator.drain(transferScope);
        expect(f.requests, requests);
        expect(f.backend.begins, begins);
        expect(f.backend.discarded, isEmpty);
        expect(staging?.backend.stages ?? 0, 0);
      },
    );
  }

  test(
    'account replacement after preparation retires the original scope job',
    () async {
      final f = await fixture(size: 16);
      const otherScope = AccountScope.verified('test-only-issuer', 'Other');
      final otherJob = await f.jobs.enqueue(
        otherScope,
        'unrelated',
        'other-item',
      );
      final controller = ManagedTransferController(
        service: f.service,
        coordinator: f.coordinator,
        session: f.session,
      );
      addTearDown(controller.dispose);
      final fired = afterPreparation(f, () => f.session.replace(otherScope));
      await controller.download(f.intent);
      expect(fired(), isTrue);
      expect(f.jobs.read(transferScope).single.state, JobState.cancelled);
      expect(f.jobs.read(otherScope).single.key, otherJob.key);
      expect(f.jobs.read(otherScope).single.state, JobState.queued);
      expect(controller.records, isEmpty);
      await f.coordinator.drain(transferScope);
      expect(f.backend.begins, 0);
      expect(f.requests, 0);
    },
  );

  test(
    'a competing idle owner after preparation preserves its reservation',
    () async {
      final f = await fixture(size: 16);
      final controller = ManagedTransferController(
        service: f.service,
        coordinator: f.coordinator,
        session: f.session,
      );
      addTearDown(controller.dispose);
      JobAdmissionLease? otherOwner;
      addTearDown(() => otherOwner?.release());
      final fired = afterPreparation(f, () {
        otherOwner = f.coordinator.acquireIdle();
      });
      await controller.download(f.intent);
      expect(fired(), isTrue);
      expect(controller.failure?.kind, FailureKind.conflict);
      expect(f.jobs.read(transferScope).single.state, JobState.cancelled);
      otherOwner!.check();
      otherOwner!.release();
      await f.run();
      expect(f.backend.begins, 0);
      expect(f.requests, 0);
    },
  );

  for (final lostAck in [false, true]) {
    test(
      'unconfirmed queued cleanup (lost acknowledgment: $lostAck) closes admission',
      () async {
        final f = await fixture(size: 16);
        final controller = ManagedTransferController(
          service: f.service,
          coordinator: f.coordinator,
          session: f.session,
        );
        addTearDown(controller.dispose);
        final fired = afterPreparation(f, () {
          controller.cancel();
          if (lostAck) {
            f.store.failAfterNextCommit = true;
          } else {
            f.store.fail = true;
          }
        });
        await controller.download(f.intent);
        expect(fired(), isTrue);
        expect(controller.failure?.kind, FailureKind.recovery);
        expect(
          f.jobs.read(transferScope).single.state,
          lostAck ? JobState.cancelled : JobState.queued,
        );
        expect(f.coordinator.acquireIdle, failure(FailureKind.conflict));
        f.store.fail = false;
        await f.run();
        expect(f.backend.begins, 0);
        expect(f.requests, 0);
      },
    );
  }

  for (final replaceAccount in [false, true]) {
    test(
      '[admission audit] repository notification ${replaceAccount ? 'account replacement' : 'cancellation'} retires committed preparation',
      () async {
        final f = await fixture(size: 16);
        final controller = ManagedTransferController(
          service: f.service,
          coordinator: f.coordinator,
          session: f.session,
        );
        addTearDown(controller.dispose);
        var scheduled = false;
        void listener() {
          if (scheduled) return;
          scheduled = true;
          scheduleMicrotask(() {
            if (replaceAccount) {
              f.session.replace(
                const AccountScope.verified('test-only-issuer', 'Other'),
              );
            } else {
              controller.cancel();
            }
          });
        }

        f.repository.addListener(listener);
        addTearDown(() => f.repository.removeListener(listener));
        await controller.download(f.intent);
        expect(scheduled, isTrue);
        expect(
          controller.failure?.kind,
          replaceAccount ? isNull : FailureKind.cancelled,
        );
        expect(f.jobs.read(transferScope).single.state, JobState.cancelled);
        await f.coordinator.drain(transferScope);
        expect(f.backend.begins, 0);
        expect(f.requests, 0);
        expect(f.backend.discarded, isEmpty);
      },
    );
  }

  for (final action in ['retry', 'discard']) {
    test(
      '[admission audit] lost $action admission acknowledgment closes the queue',
      () async {
        final f = await fixture(size: 16);
        if (action == 'retry') {
          await interruptPrefix(f);
        } else {
          await f.prepare();
          await f.run();
        }
        final controller = ManagedTransferController(
          service: f.service,
          coordinator: f.coordinator,
          session: f.session,
        );
        addTearDown(controller.dispose);
        if (action == 'discard') {
          await controller.reviewRemoval(f.intent.operationId);
          expect(controller.failure, isNull);
          f.store.failAfterNextCommit = true;
        } else {
          // Retry first records its inspection. Lose acknowledgment of the
          // following admission transaction, after its exact job is durable.
          f.store.afterCommit = () => f.store.failAfterNextCommit = true;
        }
        final requests = f.requests;
        final begins = f.backend.begins;
        if (action == 'discard') {
          await controller.confirmRemoval();
        } else {
          await controller.retry(f.intent.operationId);
        }
        expect(controller.failure?.kind, FailureKind.storage);
        expect(f.jobs.read(transferScope).last.state, JobState.queued);
        expect(f.coordinator.acquireIdle, failure(FailureKind.conflict));
        await f.run();
        expect(f.requests, requests);
        expect(f.backend.begins, begins);
        expect(f.backend.discarded, isEmpty);
      },
    );
  }

  test(
    'queued cleanup preserves running and terminal history and other attempts',
    () async {
      final f = await fixture(size: 16);
      final job = await f.prepare();
      final unrelated = await f.jobs.enqueue(
        transferScope,
        'unrelated',
        'other-item',
      );
      await f.jobs.transition(transferScope, job, JobState.running);
      expect(await f.jobs.cancelUnstarted(transferScope, job), isFalse);
      expect(f.jobs.read(transferScope).first.state, JobState.running);
      await f.jobs.transition(transferScope, job, JobState.completed);
      expect(await f.jobs.cancelUnstarted(transferScope, job), isFalse);
      expect(f.jobs.read(transferScope).first.state, JobState.completed);
      expect(f.jobs.read(transferScope).last.key, unrelated.key);
      expect(f.jobs.read(transferScope).last.state, JobState.queued);
    },
  );

  test(
    'preparation durably admits ownership before HTTP or file creation',
    () async {
      final f = await fixture();
      final job = await f.prepare();
      expect(job.state, JobState.queued);
      expect(
        f.repository.readCurrent().single.state,
        ManagedTransferState.prepared,
      );
      expect(f.backend.begins, 0);
      expect(f.backend.files, isEmpty);
      expect(f.requests, 0);

      f.backend.beforeBegin = () {
        expect(
          f.repository.readCurrent().single.state,
          ManagedTransferState.receiving,
        );
        expect(f.jobs.read(transferScope).single.state, JobState.running);
      };
      await f.run();
      final record = f.repository.readCurrent().single;
      expect(record.state, ManagedTransferState.downloaded);
      expect(record.request.observedBytes, f.payload.length);
      expect(record.request.observedSha256, digest(f.payload));
      expect(f.backend.files.values.single, f.payload);
      expect(f.backend.appends, greaterThan(1));
      expect(f.requests, 1);
      expect(f.jobs.read(transferScope).single.state, JobState.completed);
      expect(f.jobs.read(transferScope).single.checkpoint, f.payload.length);
      expect(f.store.read()['managedInstallations'], isNull);
    },
  );

  for (final stage in ['preparation', 'execution']) {
    test('store failure during $stage prevents file/network effects', () async {
      final f = await fixture();
      if (stage == 'execution') await f.prepare();
      f.store.fail = true;
      await expectLater(
        stage == 'preparation' ? f.prepare() : f.run(),
        failure(FailureKind.storage),
      );
      expect(f.backend.begins, 0);
      expect(f.requests, 0);
      expect(f.backend.files, isEmpty);
    });
  }

  for (final rejected in [
    'source-fixture',
    'source-scope',
    'grant-scope',
    'artifact',
  ]) {
    test('$rejected cannot reserve or touch content', () async {
      final f = await fixture();
      switch (rejected) {
        case 'source-fixture':
          f.authority.sourceFixture = true;
        case 'source-scope':
          f.authority.wrongSourceScope = true;
        case 'grant-scope':
          f.authority.wrongGrantScope = true;
        case 'artifact':
          f.authority.wrongSourceArtifact = true;
      }
      await expectLater(f.prepare(), failure(FailureKind.denied));
      expect(f.repository.readCurrent(), isEmpty);
      expect(f.jobs.read(transferScope), isEmpty);
      expect(f.backend.begins, 0);
      expect(f.requests, 0);
    });
  }

  test(
    'execution obtains fresh authority and refuses changed source identity',
    () async {
      final f = await fixture();
      await f.prepare();
      f.authority.sourceKey = 'different-source';
      await f.run();
      expect(f.backend.begins, 0);
      expect(f.requests, 0);
      expect(f.jobs.read(transferScope).single.failure, FailureKind.denied);
      expect(
        f.repository.readCurrent().single.state,
        ManagedTransferState.recoveryRequired,
      );
    },
  );

  test(
    'revalidated prefix resumes into a fresh file using real HTTP Range',
    () async {
      final f = await fixture(size: 16);
      await interruptPrefix(f);
      final oldPath = f.backend.files.keys.single;
      final before = Uint8List.fromList(f.backend.files[oldPath]!);
      f.onRequest = (request) => f.respond(request, offset: 4);
      final retry = await f.service.prepareRetry(
        f.intent.operationId,
        CancellationToken(),
        coordinator: f.coordinator,
      );
      expect(retry.state, JobState.queued);
      expect(f.requests, 1);
      expect(f.backend.begins, 1);
      await f.run();
      final record = f.repository.readCurrent().single;
      expect(record.state, ManagedTransferState.downloaded);
      expect(record.request.attempt, 1);
      expect(record.previous?.sourceRelative, oldPath);
      expect(f.backend.files[oldPath], before);
      expect(f.backend.files[record.request.sourceRelative], f.payload);
      expect(f.requestRanges, [null, 'bytes=4-15']);
      expect(
        f.jobs
            .read(transferScope)
            .where((job) => job.state == JobState.completed)
            .length,
        1,
      );
    },
  );

  test(
    'a prefix without a strong tag is retained but retried from zero',
    () async {
      final f = await fixture(size: 16);
      f.etag = null;
      await interruptPrefix(f);
      f.onRequest = f.respond;
      await f.service.prepareRetry(
        f.intent.operationId,
        CancellationToken(),
        coordinator: f.coordinator,
      );
      await f.run();
      final record = f.repository.readCurrent().single;
      expect(record.state, ManagedTransferState.downloaded);
      expect(f.requestRanges, [null, null]);
      expect(
        f.backend.files[record.previous!.sourceRelative],
        f.payload.sublist(0, 4),
      );
      expect(f.backend.files[record.request.sourceRelative], f.payload);
    },
  );

  test(
    'a changed range entity preserves the copied prefix without appending',
    () async {
      final f = await fixture(size: 16);
      await interruptPrefix(f);
      f.etag = '"changed-version"';
      f.onRequest = (request) => f.respond(request, offset: 4);
      await f.service.prepareRetry(
        f.intent.operationId,
        CancellationToken(),
        coordinator: f.coordinator,
      );
      await f.run();
      final record = f.repository.readCurrent().single;
      expect(record.state, ManagedTransferState.recoveryRequired);
      expect(
        f.backend.files[record.request.sourceRelative],
        f.payload.sublist(0, 4),
      );
      expect(f.backend.appends, 1);
      expect(f.jobs.read(transferScope).last.state, JobState.recoveryRequired);
    },
  );

  test(
    'payload corruption cannot become downloaded despite complete HTTP',
    () async {
      final f = await fixture(size: 16);
      f.onRequest = (request) => f.respond(request, bytes: Uint8List(16));
      await f.prepare();
      await f.run();
      expect(f.requests, 1);
      expect(f.backend.files.values.single.length, 16);
      expect(
        f.repository.readCurrent().single.state,
        ManagedTransferState.recoveryRequired,
      );
      expect(
        f.jobs.read(transferScope).single.state,
        JobState.recoveryRequired,
      );
      expect(
        f.jobs.read(transferScope).single.failure,
        FailureKind.invalidData,
      );
    },
  );

  for (final boundary in ['cancel', 'revoke', 'account']) {
    test(
      '$boundary awaits active writer before releasing work ownership',
      () async {
        final f = await fixture(size: 16);
        f.backend.appendGate = Completer<void>();
        await f.prepare();
        var settled = false;
        final running = f.run().whenComplete(() {
          settled = true;
        });
        await f.backend.appendEntered.future;
        switch (boundary) {
          case 'cancel':
            f.service.cancelPending();
          case 'revoke':
            f.authority.revoke();
          case 'account':
            f.session.replace(
              const AccountScope.verified('test-only-issuer', 'Other'),
            );
        }
        await Future<void>.delayed(Duration.zero);
        expect(settled, isFalse);
        expect(f.service.busy, isTrue);
        expect(f.backend.appends, 0);
        if (boundary == 'account') expect(f.repository.readCurrent(), isEmpty);
        f.backend.appendGate!.complete();
        await running;
        expect(f.service.busy, isFalse);
        expect(
          f.jobs.read(transferScope).single.state,
          JobState.recoveryRequired,
        );
        expect(f.backend.files.values.single, isEmpty);
        if (boundary == 'account') f.session.replace(transferScope);
        expect(
          f.repository.readCurrent().single.state,
          ManagedTransferState.recoveryRequired,
        );
      },
    );
  }

  test(
    'close awaits in-flight authority resolution instead of abandoning it',
    () async {
      final f = await fixture();
      f.authority.resolveGate = Completer<void>();
      final preparing = f.prepare();
      final checked = expectLater(preparing, failure(FailureKind.cancelled));
      await f.authority.resolveEntered.future;
      var settled = false;
      final closing = f.service.close().whenComplete(() {
        settled = true;
      });
      await Future<void>.delayed(Duration.zero);
      expect(settled, isFalse);
      f.authority.resolveGate!.complete();
      await checked;
      await closing;
      expect(f.requests, 0);
      expect(f.backend.begins, 0);
    },
  );

  test(
    'restart reconciliation observes completed bytes without repeating HTTP',
    () async {
      final f = await fixture(size: 16);
      f.backend.throwAfterFinish = true;
      final job = await f.prepare();
      await f.run();
      expect(
        f.repository.readCurrent().single.state,
        ManagedTransferState.recoveryRequired,
      );
      expect(f.backend.files.values.single, f.payload);
      f.backend.throwAfterFinish = false;
      await f.service.recoverInterrupted();
      await f.coordinator.reconcile(transferScope, job);
      expect(f.requests, 1);
      expect(f.backend.begins, 1);
      expect(
        f.repository.readCurrent().single.state,
        ManagedTransferState.downloaded,
      );
      expect(f.jobs.read(transferScope).single.state, JobState.completed);
    },
  );

  test(
    'recovery then Inspect completes the observed job and permits Stage',
    () async {
      final f = await fixture(size: 16);
      f.backend.throwAfterFinish = true;
      await f.prepare();
      await f.run();
      f.backend.throwAfterFinish = false;
      await f.service.recoverInterrupted();
      final staging = stagingFor(f);
      final controller = ManagedTransferController(
        service: f.service,
        coordinator: staging.coordinator,
        session: f.session,
        staging: staging.service,
      );
      addTearDown(controller.dispose);
      await controller.inspect(f.intent.operationId);
      expect(controller.failure, isNull);
      expect(f.jobs.read(transferScope).single.state, JobState.completed);
      await controller.stage(f.intent.operationId);
      expect(controller.failure, isNull);
      expect(
        staging.inventory.readCurrent().single.state,
        ManagedInstallationState.staged,
      );
      expect(f.requests, 1);
      expect(staging.backend.stages, 1);
    },
  );

  test(
    'restart with partial bytes never automatically retries network',
    () async {
      final f = await fixture(size: 16);
      await interruptPrefix(f);
      final job = f.jobs.read(transferScope).single;
      await f.service.recoverInterrupted();
      await f.coordinator.reconcile(transferScope, job);
      await f.run();
      expect(f.requests, 1);
      expect(f.backend.begins, 1);
      expect(
        f.repository.readCurrent().single.state,
        ManagedTransferState.recoveryRequired,
      );
      expect(f.repository.readCurrent().single.request.observedBytes, 4);
    },
  );

  test('known incoming file missing before checkpoint requires reviewed removal before fresh download', () async {
    final f = await fixture(size: 16);
    f.backend.appendGate = Completer<void>();
    await f.prepare();
    final running = f.run();
    await f.backend.appendEntered.future;
    f.service.cancelPending();
    f.backend.appendGate!.complete();
    await running;
    final record = f.repository.readCurrent().single;
    expect(record.request.sourceIdentity, isNotEmpty);
    expect(record.request.observedSha256, isEmpty);
    // Model external removal after the exact begin identity was persisted.
    f.backend.files.remove(record.request.sourceRelative);
    f.backend.identities.remove(record.request.sourceRelative);
    final controller = ManagedTransferController(
      service: f.service,
      coordinator: f.coordinator,
      session: f.session,
    );
    addTearDown(controller.dispose);
    await controller.retry(record.operationId);
    expect(controller.failure?.kind, FailureKind.recovery);
    await controller.reviewRemoval(record.operationId);
    expect(controller.failure, isNull);
    await controller.confirmRemoval();
    expect(controller.failure, isNull);
    expect(
      f.repository.findCurrent(record.operationId)!.state,
      ManagedTransferState.discarded,
    );
    expect(f.backend.discarded, isEmpty);
    final next = ManagedStageIntent(
      operationId: 'b' * 64,
      item: f.intent.item,
      packageId: f.intent.packageId,
      version: f.intent.version,
      manifestSha256: f.intent.manifestSha256,
      sourceRelative: 'transfer-0/${'b' * 64}-0.bin',
      expectedSize: f.payload.length,
      expectedSha256: digest(f.payload),
      slot: 0,
    );
    await controller.download(next);
    expect(controller.failure, isNull);
    expect(
      f.repository.findCurrent(next.operationId)!.state,
      ManagedTransferState.downloaded,
    );
    expect(f.backend.files.values.single, f.payload);
  });

  for (final action in ['retry', 'discard']) {
    test(
      'interruption before writer creation permits explicit $action',
      () async {
        final f = await fixture(size: 16);
        final job = await f.prepare();
        await f.jobs.transition(transferScope, job, JobState.running);
        await f.jobs.transition(transferScope, job, JobState.recoveryRequired);
        await f.service.recoverInterrupted();
        final controller = ManagedTransferController(
          service: f.service,
          coordinator: f.coordinator,
          session: f.session,
        );
        addTearDown(controller.dispose);
        if (action == 'retry') {
          await controller.retry(f.intent.operationId);
          expect(controller.failure, isNull);
          expect(
            f.repository.readCurrent().single.state,
            ManagedTransferState.downloaded,
          );
          expect(f.backend.files.values.single, f.payload);
          expect(f.requests, 1);
        } else {
          await controller.reviewRemoval(f.intent.operationId);
          expect(controller.failure, isNull);
          await controller.confirmRemoval();
          expect(controller.failure, isNull);
          expect(
            f.repository.readCurrent().single.state,
            ManagedTransferState.discarded,
          );
          expect(f.backend.files, isEmpty);
          expect(f.requests, 0);
        }
      },
    );
  }

  test(
    'downloaded content can be inspected repeatedly and explicitly discarded',
    () async {
      final f = await fixture(size: 16);
      await f.prepare();
      await f.run();
      final controller = ManagedTransferController(
        service: f.service,
        coordinator: f.coordinator,
        session: f.session,
      );
      addTearDown(controller.dispose);
      await controller.inspect(f.intent.operationId);
      expect(controller.failure, isNull);
      await controller.inspect(f.intent.operationId);
      expect(controller.failure, isNull);
      await controller.reviewRemoval(f.intent.operationId);
      expect(controller.failure, isNull);
      expect(controller.removalId, f.intent.operationId);
      expect(f.backend.files.length, 1);
      await controller.confirmRemoval();
      expect(controller.failure, isNull);
      expect(f.backend.files, isEmpty);
      expect(
        f.repository.readCurrent().single.state,
        ManagedTransferState.discarded,
      );
      expect(f.requests, 1);
    },
  );

  test(
    'discard after retry removes only exact current and previous attempts',
    () async {
      final f = await fixture(size: 16);
      await interruptPrefix(f);
      f.onRequest = (request) => f.respond(request, offset: 4);
      await f.service.prepareRetry(
        f.intent.operationId,
        CancellationToken(),
        coordinator: f.coordinator,
      );
      await f.run();
      final previous = f.repository
          .readCurrent()
          .single
          .previous!
          .sourceRelative;
      final current = f.repository.readCurrent().single.request.sourceRelative;
      f.backend.files['unknown-neighbor.bin'] = Uint8List.fromList([9]);
      await f.service.prepareDiscard(
        f.intent.operationId,
        CancellationToken(),
        coordinator: f.coordinator,
      );
      await f.run();
      expect(f.backend.discarded, [previous, current]);
      expect(f.backend.files, {
        'unknown-neighbor.bin': [9],
      });
      expect(
        f.repository.readCurrent().single.state,
        ManagedTransferState.discarded,
      );
      expect(f.requests, 2);
    },
  );

  test(
    'removal confirmation expires when another inspection changes its revision',
    () async {
      final f = await fixture(size: 16);
      await f.prepare();
      await f.run();
      final controller = ManagedTransferController(
        service: f.service,
        coordinator: f.coordinator,
        session: f.session,
      );
      addTearDown(controller.dispose);
      await controller.reviewRemoval(f.intent.operationId);
      expect(controller.failure, isNull);
      await f.service.inspect(
        f.intent.operationId,
        CancellationToken(),
        coordinator: f.coordinator,
      );
      await controller.confirmRemoval();
      expect(controller.failure?.kind, FailureKind.conflict);
      expect(f.backend.files.length, 1);
    },
  );

  test(
    'a failed native drain closes queue admission until explicit recovery',
    () async {
      final f = await fixture(size: 16);
      await f.prepare();
      f.backend.failDrain = true;
      await f.run();
      expect(
        f.jobs.read(transferScope).single.state,
        JobState.recoveryRequired,
      );
      expect(f.coordinator.acquireIdle, failure(FailureKind.conflict));
      expect(
        f.repository.readCurrent().single.state,
        ManagedTransferState.recoveryRequired,
      );
    },
  );

  for (final operation in ['prepare', 'inspect', 'retry']) {
    test(
      '$operation drain failure preserves the coordinator admission hold',
      () async {
        final f = await fixture(size: 16);
        if (operation == 'inspect') {
          await f.prepare();
          await f.run();
        } else if (operation == 'retry') {
          await interruptPrefix(f);
        }
        final requests = f.requests;
        f.backend.failDrain = true;
        final Future<void> failed = switch (operation) {
          'prepare' => f.prepare().then<void>((_) {}),
          'inspect' => f.service.inspect(
            f.intent.operationId,
            CancellationToken(),
            coordinator: f.coordinator,
          ),
          _ =>
            f.service
                .prepareRetry(
                  f.intent.operationId,
                  CancellationToken(),
                  coordinator: f.coordinator,
                )
                .then<void>((_) {}),
        };
        await expectLater(failed, failure(FailureKind.recovery));
        expect(f.coordinator.acquireIdle, failure(FailureKind.conflict));
        expect(f.requests, requests);
      },
    );
  }

  test(
    'cancellation after atomic stage admission cancels its exact queued job',
    () async {
      final f = await fixture(size: 16);
      await f.prepare();
      await f.run();
      final staging = stagingFor(f);
      final cancellation = CancellationToken();
      f.store.afterCommit = cancellation.cancel;
      await expectLater(
        f.service.prepareStage(
          f.intent.operationId,
          cancellation,
          coordinator: staging.coordinator,
          staging: staging.service,
        ),
        failure(FailureKind.cancelled),
      );
      expect(
        f.jobs
            .read(transferScope)
            .where(
              (job) =>
                  job.kind == ManagedContentService.jobKind &&
                  job.state == JobState.queued,
            ),
        isEmpty,
      );
      await staging.coordinator.drain(transferScope);
      expect(staging.backend.stages, 0);
      expect(f.backend.files.values.single, f.payload);
      expect(f.requests, 1);
    },
  );

  test('failed handoff transaction preserves downloaded source without a stage job', () async {
    final f = await fixture(size: 16);
    await f.prepare();
    await f.run();
    final staging = stagingFor(f);
    f.store.fail = true;
    await expectLater(
      f.service.prepareStage(
        f.intent.operationId,
        CancellationToken(),
        coordinator: staging.coordinator,
        staging: staging.service,
      ),
      failure(FailureKind.storage),
    );
    expect(
      f.repository.readCurrent().single.state,
      ManagedTransferState.downloaded,
    );
    expect(staging.inventory.readCurrent(), isEmpty);
    expect(
      f.jobs
          .read(transferScope)
          .where((job) => job.kind == ManagedContentService.jobKind),
      isEmpty,
    );
    expect(staging.backend.stages, 0);
    expect(f.backend.files.values.single, f.payload);
  });

  test('lost stage commit acknowledgement retains coherent evidence and closes admission', () async {
    final f = await fixture(size: 16);
    await f.prepare();
    await f.run();
    final staging = stagingFor(f);
    f.store.failAfterNextCommit = true;
    await expectLater(
      f.service.prepareStage(
        f.intent.operationId,
        CancellationToken(),
        coordinator: staging.coordinator,
        staging: staging.service,
      ),
      failure(FailureKind.storage),
    );
    expect(
      f.repository.readCurrent().single.state,
      ManagedTransferState.stagingAdmitted,
    );
    expect(
      staging.inventory.readCurrent().single.state,
      ManagedInstallationState.prepared,
    );
    expect(staging.coordinator.acquireIdle, failure(FailureKind.conflict));
    await staging.coordinator.drain(transferScope);
    expect(staging.backend.stages, 0);
    expect(f.backend.files.values.single, f.payload);
    expect(f.requests, 1);
  });

  test('downloaded bytes enter existing authorized staging only on explicit handoff', () async {
    final f = await fixture(size: 16);
    await f.prepare();
    await f.run();
    final backend = TransferStagingBackend(f.backend);
    final inventory = ManagedInstallationRepository(
      f.store,
      f.session,
      deviceId: backend.deviceId,
    );
    final staging = ManagedContentService(
      inventory,
      backend,
      f.jobs,
      f.session,
      authority: f.authority,
      now: () => transferNow,
    );
    final coordinator = JobCoordinator(f.jobs, {
      'managedContentTransfer': f.service,
      'managedContentTransferDiscard': f.service,
      ManagedContentService.jobKind: staging,
    });
    addTearDown(() async {
      await coordinator.close();
      await staging.close();
      inventory.dispose();
    });
    await f.service.prepareStage(
      f.intent.operationId,
      CancellationToken(),
      coordinator: coordinator,
      staging: staging,
    );
    expect(backend.stages, 0);
    expect(f.requests, 1);
    expect(
      f.repository.readCurrent().single.state,
      ManagedTransferState.stagingAdmitted,
    );
    expect(
      inventory.readCurrent().single.state,
      ManagedInstallationState.prepared,
    );
    await coordinator.drain(transferScope);
    expect(backend.stages, 1);
    expect(
      inventory.readCurrent().single.state,
      ManagedInstallationState.staged,
    );
    expect(
      f.jobs
          .read(transferScope)
          .where((job) => job.state == JobState.completed)
          .length,
      2,
    );
  });
}
