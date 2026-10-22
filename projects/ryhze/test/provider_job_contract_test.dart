import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/core/diagnostics.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/platform/desktop_contracts.dart';
import 'package:ryhze/providers/provider_registry.dart';

class TestOperation implements JobOperation {
  int executions = 0;
  int reconciliations = 0;
  bool interrupted = false;
  @override
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken cancellation,
    Future<void> Function(int) checkpoint,
  ) async {
    executions++;
    await checkpoint(10);
    if (interrupted) throw StateError('simulated interruption');
    return JobOutcome.handoffAccepted;
  }

  @override
  Future<JobOutcome> reconcile(
    JobRecord job,
    CancellationToken cancellation,
  ) async {
    reconciliations++;
    return JobOutcome.completed;
  }
}

void main() {
  final now = DateTime.utc(2026, 10, 7);
  Entitlement evidence({
    String account = 'A',
    bool fixture = false,
    DateTime? expiry,
    AccessState state = AccessState.verified,
  }) => Entitlement(
    account: account,
    item: ProviderItemRef('provider', 'Item'),
    authority: 'authority',
    observedAt: now.subtract(const Duration(hours: 1)),
    expiresAt: expiry,
    state: state,
    fixture: fixture,
  );
  test('ownership requires exact scope, authority, freshness and non-fixture evidence', () {
    bool qualifies(Entitlement value) => value.qualifies(
      accountScope: 'A',
      expectedAuthority: 'authority',
      now: now,
    );
    expect(qualifies(evidence()), isTrue);
    expect(qualifies(evidence(account: 'a')), isFalse);
    expect(qualifies(evidence(fixture: true)), isFalse);
    expect(qualifies(evidence(expiry: now)), isFalse);
    expect(qualifies(evidence(state: AccessState.revoked)), isFalse);
    expect(
      ProviderItemRef('provider', 'Item'),
      isNot(ProviderItemRef('provider', 'item')),
    );
  });
  test(
    'unknown-only differs from complete empty; duplicate identities fail',
    () {
      RepositoryRead<Entitlement> project(List<Entitlement> items) =>
          EntitlementProjection.verify(
            RepositoryRead(
              items,
              quality: ReadQuality.complete,
              observedAt: now,
            ),
            account: 'A',
            authority: 'authority',
            now: now,
          );
      expect(project([]).quality, ReadQuality.complete);
      expect(
        project([evidence(state: AccessState.unknown)]).quality,
        ReadQuality.unavailable,
      );
      expect(
        () => project([evidence(), evidence()]),
        throwsA(isA<AppFailure>()),
      );
      expect(
        () => project([evidence(account: 'B')]),
        throwsA(isA<AppFailure>()),
      );
    },
  );
  test('fixture parser refuses malformed and mixed real assertions', () {
    final parser = FixtureEntitlementAdapter();
    expect(
      () => parser.parse({
        'fixture': false,
        'complete': true,
        'records': [],
      }, now: now),
      throwsA(isA<AppFailure>()),
    );
    expect(
      () => parser.parse({
        'fixture': true,
        'complete': true,
        'records': [
          {'token': 'secret'},
        ],
      }, now: now),
      throwsA(isA<AppFailure>()),
    );
    final parsed = parser.parse({
      'fixture': true,
      'complete': false,
      'records': [
        {
          'provider': 'example',
          'item': 'one',
          'account': 'A',
          'state': 'verified',
          'observedAt': '2026-10-06T00:00:00Z',
        },
      ],
    }, now: now);
    expect(parsed.quality, ReadQuality.partial);
    expect(parsed.values.single.fixture, isTrue);
  });
  test(
    'path and URI policies reject traversal, ADS, devices and authorities',
    () {
      for (final path in [
        '../game.exe',
        r'a\..\game.exe',
        r'C:\game.exe',
        'game.exe:secret',
        'NUL.exe',
        'folder./game.exe',
        '/game.exe',
      ]) {
        expect(WindowsPathPolicy.validRelative(path), isFalse, reason: path);
      }
      expect(WindowsPathPolicy.validRelative(r'bin\game.exe'), isTrue);
      final policy = ProviderDestinationPolicy({
        'example': {'store.example'},
      });
      expect(
        policy.permits('example', Uri.parse('https://store.example/item/1')),
        isTrue,
      );
      for (final link in [
        'https://store.example.evil/item',
        'https://user@store.example/item',
        'https://store.example/item?run=cmd',
        'file:///C:/game.exe',
      ]) {
        expect(policy.permits('example', Uri.parse(link)), isFalse);
      }
    },
  );
  test(
    'durable identity deduplicates rapid requests and remains account scoped',
    () async {
      final store = MemoryTransactionStore();
      final jobs = JobRepository(store, maxPending: 1);
      const scope = AccountScope.local();
      final created = await Future.wait(
        List.generate(5, (_) => jobs.enqueue(scope, 'test', 'item')),
      );
      expect(created.map((job) => job.key).toSet(), hasLength(1));
      expect(jobs.read(scope), hasLength(1));
      await expectLater(
        jobs.enqueue(scope, 'test', 'second'),
        throwsA(isA<AppFailure>()),
      );
      final operation = TestOperation();
      final coordinator = JobCoordinator(jobs, {'test': operation});
      await Future.wait([coordinator.drain(scope), coordinator.drain(scope)]);
      expect(operation.executions, 1);
      expect(jobs.read(scope).single.state, JobState.handoffAccepted);
      await coordinator.drain(scope);
      expect(operation.executions, 1);
      expect(
        jobs.read(const AccountScope.verified('provider', 'other')),
        isEmpty,
      );
      await coordinator.close();
      await store.close();
    },
  );
  test(
    'interruption reconciles external outcome without repeating execute',
    () async {
      final store = MemoryTransactionStore();
      final jobs = JobRepository(store);
      const scope = AccountScope.local();
      await jobs.enqueue(scope, 'test', 'item');
      final operation = TestOperation()..interrupted = true;
      var coordinator = JobCoordinator(jobs, {'test': operation});
      await coordinator.drain(scope);
      expect(jobs.read(scope).single.checkpoint, 10);
      expect(jobs.read(scope).single.state, JobState.recoveryRequired);
      await coordinator.close();
      coordinator = JobCoordinator(jobs, {'test': operation});
      await coordinator.recover(scope);
      await coordinator.drain(scope);
      await coordinator.reconcile(scope, jobs.read(scope).single);
      expect(operation.executions, 1);
      expect(operation.reconciliations, 1);
      expect(jobs.read(scope).single.state, JobState.completed);
      await coordinator.close();
      await store.close();
    },
  );
  test(
    'unregistered operations are blocked without executing anything',
    () async {
      final store = MemoryTransactionStore();
      final jobs = JobRepository(store);
      await jobs.enqueue(const AccountScope.local(), 'install', 'item');
      final coordinator = JobCoordinator(jobs, {});
      await coordinator.drain(const AccountScope.local());
      expect(
        jobs.read(const AccountScope.local()).single.state,
        JobState.blocked,
      );
      await coordinator.close();
      await store.close();
    },
  );
  test('explicit later intents preserve dedupe and block unresolved same-item work', () async {
    final store = MemoryTransactionStore();
    final jobs = JobRepository(store);
    const scope = AccountScope.local();
    final first = await jobs.enqueue(
      scope,
      'fixture',
      'item',
      intentId: 'request-1',
    );
    expect(
      (await jobs.enqueue(scope, 'fixture', 'item', intentId: 'request-1')).key,
      first.key,
    );
    await expectLater(
      jobs.enqueue(scope, 'fixture', 'item', intentId: 'request-2'),
      throwsA(isA<AppFailure>()),
    );
    await jobs.transition(scope, first, JobState.running);
    await jobs.transition(scope, first, JobState.completed);
    expect(
      (await jobs.enqueue(
        scope,
        'fixture',
        'item',
        intentId: 'request-1',
      )).state,
      JobState.completed,
    );
    final second = await jobs.enqueue(
      scope,
      'fixture',
      'item',
      intentId: 'request-2',
    );
    expect(second.key, isNot(first.key));
    expect(jobs.read(scope), hasLength(2));
    await store.close();
  });
  test('stale recovery commands re-read durable state before external reconciliation', () async {
    final store = MemoryTransactionStore();
    final jobs = JobRepository(store);
    const scope = AccountScope.local();
    final first = await jobs.enqueue(scope, 'fixture', 'item');
    await jobs.transition(scope, first, JobState.running);
    await jobs.transition(scope, first, JobState.recoveryRequired);
    final stale = jobs.read(scope).single;
    await jobs.transition(scope, stale, JobState.completed);
    final operation = TestOperation();
    final coordinator = JobCoordinator(jobs, {'fixture': operation});
    await coordinator.reconcile(scope, stale);
    expect(operation.reconciliations, 0);
    await coordinator.close();
    await store.close();
  });
  test('diagnostics are bounded and have no arbitrary payload channel', () {
    final diagnostics = Diagnostics();
    for (var i = 0; i < 200; i++) {
      diagnostics.record(DiagnosticEvent.startup);
    }
    expect('startup'.allMatches(diagnostics.export()), hasLength(100));
    expect(
      const AppFailure(FailureKind.network).toString(),
      isNot(contains('token')),
    );
  });
}
