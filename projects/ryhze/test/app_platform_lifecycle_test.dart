import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';

/// Injected job fixture; production provider capabilities remain unchanged.
class _HeldFirstOperation implements JobOperation {
  final started = Completer<void>();
  final release = Completer<void>();
  late CancellationToken firstCancellation;
  int executions = 0;

  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken token,
    Future<void> Function(int) checkpoint,
  ) async {
    executions++;
    await checkpoint(1);
    if (executions == 1) {
      firstCancellation = token;
      started.complete();
      await release.future;
    }
    await checkpoint(2);
    return JobOutcome.completed;
  }

  @override
  Future<JobOutcome> reconcile(JobRecord job, CancellationToken token) async =>
      JobOutcome.unknown;
}

void main() {
  for (final interruption in [
    AppLifecycleState.paused,
    AppLifecycleState.detached,
  ]) {
    testWidgets(
      'non-Apple ${interruption.name}/resume cancels active work without locking the queue',
      (tester) async {
        final operation = _HeldFirstOperation();
        final services = AppServices(
          storage: MemoryTransactionStore(),
          jobOperations: {'fixture': operation},
        );
        addTearDown(() {
          if (!operation.release.isCompleted) operation.release.complete();
          services.dispose();
        });
        tester.binding.handleAppLifecycleStateChanged(
          AppLifecycleState.resumed,
        );
        await tester.pumpWidget(RyhzeApp(services: services));
        await tester.pumpAndSettle();
        final scope = services.session.scope;
        final first = await services.jobs.enqueue(scope, 'fixture', 'first');
        final second = await services.jobs.enqueue(scope, 'fixture', 'second');
        final drain = services.coordinator.drain(scope);
        await operation.started.future;

        for (final state in [
          AppLifecycleState.inactive,
          AppLifecycleState.hidden,
          AppLifecycleState.paused,
          if (interruption == AppLifecycleState.detached)
            AppLifecycleState.detached,
        ]) {
          tester.binding.handleAppLifecycleStateChanged(state);
        }
        expect(operation.firstCancellation.isCancelled, isTrue);
        // A paused view returns through hidden/inactive; a detached view may
        // attach directly into resumed. Mirror Flutter's lifecycle graph.
        for (final state in [
          if (interruption == AppLifecycleState.paused) ...[
            AppLifecycleState.hidden,
            AppLifecycleState.inactive,
          ],
          AppLifecycleState.resumed,
        ]) {
          tester.binding.handleAppLifecycleStateChanged(state);
        }
        await tester.pump();
        operation.release.complete();
        await drain;
        expect(
          services.jobs
              .read(scope)
              .singleWhere((job) => job.key == first.key)
              .state,
          JobState.recoveryRequired,
        );
        expect(
          services.jobs
              .read(scope)
              .singleWhere((job) => job.key == second.key)
              .state,
          JobState.queued,
        );

        await services.coordinator.drain(scope);
        expect(
          services.jobs
              .read(scope)
              .singleWhere((job) => job.key == second.key)
              .state,
          JobState.completed,
        );
        expect(operation.executions, 2);
        expect(services.lifecycleFailure, isNull);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        await services.close();
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.linux,
        TargetPlatform.windows,
      }),
    );
  }

  testWidgets(
    'Windows focus and visibility changes keep active and queued jobs usable',
    (tester) async {
      final operation = _HeldFirstOperation();
      final services = AppServices(
        storage: MemoryTransactionStore(),
        jobOperations: {'fixture': operation},
      );
      addTearDown(() {
        if (!operation.release.isCompleted) operation.release.complete();
        services.dispose();
      });
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pumpWidget(RyhzeApp(services: services));
      await tester.pumpAndSettle();
      final scope = services.session.scope;
      await services.jobs.enqueue(scope, 'fixture', 'first');
      await services.jobs.enqueue(scope, 'fixture', 'second');
      final drain = services.coordinator.drain(scope);
      await operation.started.future;

      for (final state in [
        AppLifecycleState.inactive,
        AppLifecycleState.hidden,
        AppLifecycleState.inactive,
        AppLifecycleState.resumed,
      ]) {
        tester.binding.handleAppLifecycleStateChanged(state);
        expect(operation.firstCancellation.isCancelled, isFalse);
      }
      await tester.pump();
      operation.release.complete();
      await drain;
      expect(
        services.jobs
            .read(scope)
            .every((job) => job.state == JobState.completed),
        isTrue,
      );
      expect(operation.executions, 2);
      expect(services.lifecycleFailure, isNull);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await services.close();
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
}
