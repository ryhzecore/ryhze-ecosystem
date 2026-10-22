import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/app/navigation_controller.dart';
import 'package:ryhze/features/installations/content_discard_backend.dart';
import 'package:ryhze/features/installations/managed_content_discard_service.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/managed_content_controller.dart';
import 'package:ryhze/features/installations/managed_content_panel.dart';
import 'package:ryhze/features/installations/managed_content_service.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_installation_repository.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/session/session_controller.dart';

class _Store extends MemoryTransactionStore {
  @override
  bool get durable => true;
}

class _Backend implements ContentStagingBackend {
  @override
  final deviceId = '${'1' * 16}:${'2' * 32}';
  final source = '${'3' * 16}:${'4' * 32}';
  Completer<void>? hold;
  CancellationToken? token;
  bool fail = false;
  int inspections = 0, stages = 0;
  bool checkpoint = false,
      allowStage = false,
      stagePending = false,
      failStage = false;
  Completer<void>? stageHold;
  @override
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    inspections++;
    token = cancellation;
    await hold?.future;
    cancellation.check();
    if (fail) throw const AppFailure(FailureKind.storage);
    return ContentStagingReceipt(
      state: checkpoint
          ? ContentStagingState.checkpoint
          : ContentStagingState.staged,
      verifiedBytes: checkpoint ? 0 : request.size,
      verifiedChunks: checkpoint ? 0 : 1,
      rootIdentity: deviceId,
      sourceIdentity: source,
    );
  }

  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    if (!allowStage) throw StateError('Inspection UI must never stage');
    stages++;
    token = cancellation;
    stagePending = true;
    try {
      await stageHold?.future;
      cancellation.check();
      if (failStage) throw const AppFailure(FailureKind.storage);
      checkpoint = false;
      return ContentStagingReceipt(
        state: ContentStagingState.staged,
        verifiedBytes: request.size,
        verifiedChunks: 1,
        rootIdentity: deviceId,
        sourceIdentity: source,
      );
    } finally {
      stagePending = false;
    }
  }

  @override
  Future<void> cancel() async {}
  @override
  Future<void> drain() async {
    await hold?.future;
    if (stagePending) await stageHold?.future;
  }

  @override
  Future<void> close() async {
    await drain();
  }
}

class _DiscardBackend implements ContentDiscardBackend {
  @override
  bool get discardAvailable => true;
  int plans = 0, removals = 0;
  bool refuse = false;
  Completer<void>? hold;
  CancellationToken? token;
  Future<void>? _removalSettlement;
  @override
  Future<ContentDiscardPlanReceipt> planDiscard(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    plans++;
    return ContentDiscardPlanReceipt(
      state: refuse ? ContentStagingState.recovery : ContentStagingState.staged,
      plan: refuse ? null : Uint8List.fromList([1, 2, 3]),
      verifiedBytes: request.size,
      verifiedChunks: 1,
    );
  }

  @override
  Future<ContentDiscardReceipt> discard(
    Uint8List plan,
    CancellationToken cancellation,
  ) async {
    removals++;
    token = cancellation;
    final settlement = _removalSettlement =
        hold?.future ?? Future<void>.value();
    try {
      await settlement;
      cancellation.check();
      return const ContentDiscardReceipt(
        state: ContentDiscardState.completed,
        removedBytes: 3,
        removedFiles: 2,
      );
    } finally {
      if (identical(_removalSettlement, settlement)) _removalSettlement = null;
    }
  }

  @override
  Future<ContentDiscardReceipt> inspectDiscard(
    Uint8List plan,
    CancellationToken cancellation,
  ) async => const ContentDiscardReceipt(
    state: ContentDiscardState.ready,
    removedBytes: 0,
    removedFiles: 0,
  );
  @override
  Future<void> cancel() async {}
  @override
  Future<void> drain() async {
    await _removalSettlement;
  }

  @override
  Future<void> close() => drain();
}

class _CountingRepository extends ManagedInstallationRepository {
  _CountingRepository(super.store, super.session, {required super.deviceId});
  int projectionReads = 0;
  @override
  List<ManagedInstallationRecord> readCurrent() {
    projectionReads++;
    return super.readCurrent();
  }
}

class _Fixture {
  _Fixture({this.withDiscard = false});
  final bool withDiscard;
  final discardBackend = _DiscardBackend();
  final overlays = OverlayCoordinator();
  final store = _Store();
  final session = SessionController();
  final backend = _Backend();
  late final repository = _CountingRepository(
    store,
    session,
    deviceId: backend.deviceId,
  );
  late final jobs = JobRepository(store);
  late final service = ManagedContentService(
    repository,
    backend,
    jobs,
    session,
  );
  late final discardService = ManagedContentDiscardService(
    repository,
    discardBackend,
    jobs,
    session,
  );
  late final coordinator = JobCoordinator(jobs, {
    ManagedContentService.jobKind: service,
    if (withDiscard) ManagedContentDiscardService.jobKind: discardService,
  });
  late final controller = ManagedContentController(
    repository: repository,
    service: service,
    session: session,
    coordinator: coordinator,
    discardService: withDiscard ? discardService : null,
  );
  Future<String> seed(
    int index, {
    bool cancelled = false,
    bool recovery = false,
    String? package,
  }) async {
    final id = index.toRadixString(16).padLeft(64, '0');
    final intent = ManagedStageIntent(
      operationId: id,
      item: ProviderItemRef('fixture', 'item-$index'),
      packageId: package ?? 'Package $index',
      version: '1',
      manifestSha256: 'b' * 64,
      sourceRelative: 'fixture.bin',
      expectedSize: 3,
      expectedSha256: 'c' * 64,
      slot: 0,
    );
    var lease = await repository.prepare(
      intent,
      ManagedStageObservation(
        operationId: id,
        state: ManagedStageObservationState.checkpoint,
        verifiedBytes: 0,
        verifiedChunks: 0,
        rootIdentity: backend.deviceId,
        sourceIdentity: backend.source,
      ),
      CancellationToken(),
    );
    if (cancelled) {
      await repository.cancelPrepared(lease, CancellationToken());
    } else {
      lease = await repository.beginStage(lease, CancellationToken());
      await repository.reconcile(
        lease,
        ManagedStageObservation(
          operationId: id,
          state: recovery
              ? ManagedStageObservationState.checkpoint
              : ManagedStageObservationState.staged,
          verifiedBytes: recovery ? 0 : 3,
          verifiedChunks: recovery ? 0 : 1,
          rootIdentity: backend.deviceId,
          sourceIdentity: backend.source,
        ),
        CancellationToken(),
      );
    }
    return id;
  }

  Future<void> dispose() async {
    controller.dispose();
    await coordinator.close();
    await service.close();
    if (withDiscard) await discardService.close();
    overlays.dispose();
    repository.dispose();
    jobs.dispose();
    session.dispose();
    await store.close();
  }
}

Widget _app(
  ManagedContentController controller, {
  double scale = 1,
  OverlayCoordinator? overlays,
}) => MaterialApp(
  theme: RyhzeTokens.theme,
  builder: (context, child) => MediaQuery(
    data: MediaQuery.of(context).copyWith(textScaler: TextScaler.linear(scale)),
    child: child!,
  ),
  home: Scaffold(
    body: SingleChildScrollView(
      child: SizedBox(
        width: 360,
        child: ManagedContentPanel(controller: controller, overlays: overlays),
      ),
    ),
  ),
);

Future<void> _capture(
  WidgetTester tester,
  GlobalKey boundaryKey,
  String name,
) async {
  final directory = Platform.environment['RYHZE_MANAGED_PREVIEW_DIR'];
  if (directory == null || directory.isEmpty) return;
  final boundary =
      boundaryKey.currentContext!.findRenderObject()! as RenderRepaintBoundary;
  await tester.runAsync(() async {
    final image = await boundary.toImage(pixelRatio: 1);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      if (bytes == null) throw StateError('Preview encoding failed');
      await Directory(directory).create(recursive: true);
      await File('$directory${Platform.pathSeparator}$name.png').writeAsBytes(
        bytes.buffer.asUint8List(bytes.offsetInBytes, bytes.lengthInBytes),
        flush: true,
      );
    } finally {
      image.dispose();
    }
  });
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final font in ['Inter', 'SpaceGrotesk']) {
      await (FontLoader(
        font,
      )..addFont(rootBundle.load('assets/fonts/$font.ttf'))).load();
    }
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  testWidgets(
    'known parent overlay keeps its ownership while child confirmation closes',
    (tester) async {
      final fixture = _Fixture(withDiscard: true);
      await fixture.seed(1);
      final parentDone = Completer<void>();
      final parent = fixture.overlays.run<void>(() => parentDone.future);
      await tester.pumpWidget(
        MaterialApp(
          theme: RyhzeTokens.theme,
          home: Scaffold(
            body: ManagedContentPanel(
              controller: fixture.controller,
              overlays: fixture.overlays,
              parentOwnsOverlay: true,
            ),
          ),
        ),
      );
      await tester.tap(find.widgetWithText(RyhzeControl, 'Review removal'));
      await tester.pumpAndSettle();
      expect(find.text('Remove this retained staging copy?'), findsOneWidget);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(find.text('Remove this retained staging copy?'), findsNothing);
      expect(fixture.overlays.isOpen, isTrue);
      expect(fixture.discardBackend.removals, 0);
      parentDone.complete();
      await parent;
      await tester.pumpWidget(const SizedBox());
      await fixture.dispose();
    },
  );
  testWidgets(
    'owner lifecycle dismissal closes removal dialog without effects',
    (tester) async {
      final fixture = _Fixture(withDiscard: true);
      await fixture.seed(1);
      await tester.pumpWidget(
        _app(fixture.controller, overlays: fixture.overlays),
      );
      final review = find.widgetWithText(RyhzeControl, 'Review removal');
      await tester.ensureVisible(review);
      await tester.pumpAndSettle();
      await tester.tap(review);
      await tester.pumpAndSettle();
      expect(find.text('Remove this retained staging copy?'), findsOneWidget);
      final staleConfirmation = fixture.controller.removalConfirmation!;
      // The application owner dismisses presentation before suspending authority.
      fixture.controller.dismissRemoval();
      fixture.discardService.interrupt();
      await tester.pumpAndSettle();
      expect(find.text('Remove this retained staging copy?'), findsNothing);
      expect(fixture.overlays.isOpen, isFalse);
      expect(fixture.controller.removalConfirmation, isNull);
      await fixture.controller.confirmRemoval(staleConfirmation);
      expect(fixture.discardBackend.removals, 0);
      fixture.discardService.resume();
      await tester.ensureVisible(review);
      await tester.pumpAndSettle();
      await tester.tap(review);
      await tester.pumpAndSettle();
      expect(find.text('Remove this retained staging copy?'), findsOneWidget);
      expect(
        identical(fixture.controller.removalConfirmation, staleConfirmation),
        isFalse,
      );
      expect(fixture.discardBackend.plans, 2);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(fixture.discardBackend.removals, 0);
      await tester.pumpWidget(const SizedBox());
      await fixture.dispose();
    },
  );
  for (final layout in [
    (1.0, const Size(360, 1000)),
    (2.0, const Size(360, 1000)),
    (1.0, const Size(800, 600)),
    (2.0, const Size(360, 600)),
  ]) {
    final scale = layout.$1;
    testWidgets(
      'removal confirmation defaults to Cancel and invalidates scope at text $scale size ${layout.$2}',
      (tester) async {
        final fixture = _Fixture(withDiscard: true);
        await fixture.seed(1);
        tester.view.physicalSize = layout.$2;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final boundary = GlobalKey();
        await tester.pumpWidget(
          RepaintBoundary(
            key: boundary,
            child: _app(
              fixture.controller,
              scale: scale,
              overlays: fixture.overlays,
            ),
          ),
        );
        final review = find.widgetWithText(RyhzeControl, 'Review removal');
        await tester.ensureVisible(review);
        await tester.pumpAndSettle();
        await tester.tap(review);
        await tester.pumpAndSettle();
        expect(find.text('Remove this retained staging copy?'), findsOneWidget);
        expect(fixture.discardBackend.removals, 0);
        expect(
          FocusManager.instance.primaryFocus?.context
              ?.findAncestorWidgetOfExactType<RyhzeControl>()
              ?.label,
          'Cancel removal',
        );
        expect(fixture.overlays.isOpen, isTrue);
        final cancel = find.widgetWithText(RyhzeControl, 'Cancel');
        final remove = find.widgetWithText(RyhzeControl, 'Remove staging copy');
        void expectVisibleActions() {
          final dialog = tester.getRect(find.byType(Dialog));
          final viewport = Offset.zero & layout.$2;
          for (final control in [cancel, remove]) {
            final rect = tester.getRect(control);
            expect(rect.height, greaterThanOrEqualTo(48));
            expect(dialog.intersect(rect), rect);
            expect(viewport.intersect(rect), rect);
            expect(control.hitTestable(), findsOneWidget);
          }
        }

        expectVisibleActions();
        await tester.sendKeyEvent(LogicalKeyboardKey.tab);
        await tester.pumpAndSettle();
        expect(
          FocusManager.instance.primaryFocus?.context
              ?.findAncestorWidgetOfExactType<RyhzeControl>()
              ?.label,
          'Remove staging copy',
        );
        expectVisibleActions();
        final focusedDecoration =
            tester
                    .widget<AnimatedContainer>(
                      find.descendant(
                        of: remove,
                        matching: find.byType(AnimatedContainer),
                      ),
                    )
                    .decoration!
                as BoxDecoration;
        expect(focusedDecoration.border!.top.color, RyhzeTokens.accent);
        if (scale == 2 && layout.$2.height == 600) {
          await tester.sendKeyEvent(LogicalKeyboardKey.tab);
          await tester.pumpAndSettle();
          expect(
            FocusManager.instance.primaryFocus?.debugLabel,
            'Removal details',
          );
          final details = find.byKey(const ValueKey('removal-details'));
          expect(
            (tester.widget<DecoratedBox>(details).decoration as BoxDecoration)
                .border!
                .top
                .color,
            RyhzeTokens.accent,
          );
          final scrolling = tester
              .widget<SingleChildScrollView>(
                find.descendant(
                  of: details,
                  matching: find.byType(SingleChildScrollView),
                ),
              )
              .controller!;
          expect(scrolling.offset, 0);
          await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
          await tester.pump();
          expect(scrolling.offset, greaterThan(0));
          for (
            var page = 0;
            page < 30 && scrolling.offset < scrolling.position.maxScrollExtent;
            page++
          ) {
            await tester.sendKeyEvent(LogicalKeyboardKey.pageDown);
            await tester.pump();
          }
          expect(scrolling.offset, scrolling.position.maxScrollExtent);
          final explanation = find.text(
            'Only the reviewed staging files will be removed. The incoming file, installed titles and history are kept. This does not uninstall a title or clear your cache.',
          );
          expect(
            tester.getRect(explanation).bottom,
            lessThanOrEqualTo(tester.getRect(details).bottom),
          );
          expectVisibleActions();
          await _capture(tester, boundary, 'discard-details-scroll-bottom');
          await tester.sendKeyEvent(LogicalKeyboardKey.home);
          await tester.pump();
          expect(scrolling.offset, 0);
          await tester.sendKeyEvent(LogicalKeyboardKey.escape);
          await tester.pumpAndSettle();
          expect(find.byType(Dialog), findsNothing);
          expect(fixture.discardBackend.removals, 0);
          await tester.ensureVisible(review);
          await tester.pumpAndSettle();
          await tester.tap(review);
          await tester.pumpAndSettle();
          await tester.sendKeyEvent(LogicalKeyboardKey.tab);
          await tester.pumpAndSettle();
        }
        await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
        await tester.sendKeyEvent(LogicalKeyboardKey.tab);
        await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
        await tester.pumpAndSettle();
        expect(
          FocusManager.instance.primaryFocus?.context
              ?.findAncestorWidgetOfExactType<RyhzeControl>()
              ?.label,
          'Cancel removal',
        );
        await _capture(
          tester,
          boundary,
          layout.$2.height == 1000
              ? 'discard-confirm-text$scale'
              : 'discard-confirm-text$scale-height${layout.$2.height}',
        );
        await tester.sendKeyEvent(LogicalKeyboardKey.enter);
        await tester.pumpAndSettle();
        expect(find.text('Remove this retained staging copy?'), findsNothing);
        expect(fixture.discardBackend.removals, 0);
        await tester.ensureVisible(review);
        await tester.pumpAndSettle();
        await tester.tap(review);
        await tester.pumpAndSettle();
        fixture.session.replace(
          const AccountScope.verified('fixture', 'other'),
        );
        await tester.pumpAndSettle();
        expect(find.text('Remove this retained staging copy?'), findsNothing);
        expect(fixture.discardBackend.removals, 0);
        expect(fixture.overlays.isOpen, isFalse);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
        await fixture.dispose();
      },
    );
  }
  testWidgets(
    'removal is explicit, duplicate safe and cancellation waits for settlement',
    (tester) async {
      final fixture = _Fixture(withDiscard: true);
      await fixture.seed(1);
      await tester.pumpWidget(
        _app(fixture.controller, overlays: fixture.overlays),
      );
      final review = find.widgetWithText(RyhzeControl, 'Review removal');
      await tester.ensureVisible(review);
      await tester.pumpAndSettle();
      await tester.tap(review);
      await tester.pumpAndSettle();
      final confirmation = fixture.controller.removalConfirmation!;
      fixture.discardBackend.hold = Completer<void>();
      final remove = find.widgetWithText(RyhzeControl, 'Remove staging copy');
      await tester.ensureVisible(remove);
      await tester.pumpAndSettle();
      await tester.tap(remove);
      await tester.pumpAndSettle();
      expect(fixture.discardBackend.removals, 1);
      await fixture.controller.confirmRemoval(confirmation);
      expect(fixture.discardBackend.removals, 1);
      fixture.controller.cancel();
      await tester.pump();
      expect(fixture.controller.busy, isTrue);
      expect(fixture.discardBackend.token!.isCancelled, isTrue);
      fixture.discardBackend.hold!.complete();
      await tester.pumpAndSettle();
      expect(fixture.controller.busy, isFalse);
      expect(fixture.controller.removalConfirmation, isNull);
      expect(find.text('Inspect removal recovery'), findsOneWidget);
      fixture.discardBackend.hold = null;
      final retry = find.widgetWithText(
        RyhzeControl,
        'Inspect removal recovery',
      );
      await tester.ensureVisible(retry);
      await tester.pumpAndSettle();
      await tester.tap(retry);
      await tester.pumpAndSettle();
      expect(find.text('Remove this retained staging copy?'), findsOneWidget);
      expect(fixture.discardBackend.removals, 1);
      await tester.ensureVisible(
        find.widgetWithText(RyhzeControl, 'Remove staging copy'),
      );
      await tester.pumpAndSettle();
      await tester.tap(
        find.widgetWithText(RyhzeControl, 'Remove staging copy'),
      );
      await tester.pumpAndSettle();
      expect(fixture.discardBackend.removals, 2);
      expect(
        fixture.controller.records.single.state,
        ManagedInstallationState.discarded,
      );
      await tester.pumpWidget(const SizedBox());
      await fixture.dispose();
    },
  );
  testWidgets(
    'ambiguous removal plan shows safe refusal without confirmation',
    (tester) async {
      final fixture = _Fixture(withDiscard: true);
      await fixture.seed(1);
      fixture.discardBackend.refuse = true;
      await tester.pumpWidget(
        _app(fixture.controller, overlays: fixture.overlays),
      );
      final review = find.widgetWithText(RyhzeControl, 'Review removal');
      await tester.ensureVisible(review);
      await tester.pumpAndSettle();
      await tester.tap(review);
      await tester.pumpAndSettle();
      expect(find.text('Remove this retained staging copy?'), findsNothing);
      expect(fixture.controller.failure, isNotNull);
      expect(fixture.discardBackend.removals, 0);
      await tester.pumpWidget(const SizedBox());
      await fixture.dispose();
    },
  );
  for (final layout in [(800.0, 1.0, 'normal'), (360.0, 2.0, 'narrow-text2')]) {
    testWidgets(
      '${layout.$3} keyboard inspection and optional visual evidence',
      (tester) async {
        tester.view.physicalSize = Size(layout.$1, 1600);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final semantics = tester.ensureSemantics();
        final fixture = _Fixture();
        final id = await fixture.seed(
          1,
          package: 'Retained local content example',
        );
        final boundary = GlobalKey();
        try {
          await tester.pumpWidget(
            RepaintBoundary(
              key: boundary,
              child: MaterialApp(
                theme: RyhzeTokens.theme,
                home: Scaffold(
                  body: MediaQuery(
                    data: MediaQueryData(
                      size: Size(layout.$1, 1600),
                      textScaler: TextScaler.linear(layout.$2),
                    ),
                    child: SingleChildScrollView(
                      child: Padding(
                        padding: const EdgeInsets.all(16),
                        child: ManagedContentPanel(
                          controller: fixture.controller,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          await _capture(tester, boundary, '${layout.$3}-staged');
          final inspect = find.widgetWithText(
            RyhzeControl,
            'Inspect retained content',
          );
          await tester.ensureVisible(inspect);
          await tester.pumpAndSettle();
          await tester.sendKeyEvent(LogicalKeyboardKey.tab);
          await tester.pumpAndSettle();
          expect(
            FocusManager.instance.primaryFocus?.context
                ?.findAncestorWidgetOfExactType<RyhzeControl>()
                ?.label,
            'Inspect retained content for Retained local content example',
          );
          final semanticData = tester.getSemantics(inspect).getSemanticsData();
          expect(semanticData.label, contains('Inspect retained content'));
          expect(semanticData.hasAction(ui.SemanticsAction.tap), isTrue);
          await _capture(tester, boundary, '${layout.$3}-keyboard-focus');
          fixture.backend.hold = Completer<void>();
          await tester.sendKeyEvent(LogicalKeyboardKey.enter);
          await tester.pump();
          expect(fixture.backend.inspections, 1);
          expect(fixture.controller.busy, isTrue);
          final cancel = find.widgetWithText(RyhzeControl, 'Cancel inspection');
          await tester.ensureVisible(cancel);
          await tester.pumpAndSettle();
          await tester.tap(cancel);
          await tester.pump();
          expect(fixture.controller.cancellationRequested, isTrue);
          await _capture(tester, boundary, '${layout.$3}-cancelling');
          fixture.backend.hold!.complete();
          await tester.pumpAndSettle();
          fixture.backend.hold = null;
          final lease = fixture.repository.leaseCurrent(id)!;
          await fixture.repository.reconcile(
            lease,
            ManagedStageObservation(
              operationId: id,
              state: ManagedStageObservationState.recovery,
              verifiedBytes: 0,
              verifiedChunks: 0,
              rootIdentity: fixture.backend.deviceId,
              sourceIdentity: fixture.backend.source,
            ),
            CancellationToken(),
          );
          await tester.pumpAndSettle();
          await _capture(tester, boundary, '${layout.$3}-recovery');
          fixture.backend.fail = true;
          await tester.ensureVisible(inspect);
          await tester.pumpAndSettle();
          await tester.tap(inspect);
          await tester.pumpAndSettle();
          expect(fixture.backend.inspections, 2);
          expect(
            find.text(const AppFailure(FailureKind.storage).message),
            findsOneWidget,
          );
          await _capture(tester, boundary, '${layout.$3}-error');
          fixture.backend.fail = false;
          await tester.tap(inspect);
          await tester.pumpAndSettle();
          expect(fixture.backend.inspections, 3);
          expect(fixture.controller.failure, isNull);
          expect(find.text('Staged locally; not installed.'), findsOneWidget);
          expect(tester.takeException(), isNull);
        } finally {
          if (fixture.backend.hold case final hold? when !hold.isCompleted) {
            hold.complete();
          }
          await tester.pumpWidget(const SizedBox());
          await fixture.dispose();
          semantics.dispose();
        }
      },
    );
  }
  test('scope replacement cancels queued copy and hides old status until settlement', () async {
    final fixture = _Fixture();
    final id = await fixture.seed(1, recovery: true);
    await fixture.repository.reconcile(
      fixture.repository.leaseCurrent(id)!,
      ManagedStageObservation(
        operationId: id,
        state: ManagedStageObservationState.recovery,
        verifiedBytes: 0,
        verifiedChunks: 0,
        rootIdentity: fixture.backend.deviceId,
        sourceIdentity: fixture.backend.source,
      ),
      CancellationToken(),
    );
    fixture.backend.checkpoint = true;
    fixture.backend.allowStage = true;
    await fixture.controller.inspect(id);
    fixture.backend.stageHold = Completer<void>();
    final work = fixture.controller.resumeRecord(id);
    for (var i = 0; i < 20 && fixture.backend.stages == 0; i++) {
      await Future<void>.delayed(Duration.zero);
    }
    expect(fixture.backend.stages, 1);
    fixture.session.replace(const AccountScope.verified('fixture', 'other'));
    expect(fixture.controller.records, isEmpty);
    expect(fixture.controller.activeInCurrentScope, isFalse);
    expect(fixture.controller.busy, isTrue);
    fixture.backend.stageHold!.complete();
    await work;
    expect(fixture.controller.busy, isFalse);
    expect(fixture.controller.failure, isNull);
    await fixture.dispose();
  });
  testWidgets(
    'explicit checkpoint resume supports keyboard, duplicate rejection, cancellation and retry errors',
    (tester) async {
      final fixture = _Fixture();
      final id = await fixture.seed(1, recovery: true);
      await fixture.repository.reconcile(
        fixture.repository.leaseCurrent(id)!,
        ManagedStageObservation(
          operationId: id,
          state: ManagedStageObservationState.recovery,
          verifiedBytes: 0,
          verifiedChunks: 0,
          rootIdentity: fixture.backend.deviceId,
          sourceIdentity: fixture.backend.source,
        ),
        CancellationToken(),
      );
      fixture.backend.checkpoint = true;
      fixture.backend.allowStage = true;
      final boundary = GlobalKey();
      await tester.pumpWidget(
        RepaintBoundary(key: boundary, child: _app(fixture.controller)),
      );
      final inspect = find.widgetWithText(
        RyhzeControl,
        'Inspect retained content',
      );
      expect(find.text('Resume local staging'), findsNothing);
      await tester.ensureVisible(inspect);
      await tester.pumpAndSettle();
      await tester.tap(inspect);
      await tester.pumpAndSettle();
      final resume = find.widgetWithText(RyhzeControl, 'Resume local staging');
      expect(resume, findsOneWidget);
      await _capture(tester, boundary, 'resume-ready');
      await tester.ensureVisible(resume);
      await tester.pumpAndSettle();
      for (var i = 0; i < 4; i++) {
        await tester.sendKeyEvent(LogicalKeyboardKey.tab);
        await tester.pumpAndSettle();
        if (FocusManager.instance.primaryFocus?.context
                ?.findAncestorWidgetOfExactType<RyhzeControl>()
                ?.label ==
            'Resume local staging for Package 1') {
          break;
        }
      }
      expect(
        FocusManager.instance.primaryFocus?.context
            ?.findAncestorWidgetOfExactType<RyhzeControl>()
            ?.label,
        'Resume local staging for Package 1',
      );
      fixture.backend.stageHold = Completer<void>();
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await tester.pumpAndSettle();
      expect(fixture.backend.stages, 1);
      expect(fixture.controller.resuming, isTrue);
      expect(find.text('Checking and resuming local staging…'), findsOneWidget);
      await _capture(tester, boundary, 'resume-working');
      await fixture.controller.resumeRecord(id);
      await fixture.controller.inspect(id);
      expect(fixture.backend.stages, 1);
      final cancel = find.widgetWithText(RyhzeControl, 'Cancel local staging');
      await tester.ensureVisible(cancel);
      await tester.pumpAndSettle();
      await tester.tap(cancel);
      await tester.pump();
      expect(fixture.controller.busy, isTrue);
      expect(
        find.text(
          'Cancelling local staging; waiting for local work to finish.',
        ),
        findsOneWidget,
      );
      await _capture(tester, boundary, 'resume-cancelling');
      fixture.backend.stageHold!.complete();
      await tester.pumpAndSettle();
      fixture.backend.stageHold = null;
      expect(fixture.controller.busy, isFalse);
      expect(find.text('Resume local staging'), findsNothing);
      // A new explicit inspection is required after cancellation.
      await tester.ensureVisible(inspect);
      await tester.pumpAndSettle();
      await tester.tap(inspect);
      await tester.pumpAndSettle();
      fixture.backend.failStage = true;
      await tester.ensureVisible(resume);
      await tester.pumpAndSettle();
      await tester.tap(resume);
      await tester.pumpAndSettle();
      expect(fixture.backend.stages, 2);
      expect(fixture.controller.failure, isNotNull);
      expect(
        fixture.repository.findCurrent(id)!.state,
        ManagedInstallationState.recoveryRequired,
      );
      expect(find.text('Staged locally; not installed.'), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await fixture.dispose();
    },
  );
  testWidgets(
    '21-record projection refreshes only for repository or scope changes',
    (tester) async {
      final fixture = _Fixture();
      for (var i = 1; i <= 21; i++) {
        await fixture.seed(i, cancelled: true);
      }
      // Retained pre-optimization getter access model for the same 21 records:
      // one panel build requested records, visible records and pagination values.
      // This measures real repository reads, not elapsed-time microbenchmarks.
      List<ManagedInstallationRecord> legacyRecords() =>
          fixture.repository.readCurrent();
      int legacyPages() => ((legacyRecords().length + 19) ~/ 20).clamp(1, 7);
      int legacyPage() => 0.clamp(0, legacyPages() - 1);
      fixture.repository.projectionReads = 0;
      expect(legacyRecords().isEmpty, isFalse);
      expect(legacyRecords().skip(legacyPage() * 20).take(20).length, 20);
      expect(legacyPages() > 1, isTrue);
      expect(legacyPage(), 0); // Previous control.
      expect('Page ${legacyPage() + 1} of ${legacyPages()}', 'Page 1 of 2');
      expect(legacyPage() + 1 == legacyPages(), isFalse); // Next control.
      final legacyBuildReads = fixture.repository.projectionReads;
      expect(legacyBuildReads, 9);
      final controller = fixture.controller;
      fixture.repository.projectionReads = 0;
      await tester.pumpWidget(_app(controller));
      await tester.pumpWidget(_app(controller));
      controller.nextPage();
      await tester.pump();
      controller.previousPage();
      await tester.pump();
      expect(
        fixture.repository.projectionReads,
        0,
        reason: 'Builds and page changes must reuse the immutable 21-record projection',
      );
      final evidenceDirectory =
          Platform.environment['RYHZE_MANAGED_PREVIEW_DIR'];
      if (evidenceDirectory != null && evidenceDirectory.isNotEmpty) {
        await tester.runAsync(() async {
          await Directory(evidenceDirectory).create(recursive: true);
          await File(
            '$evidenceDirectory${Platform.pathSeparator}projection-counts.json',
          ).writeAsString(
            jsonEncode({
              'records': 21,
              'beforeMethod':
                  'retained prior getter-access model for one panel build',
              'beforeRepositoryReads': legacyBuildReads,
              'afterMethod':
                  'actual two widget builds and next/previous page actions',
              'afterRepositoryReads': fixture.repository.projectionReads,
            }),
            flush: true,
          );
        });
      }
      fixture.session.replace(const AccountScope.verified('fixture', 'other'));
      await tester.pump();
      expect(fixture.repository.projectionReads, 1);
      expect(controller.records, isEmpty);
      fixture.session.signOut();
      await tester.pump();
      expect(fixture.repository.projectionReads, 2);
      expect(controller.records.length, 21);
      final beforeChange = fixture.repository.projectionReads;
      await fixture.seed(22, cancelled: true);
      await tester.pump();
      expect(
        fixture.repository.projectionReads - beforeChange,
        2,
        reason: 'Prepare and cancel each publish one repository notification',
      );
      expect(controller.records.length, 22);
      expect(() => controller.records.clear(), throwsUnsupportedError);
      await tester.pumpWidget(const SizedBox());
      await fixture.dispose();
    },
  );
  test('cancel remains busy until real service settlement; late scope errors suppressed', () async {
    final fixture = _Fixture();
    final id = await fixture.seed(1);
    fixture.backend.hold = Completer<void>();
    final work = fixture.controller.inspect(id);
    await Future<void>.delayed(Duration.zero);
    fixture.controller.cancel();
    expect(fixture.controller.busy, isTrue);
    expect(fixture.controller.cancellationRequested, isTrue);
    expect(fixture.backend.token!.isCancelled, isTrue);
    fixture.session.replace(const AccountScope.verified('fixture', 'other'));
    expect(fixture.controller.records, isEmpty);
    fixture.backend.hold!.complete();
    await work;
    expect(fixture.controller.busy, isFalse);
    expect(fixture.controller.failure, isNull);
    await fixture.dispose();
  });
  test('dispose cancels inspection and suppresses late notification', () async {
    final fixture = _Fixture();
    final id = await fixture.seed(1);
    fixture.backend.hold = Completer<void>();
    final work = fixture.controller.inspect(id);
    await Future<void>.delayed(Duration.zero);
    fixture.controller.dispose();
    expect(fixture.backend.token!.isCancelled, isTrue);
    fixture.backend.hold!.complete();
    await work;
    await fixture.service.close();
    fixture.repository.dispose();
    fixture.jobs.dispose();
    fixture.session.dispose();
    await fixture.store.close();
  });
  testWidgets('empty panel has no heading or actions', (tester) async {
    final fixture = _Fixture();
    await tester.pumpWidget(_app(fixture.controller));
    expect(find.text('Retained local content'), findsNothing);
    expect(find.byType(RyhzeControl), findsNothing);
    await tester.pumpWidget(const SizedBox());
    await fixture.dispose();
  });
  testWidgets(
    'retained inspection is usable, cancel and failure are truthful at large text',
    (tester) async {
      final fixture = _Fixture();
      await fixture.seed(1, package: 'Long package identity ' * 8);
      await tester.pumpWidget(_app(fixture.controller, scale: 2));
      expect(find.text('Staged locally; not installed.'), findsOneWidget);
      final inspect = find.widgetWithText(
        RyhzeControl,
        'Inspect retained content',
      );
      await tester.ensureVisible(inspect);
      await tester.pumpAndSettle();
      expect(tester.getSize(inspect).height, greaterThanOrEqualTo(48));
      fixture.backend.hold = Completer<void>();
      await tester.tap(inspect);
      await tester.pump();
      expect(find.text('Inspecting retained content…'), findsOneWidget);
      final cancel = find.widgetWithText(RyhzeControl, 'Cancel inspection');
      await tester.ensureVisible(cancel);
      await tester.pumpAndSettle();
      await tester.tap(cancel);
      await tester.pump();
      expect(
        find.text('Cancelling inspection; waiting for local work to finish.'),
        findsOneWidget,
      );
      fixture.backend.hold!.complete();
      await tester.pumpAndSettle();
      fixture.backend.hold = null;
      fixture.backend.fail = true;
      await tester.ensureVisible(inspect);
      await tester.pumpAndSettle();
      await tester.tap(inspect);
      await tester.pumpAndSettle();
      expect(
        find.text(const AppFailure(FailureKind.storage).message),
        findsOneWidget,
      );
      expect(fixture.backend.inspections, 2);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await fixture.dispose();
    },
  );
  testWidgets('twenty row pages reset on account replacement', (tester) async {
    final fixture = _Fixture();
    for (var i = 1; i <= 21; i++) {
      await fixture.seed(i, cancelled: true);
    }
    await tester.pumpWidget(_app(fixture.controller));
    expect(find.text('Package 20'), findsOneWidget);
    expect(find.text('Package 21'), findsNothing);
    final next = find.widgetWithText(RyhzeControl, 'Next');
    await tester.ensureVisible(next);
    await tester.pumpAndSettle();
    await tester.tap(next);
    await tester.pump();
    expect(find.text('Package 21'), findsOneWidget);
    expect(find.text('Package 1'), findsNothing);
    fixture.session.replace(const AccountScope.verified('fixture', 'other'));
    await tester.pump();
    expect(find.text('Retained local content'), findsNothing);
    expect(fixture.controller.page, 0);
    fixture.session.signOut();
    await tester.pump();
    expect(find.text('Package 1'), findsOneWidget);
    expect(find.text('Package 21'), findsNothing);
    expect(find.text('Inspect retained content'), findsNothing);
    await tester.pumpWidget(const SizedBox());
    await fixture.dispose();
  });
}
