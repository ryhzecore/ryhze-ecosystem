import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/features/installations/content_transfer_backend.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/managed_content_manifest.dart';
import 'package:ryhze/features/installations/managed_content_service.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_installation_repository.dart';
import 'package:ryhze/features/installations/managed_transfer_controller.dart';
import 'package:ryhze/features/installations/managed_transfer_panel.dart';
import 'package:ryhze/features/installations/managed_transfer_record.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

import 'managed_transfer_fixture.dart';

class _NoStagingEffects implements ContentStagingBackend {
  _NoStagingEffects(this.deviceId);
  @override
  final String deviceId;
  @override
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken token,
  ) async =>
      throw StateError('This widget test must not invoke staging effects');
  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken token,
  ) async =>
      throw StateError('This widget test must not invoke staging effects');
  @override
  Future<void> cancel() async {}
  @override
  Future<void> drain() async {}
  @override
  Future<void> close() async {}
}

ContentTransferRequest observed(
  ContentTransferRequest original,
  ContentTransferReceipt receipt,
) => ContentTransferRequest(
  slot: original.slot,
  operationId: original.operationId,
  expectedSize: original.expectedSize,
  expectedSha256: original.expectedSha256,
  attempt: original.attempt,
  rootIdentity: receipt.rootIdentity,
  directoryIdentity: receipt.directoryIdentity,
  sourceIdentity: receipt.sourceIdentity,
  observedBytes: receipt.observedBytes,
  observedSha256: receipt.observedSha256,
);

/// Deterministic UI setup through the repository and test-only native model.
/// Transport integration and actual native files are tested in separate suites.
Future<void> seedDownloaded(TransferFixture fixture) async {
  final token = CancellationToken();
  final manifest = ManagedManifestProvenance.create(
    authority: fixture.authority.authority,
    policy: fixture.authority.policy,
    sourceManifestSha256: digest(fixture.manifestBytes),
    normalizedBytes: fixture.manifestBytes,
    observedAt: transferNow,
    expiresAt: transferNow.add(const Duration(minutes: 5)),
  );
  final admission = await fixture.repository.prepareTransfer(
    fixture.intent,
    manifest,
    fixture.authority.sourceKey,
    token,
    jobs: fixture.jobs,
    checkAdmission: () {},
  );
  var lease = await fixture.repository.begin(
    admission.lease,
    admission.job.intentId!,
    token,
    checkAuthority: () {},
  );
  var request = lease.record.request;
  request = observed(request, await fixture.backend.begin(request, token));
  lease = await fixture.repository.observe(lease, request, token);
  request = observed(
    request,
    await fixture.backend.append(request, fixture.payload, token),
  );
  request = observed(request, await fixture.backend.finish(request, token));
  await fixture.backend.drain();
  await fixture.repository.observe(lease, request, token, complete: true);
  await fixture.jobs.transition(transferScope, admission.job, JobState.running);
  await fixture.jobs.transition(
    transferScope,
    admission.job,
    JobState.completed,
    checkpoint: fixture.payload.length,
  );
}

final previewBoundary = GlobalKey();

Widget app(ManagedTransferController controller, {double scale = 1}) =>
    MaterialApp(
      theme: RyhzeTokens.theme,
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context).copyWith(
          textScaler: TextScaler.linear(scale),
          disableAnimations: true,
        ),
        child: child!,
      ),
      home: Scaffold(
        body: RepaintBoundary(
          key: previewBoundary,
          child: SingleChildScrollView(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: ManagedTransferPanel(controller: controller),
            ),
          ),
        ),
      ),
    );

// Optional rendered fixture evidence, independent of the strict app references.
Future<void> capture(WidgetTester tester, String name) async {
  final directory = Platform.environment['RYHZE_TRANSFER_PREVIEW_DIR'];
  if (directory == null || directory.isEmpty) return;
  final boundary =
      previewBoundary.currentContext!.findRenderObject()!
          as RenderRepaintBoundary;
  await tester.runAsync(() async {
    final image = await boundary.toImage(pixelRatio: 1);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      if (bytes == null) throw StateError('Preview encoding failed');
      await Directory(directory).create(recursive: true);
      final path = '$directory${Platform.pathSeparator}$name.png';
      if (await File(path).exists()) throw StateError('Preserve prior preview');
      await File(path).writeAsBytes(
        bytes.buffer.asUint8List(bytes.offsetInBytes, bytes.lengthInBytes),
        flush: true,
      );
    } finally {
      image.dispose();
    }
  });
}

typedef _TransferSetup = ({
  TransferFixture fixture,
  ManagedTransferController controller,
});

void _testTransferWidgets(
  String description,
  Future<void> Function(WidgetTester, _TransferSetup) body,
) {
  testWidgets(description, (tester) async {
    final fixture = TransferFixture.withoutNetwork();
    final controller = ManagedTransferController(
      service: fixture.service,
      coordinator: fixture.coordinator,
      session: fixture.session,
    );
    try {
      await seedDownloaded(fixture);
      await body(tester, (fixture: fixture, controller: controller));
    } finally {
      // Await fixture futures before testWidgets leaves its fake-async zone.
      // addTearDown runs later and cannot flush this store's pending callbacks.
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
      await fixture.close();
    }
  }, timeout: const Timeout(Duration(seconds: 60)));
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final name in ['Inter', 'SpaceGrotesk']) {
      await (FontLoader(
        name,
      )..addFont(rootBundle.load('assets/fonts/$name.ttf'))).load();
    }
  });

  _testTransferWidgets(
    'downloads remain distinct from installed games at 390px and enlarged text',
    (tester, setupData) async {
      tester.view.physicalSize = const Size(390, 900);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(app(setupData.controller, scale: 1.6));
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
      expect(
        find.text('Downloaded and verified; not installed.'),
        findsOneWidget,
      );
      expect(
        find.text('Downloaded files are kept separate from installed games.'),
        findsOneWidget,
      );
      expect(find.text('Launch'), findsNothing);
      final context = tester.element(find.byType(ManagedTransferPanel));
      expect(Theme.of(context).scaffoldBackgroundColor, Colors.black);
      expect(Theme.of(context).colorScheme.primary, Colors.white);
      for (final element in find.byType(RyhzeControl).evaluate()) {
        final control = find.byWidget(element.widget);
        expect(tester.getSize(control).height, greaterThanOrEqualTo(48));
        final rect = tester.getRect(control);
        expect(rect.left, greaterThanOrEqualTo(0));
        expect(rect.right, lessThanOrEqualTo(390));
      }
      final stage = tester.widget<RyhzeControl>(
        find.widgetWithText(RyhzeControl, 'Stage files'),
      );
      expect(stage.onPressed, isNull); // No staging integration was supplied.
      await capture(tester, 'narrow-large-text');
    },
  );

  _testTransferWidgets(
    'keyboard focus and semantic activation inspect without starting a transfer',
    (tester, data) async {
      await tester.pumpWidget(app(data.controller));
      final inspect = find.widgetWithText(RyhzeControl, 'Inspect');
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.pumpAndSettle();
      expect(
        FocusManager.instance.primaryFocus?.context
            ?.findAncestorWidgetOfExactType<RyhzeControl>()
            ?.label,
        'Inspect download test-package',
      );
      final semantic = tester.getSemantics(inspect).getSemanticsData();
      expect(semantic.label, contains('Inspect download test-package'));
      expect(semantic.hasAction(ui.SemanticsAction.tap), isTrue);
      final paint =
          tester
                  .widget<AnimatedContainer>(
                    find.descendant(
                      of: inspect,
                      matching: find.byType(AnimatedContainer),
                    ),
                  )
                  .decoration!
              as BoxDecoration;
      expect(paint.border!.top.color, Colors.white);
      await capture(tester, 'keyboard-focus');
      final begins = data.fixture.backend.begins;
      final inspections = data.fixture.backend.inspections;
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await tester.pumpAndSettle();
      expect(data.fixture.backend.inspections, inspections + 1);
      expect(data.fixture.backend.begins, begins);
      expect(data.fixture.requests, 0);
      expect(data.controller.failure, isNull);
    },
  );

  _testTransferWidgets(
    'removal review can be cancelled and old account confirmation cannot remove files',
    (tester, data) async {
      await tester.pumpWidget(app(data.controller));
      await tester.tap(find.widgetWithText(RyhzeControl, 'Review removal'));
      await tester.pumpAndSettle();
      expect(
        find.text(
          'Remove these downloaded files? Your installed games and other files will be kept.',
        ),
        findsOneWidget,
      );
      expect(data.fixture.backend.discarded, isEmpty);
      await capture(tester, 'removal-review');
      await tester.tap(find.widgetWithText(RyhzeControl, 'Keep files'));
      await tester.pumpAndSettle();
      expect(data.controller.removalId, isNull);
      expect(data.fixture.backend.discarded, isEmpty);
      await tester.tap(find.widgetWithText(RyhzeControl, 'Review removal'));
      await tester.pumpAndSettle();
      expect(data.controller.removalId, data.fixture.intent.operationId);
      data.fixture.session.replace(
        const AccountScope.verified('test-only-issuer', 'OtherAccount'),
      );
      await tester.pumpAndSettle();
      expect(find.text('Downloads'), findsNothing);
      expect(data.controller.removalId, isNull);
      await data.controller.confirmRemoval();
      expect(data.fixture.backend.discarded, isEmpty);
      expect(data.controller.failure?.kind, FailureKind.conflict);
    },
  );

  _testTransferWidgets(
    'removal confirmation rejects a changed record revision',
    (tester, data) async {
      await tester.pumpWidget(app(data.controller));
      await data.controller.reviewRemoval(data.fixture.intent.operationId);
      final lease = data.fixture.repository.leaseCurrent(
        data.fixture.intent.operationId,
      )!;
      await data.fixture.repository.observe(
        lease,
        lease.record.request,
        CancellationToken(),
        complete: true,
      );
      await data.controller.confirmRemoval();
      await tester.pumpAndSettle();
      expect(data.controller.failure?.kind, FailureKind.conflict);
      expect(data.fixture.backend.discarded, isEmpty);
      expect(
        data.fixture.repository.readCurrent().single.state,
        ManagedTransferState.downloaded,
      );
    },
  );

  _testTransferWidgets(
    'cancellation remains visible until actual inspection settlement',
    (tester, data) async {
      await tester.pumpWidget(app(data.controller));
      final gate = data.fixture.backend.drainGate = Completer<void>();
      final work = data.controller.inspect(data.fixture.intent.operationId);
      await tester.pump();
      expect(data.controller.busy, isTrue);
      await tester.tap(find.widgetWithText(RyhzeControl, 'Cancel'));
      await tester.pump();
      expect(
        find.text('Cancelling; waiting for file work to finish.'),
        findsOneWidget,
      );
      expect(data.controller.busy, isTrue);
      expect(
        tester
            .widget<RyhzeControl>(find.widgetWithText(RyhzeControl, 'Cancel'))
            .onPressed,
        isNull,
      );
      gate.complete();
      await work;
      await tester.pumpAndSettle();
      expect(data.controller.busy, isFalse);
      expect(data.controller.failure?.kind, FailureKind.cancelled);
      expect(data.fixture.backend.discarded, isEmpty);
    },
  );

  _testTransferWidgets(
    'incoming source removal is disabled until matching staging finishes',
    (tester, data) async {
      final repository = ManagedInstallationRepository(
        data.fixture.store,
        data.fixture.session,
        deviceId: data.fixture.backend.deviceId,
      );
      final staging = ManagedContentService(
        repository,
        _NoStagingEffects(data.fixture.backend.deviceId),
        data.fixture.jobs,
        data.fixture.session,
      );
      final controller = ManagedTransferController(
        service: data.fixture.service,
        coordinator: data.fixture.coordinator,
        session: data.fixture.session,
        staging: staging,
      );
      try {
        final lease = data.fixture.repository.leaseCurrent(
          data.fixture.intent.operationId,
        )!;
        final stage = await repository.prepareStage(
          lease.record.stagingIntent,
          ManagedStageObservation(
            operationId: lease.record.operationId,
            state: ManagedStageObservationState.checkpoint,
            rootIdentity: lease.record.request.rootIdentity,
            sourceIdentity: lease.record.request.sourceIdentity,
            verifiedBytes: 0,
            verifiedChunks: 0,
          ),
          CancellationToken(),
          jobs: data.fixture.jobs,
          checkAdmission: () {},
          manifest: lease.record.manifest,
          transferRevision: lease.record.revision,
        );
        await tester.pumpWidget(app(controller));
        expect(
          find.text('Sent to local staging; not installed.'),
          findsOneWidget,
        );
        final remove = find.widgetWithText(RyhzeControl, 'Review removal');
        expect(tester.widget<RyhzeControl>(remove).onPressed, isNull);
        final active = await repository.beginStage(
          stage.lease,
          CancellationToken(),
          checkAuthority: () {},
        );
        await repository.reconcile(
          active,
          ManagedStageObservation(
            operationId: active.record.operationId,
            state: ManagedStageObservationState.staged,
            rootIdentity: active.record.rootIdentity,
            sourceIdentity: active.record.sourceIdentity,
            verifiedBytes: 128,
            verifiedChunks: 1,
          ),
          CancellationToken(),
        );
        // The controller listens to the separate installation repository.
        await tester.pumpAndSettle();
        expect(
          data.fixture.repository.canDiscard(
            data.fixture.repository.readCurrent().single,
          ),
          isTrue,
        );
        expect(tester.widget<RyhzeControl>(remove).onPressed, isNotNull);
        expect(data.fixture.backend.discarded, isEmpty);
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        controller.dispose();
        await staging.close();
        repository.dispose();
      }
    },
  );
}
