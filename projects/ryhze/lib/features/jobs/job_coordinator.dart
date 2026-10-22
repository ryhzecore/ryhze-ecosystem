import '../../core/app_failure.dart';
import '../session/session_controller.dart';
import 'job_repository.dart';

/// An accepted provider handoff is explicitly different from observed completion.
enum JobOutcome { completed, handoffAccepted, unknown }

/// An operation result cannot establish that its external worker settled.
/// Unlike an ordinary failed job, this closes admission until owner recovery.
class JobSettlementFailure extends AppFailure {
  const JobSettlementFailure() : super(FailureKind.recovery);
}

abstract interface class JobOperation {
  Future<JobOutcome> execute(
    JobRecord job,
    CancellationToken cancellation,
    Future<void> Function(int checkpoint) saveCheckpoint,
  );
  Future<JobOutcome> reconcile(JobRecord job, CancellationToken cancellation);
}

/// Holds admission closed after the actual operation futures have settled.
/// This is a job-queue lease, not permission to replace application files.
class JobShutdownLease {
  JobShutdownLease._(this._owner, this._identity, this._epoch);
  final JobCoordinator _owner;
  final Object _identity;
  final int _epoch;
  bool _released = false;

  Future<void> release(AccountScope scope) async {
    if (_released) return;
    _released = true;
    if (identical(_owner._shutdownHold, _identity)) {
      _owner._shutdownHold = null;
      // A newer suspension or account boundary belongs to its current owner.
      if (_owner._epoch == _epoch) await _owner.resume(scope);
    }
  }
}

/// Reserves an already idle queue for one explicit, durably admitted attempt.
/// This cannot cancel other work or reopen admission after a newer suspension.
class JobAdmissionLease {
  JobAdmissionLease._(this._owner, this._identity, this._epoch);
  final JobCoordinator _owner;
  final Object _identity;
  final int _epoch;
  bool _released = false;

  void check() {
    if (_released ||
        _owner._closed ||
        _owner._suspended ||
        _owner._epoch != _epoch ||
        !identical(_owner._shutdownHold, _identity)) {
      throw const AppFailure(FailureKind.cancelled);
    }
  }

  Future<void> run(
    AccountScope scope,
    JobRecord job,
    CancellationToken cancellation,
  ) {
    check();
    cancellation.check();
    // No await between releasing the hold and owning the selected future.
    _released = true;
    _owner._shutdownHold = null;
    return _owner._draining = _owner
        ._runSelected(scope, job, cancellation)
        .whenComplete(() => _owner._draining = null);
  }

  void release() {
    if (_released) return;
    _released = true;
    if (identical(_owner._shutdownHold, _identity)) {
      _owner._shutdownHold = null;
    }
  }

  /// A failed native drain proves neither completion nor safe readmission.
  /// Drop this reservation while keeping the queue closed for owner recovery.
  void suspendAfterSettlementFailure() {
    _owner.suspend();
    release();
  }
}

/// Single-worker queue bounds concurrency and serializes conflicting items.
/// Only implemented local/native operations are registered. A local operation
/// does not establish an authorized download source or provider capability.
/// Persist before side effects; after interruption reconcile, never blindly retry.
class JobCoordinator {
  JobCoordinator(this.repository, Map<String, JobOperation> operations)
    : operations = Map.unmodifiable(operations);
  final JobRepository repository;
  final Map<String, JobOperation> operations;
  Future<void>? _draining;
  Future<void>? _reconciling;
  Future<void>? _resuming;
  CancellationToken? _active;
  bool _closed = false;
  bool _suspended = false;
  Object? _shutdownHold;
  int _epoch = 0;

  /// Admission preparation is allowed only while no queue work is in flight.
  /// Holding this lease does not authorize filesystem effects by itself.
  JobAdmissionLease acquireIdle() {
    if (_closed ||
        _suspended ||
        _shutdownHold != null ||
        _draining != null ||
        _reconciling != null ||
        _resuming != null ||
        _active != null) {
      throw const AppFailure(FailureKind.conflict);
    }
    final identity = _shutdownHold = Object();
    return JobAdmissionLease._(this, identity, _epoch);
  }

  void cancelActive() {
    _epoch++;
    _active?.cancel();
  }

  /// Stop admission immediately; cancellation alone permits another drain.
  void suspend() {
    _suspended = true;
    cancelActive();
  }

  /// Reopen admission only after the current scope has recovered. A newer
  /// suspend, session cancellation or close invalidates this resume attempt.
  /// Recovery failure leaves admission closed so the owner can explicitly retry.
  Future<void> resume(AccountScope scope) {
    if (_closed || _shutdownHold != null) return Future.value();
    suspend();
    final epoch = _epoch;
    final previous = _resuming;
    late final Future<void> result;
    result = _resume(scope, epoch, previous).whenComplete(() {
      if (identical(_resuming, result)) _resuming = null;
    });
    _resuming = result;
    return result;
  }

  Future<void> _resume(
    AccountScope scope,
    int epoch,
    Future<void>? previous,
  ) async {
    if (previous != null) {
      try {
        await previous;
      } catch (_) {
        // A later explicit attempt may retry failed recovery.
      }
    }
    if (_closed || epoch != _epoch) return;
    await _draining;
    await _reconciling;
    if (_closed || epoch != _epoch) return;
    await recover(scope);
    if (!_closed && _shutdownHold == null && epoch == _epoch) {
      _suspended = false;
    }
  }

  /// Prevent lifecycle resume or a second caller from reopening admission while
  /// shutdown preparation is in progress. Failure keeps the queue suspended;
  /// it never issues a lease that would falsely claim settlement.
  Future<JobShutdownLease> prepareShutdown() async {
    if (_closed || _shutdownHold != null) {
      throw const AppFailure(FailureKind.conflict);
    }
    final identity = _shutdownHold = Object();
    try {
      final cancellationFailure = _cancelForSettlement(suspending: true);
      final epoch = _epoch;
      await _joinAfterCancellation(cancellationFailure);
      if (_closed) throw const AppFailure(FailureKind.cancelled);
      return JobShutdownLease._(this, identity, epoch);
    } catch (_) {
      if (identical(_shutdownHold, identity)) _shutdownHold = null;
      rethrow;
    }
  }

  /// Operations must cooperate with cancellation. Wait for their final durable
  /// checkpoint before classifying a running job after lifecycle interruption.
  Future<void> quiesce() async {
    await _joinAfterCancellation(_cancelForSettlement());
  }

  Future<void> close() async {
    _closed = true;
    await _joinAfterCancellation(_cancelForSettlement());
  }

  (Object, StackTrace)? _cancelForSettlement({bool suspending = false}) {
    if (suspending) _suspended = true;
    try {
      cancelActive();
      return null;
    } catch (error, stack) {
      // A callback failure cannot skip joining the operation's durable writes.
      return (error, stack);
    }
  }

  Future<void> _joinAfterCancellation((Object, StackTrace)? failure) async {
    try {
      await _waitForWork();
    } catch (error, stack) {
      failure ??= (error, stack);
    }
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }

  void _retainSettlementFailure() {
    // Publish before callbacks. The originating settlement error owns the result.
    _cancelForSettlement(suspending: true);
  }

  Future<void> _waitForWork() async {
    await Future.wait<void>([?_draining, ?_reconciling, ?_resuming]);
  }

  Future<void> recover(AccountScope scope) async {
    for (final job in repository.read(scope)) {
      if (job.state == JobState.running) {
        await repository.transition(scope, job, JobState.recoveryRequired);
      }
    }
  }

  Future<void> drain(AccountScope scope) {
    if (_closed ||
        _suspended ||
        _shutdownHold != null ||
        _reconciling != null) {
      return Future.value();
    }
    return _draining ??= _run(scope).whenComplete(() => _draining = null);
  }

  Future<void> _runSelected(
    AccountScope scope,
    JobRecord requested,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    final matching = repository
        .read(scope)
        .where((entry) => entry.key == requested.key);
    if (matching.length != 1 || matching.single.state != JobState.queued) {
      throw const AppFailure(FailureKind.conflict);
    }
    final job = matching.single;
    final operation = operations[job.kind];
    if (operation == null) {
      await repository.transition(scope, job, JobState.blocked);
      return;
    }
    await _execute(scope, job, operation, cancellation);
  }

  Future<void> _run(AccountScope scope) async {
    final epoch = _epoch;
    while (!_closed && !_suspended && epoch == _epoch) {
      final queued = repository
          .read(scope)
          .where((job) => job.state == JobState.queued);
      if (queued.isEmpty) return;
      final job = queued.first;
      final operation = operations[job.kind];
      if (operation == null) {
        await repository.transition(scope, job, JobState.blocked);
        continue;
      }
      await _execute(scope, job, operation);
    }
  }

  Future<void> _execute(
    AccountScope scope,
    JobRecord job,
    JobOperation operation, [
    CancellationToken? cancellation,
  ]) async {
    final token = _active = CancellationToken();
    final remove = cancellation?.onCancel(token.cancel);
    var started = false;
    try {
      await repository.transition(scope, job, JobState.running);
      started = true;
      token.check();
      final outcome = await operation.execute(
        job,
        token,
        (checkpoint) => repository.transition(
          scope,
          job,
          JobState.running,
          checkpoint: checkpoint,
        ),
      );
      await repository.transition(
        scope,
        job,
        token.isCancelled ? JobState.recoveryRequired : _state(outcome),
      );
    } catch (error) {
      if (error is JobSettlementFailure) _retainSettlementFailure();
      // No external operation ran if its initial durable write failed.
      if (!started) rethrow;
      // An exception after an external effect does not prove that it failed.
      await repository.transition(
        scope,
        job,
        JobState.recoveryRequired,
        failure: error is AppFailure ? error.kind : FailureKind.recovery,
      );
    } finally {
      remove?.call();
      _active = null;
    }
  }

  Future<void> reconcile(AccountScope scope, JobRecord job) {
    if (_closed ||
        _suspended ||
        _shutdownHold != null ||
        _draining != null ||
        _reconciling != null) {
      return Future.value();
    }
    return _reconciling = _reconcile(
      scope,
      job,
    ).whenComplete(() => _reconciling = null);
  }

  Future<void> _reconcile(AccountScope scope, JobRecord requested) async {
    final matches = repository
        .read(scope)
        .where((entry) => entry.key == requested.key);
    if (matches.isEmpty) return;
    final job = matches.single;
    if (_closed ||
        _suspended ||
        _draining != null ||
        _active != null ||
        job.state != JobState.recoveryRequired) {
      return;
    }
    final operation = operations[job.kind];
    if (operation == null) return;
    final token = _active = CancellationToken();
    try {
      final outcome = await operation.reconcile(job, token);
      if (!token.isCancelled) {
        await repository.transition(scope, job, _state(outcome));
      }
    } catch (error) {
      if (error is JobSettlementFailure) _retainSettlementFailure();
      /* Keep recovery required, never manufacture terminal success. */
    } finally {
      _active = null;
    }
  }

  JobState _state(JobOutcome outcome) => switch (outcome) {
    JobOutcome.completed => JobState.completed,
    JobOutcome.handoffAccepted => JobState.handoffAccepted,
    JobOutcome.unknown => JobState.recoveryRequired,
  };
}
