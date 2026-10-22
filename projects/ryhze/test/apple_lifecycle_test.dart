import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/activity.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';

/// An explicitly injected fixture. Production still registers no job adapter.
class _HeldOperation implements JobOperation {
  final started = Completer<void>();
  final release = Completer<void>();
  late CancellationToken cancellation;
  int executions = 0;
  int reconciliations = 0;

  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken token,
    Future<void> Function(int) checkpoint,
  ) async {
    executions++;
    cancellation = token;
    await checkpoint(1);
    started.complete();
    await release.future;
    return JobOutcome.completed;
  }

  @override
  Future<JobOutcome> reconcile(JobRecord job, CancellationToken token) async {
    reconciliations++;
    return JobOutcome.unknown;
  }
}

class _FailingStore extends MemoryTransactionStore {
  bool fail = false;
  @override
  Future<void> transact(void Function(StoreData) update) async {
    if (fail) {
      throw StateError('Synthetic raw storage detail must stay private');
    }
    await super.transact(update);
  }
}

void main() {
  test(
    'resume waits for cancellation checkpoint and never reexecutes',
    () async {
      final operation = _HeldOperation();
      final services = AppServices(
        storage: MemoryTransactionStore(),
        jobOperations: {'fixture': operation},
      );
      addTearDown(services.close);
      final scope = services.session.scope;
      await services.jobs.enqueue(scope, 'fixture', 'item');
      final drain = services.coordinator.drain(scope);
      await operation.started.future;
      final recovery = services.recoverAfterResume();
      expect(services.recoverAfterResume(), same(recovery));
      await Future<void>.delayed(Duration.zero);
      expect(operation.cancellation.isCancelled, isTrue);
      expect(services.jobs.read(scope).single.state, JobState.running);
      operation.release.complete();
      await recovery;
      await drain;
      expect(services.jobs.read(scope).single.state, JobState.recoveryRequired);
      expect(services.jobs.read(scope).single.checkpoint, 1);
      expect(operation.executions, 1);
      expect(operation.reconciliations, 0);
      expect(services.lifecycleFailure, isNull);
      expect(services.workspace.events.single.kind, ActivityKind.recovery);
      await services.recoverAfterResume();
      expect(services.workspace.events.single.count, 1);
      expect(operation.executions, 1);
    },
  );

  test('account replacement recovers the new scope after invalidating old recovery', () async {
    final operation = _HeldOperation();
    final services = AppServices(
      storage: MemoryTransactionStore(),
      jobOperations: {'fixture': operation},
    );
    addTearDown(services.close);
    final original = services.session.scope;
    const replacement = AccountScope.verified('fixture', 'account-b');
    await services.jobs.enqueue(original, 'fixture', 'original');
    final other = await services.jobs.enqueue(replacement, 'fixture', 'other');
    await services.jobs.transition(replacement, other, JobState.running);
    final drain = services.coordinator.drain(original);
    await operation.started.future;
    final recovery = services.recoverAfterResume();
    await Future<void>.delayed(Duration.zero);
    services.session.replace(replacement);
    var notifications = 0;
    services.addListener(() => notifications++);
    operation.release.complete();
    await recovery;
    await drain;
    // Scope replacement must schedule its own recovery; no second explicit
    // recoverAfterResume call is needed to reopen admission for the new account.
    await Future<void>.delayed(Duration.zero);
    expect(notifications, greaterThan(0));
    expect(
      services.jobs.read(replacement).single.state,
      JobState.recoveryRequired,
    );
    expect(services.lifecycleFailure, isNull);
    final originalEvent = services.activity.read(original).single;
    expect(originalEvent.kind, ActivityKind.recovery);
    expect(services.workspace.events.single.kind, ActivityKind.recovery);
    expect(services.workspace.events.single.id, isNot(originalEvent.id));
    expect(operation.executions, 1);
    await services.jobs.enqueue(replacement, 'unconfigured', 'admitted');
    await services.coordinator.drain(replacement);
    expect(services.jobs.read(replacement).last.state, JobState.blocked);
  });

  testWidgets(
    'iOS inactivity cancels and resume recovers after the active operation ends',
    (tester) async {
      final operation = _HeldOperation();
      final services = AppServices(
        storage: MemoryTransactionStore(),
        jobOperations: {'fixture': operation},
      );
      await tester.pumpWidget(RyhzeApp(services: services));
      await tester.pumpAndSettle();
      final scope = services.session.scope;
      await services.jobs.enqueue(scope, 'fixture', 'item');
      final drain = services.coordinator.drain(scope);
      await operation.started.future;
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      expect(operation.cancellation.isCancelled, isTrue);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pump();
      expect(services.jobs.read(scope).single.state, JobState.running);
      operation.release.complete();
      await drain;
      await tester.pumpAndSettle();
      expect(services.jobs.read(scope).single.state, JobState.recoveryRequired);
      expect(operation.executions, 1);
      expect(operation.reconciliations, 0);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await services.close();
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  testWidgets('macOS focus loss alone does not interrupt an active operation', (
    tester,
  ) async {
    final operation = _HeldOperation();
    final services = AppServices(
      storage: MemoryTransactionStore(),
      jobOperations: {'fixture': operation},
    );
    await tester.pumpWidget(RyhzeApp(services: services));
    await tester.pumpAndSettle();
    final scope = services.session.scope;
    await services.jobs.enqueue(scope, 'fixture', 'item');
    final drain = services.coordinator.drain(scope);
    await operation.started.future;
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    expect(operation.cancellation.isCancelled, isFalse);
    operation.release.complete();
    await drain;
    expect(services.jobs.read(scope).single.state, JobState.completed);
    expect(operation.executions, 1);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
    await services.close();
  }, variant: TargetPlatformVariant.only(TargetPlatform.macOS));

  testWidgets(
    'resume failure is safe, visible and retryable without replaying a job',
    (tester) async {
      final storage = _FailingStore();
      final services = AppServices(storage: storage);
      final scope = services.session.scope;
      final job = await services.jobs.enqueue(scope, 'fixture', 'interrupted');
      await services.jobs.transition(scope, job, JobState.running);
      await tester.pumpWidget(RyhzeApp(services: services));
      await tester.pumpAndSettle();
      storage.fail = true;
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pumpAndSettle();
      expect(services.lifecycleFailure?.kind, FailureKind.recovery);
      expect(find.textContaining('Synthetic raw storage'), findsNothing);
      expect(find.byKey(const Key('retry-lifecycle-recovery')), findsOneWidget);
      expect(services.jobs.read(scope).single.state, JobState.running);
      storage.fail = false;
      await tester.tap(find.byKey(const Key('retry-lifecycle-recovery')));
      await tester.pumpAndSettle();
      expect(services.lifecycleFailure, isNull);
      expect(find.byKey(const Key('retry-lifecycle-recovery')), findsNothing);
      expect(services.jobs.read(scope).single.state, JobState.recoveryRequired);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await services.close();
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );
}
