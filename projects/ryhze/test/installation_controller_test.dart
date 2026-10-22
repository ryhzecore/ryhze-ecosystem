import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/installation_controller.dart';
import 'package:ryhze/features/installations/installation_repository.dart';
import 'package:ryhze/features/installations/installations_panel.dart';

class _Inspection implements InstallationRepository {
  final pending = <Completer<RepositoryRead<InstallationObservation>>>[];
  final tokens = <CancellationToken>[];
  @override
  Future<RepositoryRead<InstallationObservation>> inspect(
    CancellationToken token,
  ) {
    tokens.add(token);
    final request = Completer<RepositoryRead<InstallationObservation>>();
    pending.add(request);
    return request.future;
  }
}

RepositoryRead<InstallationObservation> result({bool empty = false}) =>
    RepositoryRead(
      empty
          ? []
          : [
              InstallationObservation(
                item: ProviderItemRef('steam', '123'),
                title: 'Synthetic local record',
                libraryRoot: r'C:\SyntheticLibrary',
                relativeDirectory: r'steamapps\common\Synthetic',
                providerState: '4',
              ),
            ],
      quality: ReadQuality.complete,
      observedAt: DateTime.utc(2026, 10, 10),
    );

void main() {
  test(
    'scope clear settles throwing cancellation before admitting more work',
    () async {
      final repository = _Inspection();
      final controller = InstallationController()..attach(repository);
      addTearDown(controller.dispose);
      var request = controller.inspect();
      repository.pending.last.complete(result());
      await request;
      request = controller.inspect();
      controller.failure = const AppFailure(FailureKind.network);
      final expected = StateError('synthetic cancellation failure');
      Future<void>? reentrant;
      var laterCalled = false;
      repository.tokens.last.onCancel(() {
        expect(controller.busy, isFalse);
        expect(controller.read, isNull);
        expect(controller.failure, isNull);
        reentrant = controller.inspect();
        throw expected;
      });
      repository.tokens.last.onCancel(() => laterCalled = true);
      expect(() => controller.cancel(clear: true), throwsA(same(expected)));
      await reentrant;
      expect(laterCalled, isTrue);
      expect(repository.pending, hasLength(2));
      expect(controller.busy, isFalse);
      final next = controller.inspect();
      expect(repository.pending, hasLength(3));
      repository.pending[1].complete(result());
      await request;
      expect(controller.busy, isTrue);
      expect(controller.read, isNull);
      final newest = result(empty: true);
      repository.pending[2].complete(newest);
      await next;
      expect(controller.read, same(newest));
    },
  );

  test(
    'interrupt publishes stale state even when cancellation throws',
    () async {
      final repository = _Inspection();
      final controller = InstallationController()..attach(repository);
      addTearDown(controller.dispose);
      var request = controller.inspect();
      final original = result();
      repository.pending.last.complete(original);
      await request;
      request = controller.inspect();
      final expected = StateError('synthetic interruption failure');
      Future<void>? reentrant;
      repository.tokens.last.onCancel(() {
        expect(controller.busy, isFalse);
        expect(controller.read!.quality, ReadQuality.stale);
        expect(controller.read!.observedAt, original.observedAt);
        reentrant = controller.inspect();
        throw expected;
      });
      expect(controller.interrupt, throwsA(same(expected)));
      await reentrant;
      final stale = controller.read;
      repository.pending.last.complete(result(empty: true));
      await request;
      expect(controller.read, same(stale));
      expect(repository.pending, hasLength(2));
    },
  );

  test(
    'newer reentrant repository attachment wins after throwing cleanup',
    () async {
      final original = _Inspection();
      final replaced = _Inspection();
      final newest = _Inspection();
      final controller = InstallationController()..attach(original);
      addTearDown(controller.dispose);
      final request = controller.inspect();
      final expected = StateError('synthetic replacement failure');
      Future<void>? reentrant;
      var notifications = 0;
      controller.addListener(() => notifications++);
      original.tokens.single.onCancel(() {
        controller.attach(newest);
        controller.notifyListeners();
        reentrant = controller.inspect();
        throw expected;
      });
      expect(() => controller.attach(replaced), throwsA(same(expected)));
      await reentrant;
      expect(notifications, 1);
      expect(replaced.pending, isEmpty);
      expect(newest.pending, isEmpty);
      original.pending.single.complete(result());
      await request;
      expect(controller.read, isNull);
      final next = controller.inspect();
      expect(newest.pending, hasLength(1));
      newest.pending.single.complete(result(empty: true));
      await next;
      expect(controller.read!.values, isEmpty);
    },
  );

  test(
    'throwing disposal releases notifier and ignores late inspection',
    () async {
      final repository = _Inspection();
      final controller = InstallationController()..attach(repository);
      final request = controller.inspect();
      final expected = StateError('synthetic disposal failure');
      var notified = 0;
      controller.addListener(() => notified++);
      repository.tokens.single.onCancel(() {
        expect(controller.busy, isFalse);
        controller.dispose();
        throw expected;
      });
      expect(controller.dispose, throwsA(same(expected)));
      expect(controller.dispose, returnsNormally);
      expect(() => controller.addListener(() {}), throwsFlutterError);
      await controller.inspect();
      controller.attach(_Inspection());
      expect(controller.available, isFalse);
      repository.pending.single.complete(result());
      await request;
      expect(controller.read, isNull);
      expect(notified, 0);
    },
  );

  test(
    'disposal during initial notification never starts inspection',
    () async {
      final repository = _Inspection();
      final controller = InstallationController()..attach(repository);
      var laterCalled = false;
      controller.addListener(controller.dispose);
      controller.addListener(() {
        laterCalled = true;
        expect(controller.busy, isFalse);
      });
      await controller.inspect();
      expect(laterCalled, isTrue);
      expect(repository.pending, isEmpty);
      expect(controller.dispose, returnsNormally);
      expect(() => controller.addListener(() {}), throwsFlutterError);
    },
  );

  test(
    'inspection deduplicates, retains stale results and cancels replaced scope',
    () async {
      final repository = _Inspection();
      final controller = InstallationController()..attach(repository);
      var request = controller.inspect();
      await controller.inspect();
      expect(repository.pending, hasLength(1));
      repository.pending.last.complete(result());
      await request;
      request = controller.inspect();
      repository.pending.last.completeError(
        const AppFailure(FailureKind.denied),
      );
      await request;
      expect(controller.read!.quality, ReadQuality.stale);
      expect(controller.read!.values.single.title, 'Synthetic local record');
      request = controller.inspect();
      controller.cancel(clear: true);
      expect(repository.tokens.last.isCancelled, isTrue);
      repository.pending.last.complete(result());
      await request;
      expect(controller.read, isNull);
      request = controller.inspect();
      repository.pending.last.complete(result(empty: true));
      await request;
      expect(controller.read!.values, isEmpty);
      controller.dispose();
    },
  );
  test('lifecycle interruption preserves stale observations until explicit refresh', () async {
    final repository = _Inspection();
    final controller = InstallationController()..attach(repository);
    final original = result();
    var request = controller.inspect();
    repository.pending.last.complete(original);
    await request;

    request = controller.inspect();
    controller.interrupt();
    expect(repository.tokens.last.isCancelled, isTrue);
    expect(controller.busy, isFalse);
    expect(controller.read!.quality, ReadQuality.stale);
    expect(controller.read!.values, orderedEquals(original.values));
    expect(controller.read!.observedAt, original.observedAt);
    final interrupted = controller.read;
    repository.pending.last.complete(result(empty: true));
    await request;
    expect(controller.read, same(interrupted));
    expect(repository.pending, hasLength(2));

    request = controller.inspect();
    final refreshed = RepositoryRead<InstallationObservation>(
      [],
      quality: ReadQuality.complete,
      observedAt: original.observedAt.add(const Duration(minutes: 1)),
    );
    repository.pending.last.complete(refreshed);
    await request;
    expect(controller.read, same(refreshed));
    expect(controller.read!.quality, ReadQuality.complete);
    expect(controller.read!.values, isEmpty);
    expect(controller.failure, isNull);
    controller.dispose();
  });
  testWidgets(
    'device inspection is explicit and shows no Play or ownership action',
    (tester) async {
      final repository = _Inspection();
      final controller = InstallationController()..attach(repository);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SingleChildScrollView(
              child: InstallationPanel(controller: controller),
            ),
          ),
        ),
      );
      expect(repository.pending, isEmpty);
      await tester.tap(find.text('Inspect Steam installations'));
      await tester.pump();
      expect(find.text('Cancel'), findsOneWidget);
      repository.pending.single.complete(result());
      await tester.pumpAndSettle();
      expect(find.text('Synthetic local record'), findsOneWidget);
      expect(find.textContaining('Ownership not verified'), findsOneWidget);
      expect(find.text('Play'), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      controller.dispose();
    },
  );
}
