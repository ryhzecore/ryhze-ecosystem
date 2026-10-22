import 'dart:async';

import '../../core/app_failure.dart';
import '../../domain/contracts.dart';
import '../session/session_controller.dart';
import 'managed_content_manifest.dart';
import 'managed_installation.dart';

class ManagedContentAccessRequest {
  const ManagedContentAccessRequest(
    this.scope,
    this.sessionGeneration,
    this.deviceId,
    this.intent,
  );
  final AccountScope scope;
  final int sessionGeneration;
  final String deviceId;
  final ManagedStageIntent intent;
}

/// A trusted composition adapter's assertion, NOT authentication by construction.
/// The adapter independently verifies source authenticity and distribution/access
/// rights before returning. Catalogues and caller-supplied hashes cannot do this.
class ManagedContentGrant {
  const ManagedContentGrant({
    required this.scope,
    required this.sessionGeneration,
    required this.deviceId,
    required this.provenance,
    required this.state,
    required this.fixture,
  });
  final AccountScope scope;
  final int sessionGeneration;
  final String deviceId;
  final ManagedManifestProvenance provenance;
  final AccessState state;
  final bool fixture;
}

abstract interface class ManagedContentAuthority {
  bool get available;
  String get authority;
  String get policy;
  int get generation;

  /// An adapter increments generation and synchronously notifies on revocation,
  /// account/connection/policy replacement or loss of authority. Listeners cannot
  /// be substituted by polling generation after a filesystem operation ends.
  void addInvalidationListener(void Function() listener);
  void removeInvalidationListener(void Function() listener);

  /// Must propagate cancellation, bound transport waits, and settle the actual
  /// read before completing. There is no production protocol in this contract.
  Future<ManagedContentGrant> authorize(
    ManagedContentAccessRequest request,
    CancellationToken cancellation,
  );
}

class UnavailableManagedContentAuthority implements ManagedContentAuthority {
  const UnavailableManagedContentAuthority();
  @override
  bool get available => false;
  @override
  String get authority => '';
  @override
  String get policy => '';
  @override
  int get generation => 0;
  @override
  void addInvalidationListener(void Function() listener) {}
  @override
  void removeInvalidationListener(void Function() listener) {}
  @override
  Future<ManagedContentGrant> authorize(
    ManagedContentAccessRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    throw const AppFailure(FailureKind.unsupported);
  }
}

/// Read-only prerequisite owned by ManagedContentService, sharing its session
/// and device. At most one read/live ticket exists. No durable permission exists.
class ManagedContentAdmissionService {
  ManagedContentAdmissionService({
    required this.session,
    required this.deviceId,
    this.source = const UnavailableManagedContentAuthority(),
    DateTime Function()? now,
  }) : _now = now ?? DateTime.now {
    validateManagedDeviceId(deviceId);
    source.addInvalidationListener(_invalidate);
  }
  static const maximumGrantAge = Duration(minutes: 5);
  final SessionController session;
  final String deviceId;
  final ManagedContentAuthority source;
  final DateTime Function() _now;
  final Set<CancellationToken> _tokens = {};
  final Set<ManagedContentAdmission> _tickets = {};
  Future<ManagedContentAdmission>? _running;
  Future<void>? _closing;
  bool _closed = false, _suspended = false;
  int _generation = 0;

  Future<ManagedContentAdmission> admit(
    ManagedStageIntent intent,
    CancellationToken cancellation,
  ) {
    if (_running != null || _tickets.isNotEmpty) {
      return Future.error(const AppFailure(FailureKind.conflict));
    }
    final completion = Completer<ManagedContentAdmission>();
    final result = completion.future;
    // Publish ownership before any authority getter/callback can reenter close.
    _running = result;
    unawaited(
      _settle(completion, () => _admit(intent, cancellation), () {
        if (identical(_running, result)) _running = null;
      }),
    );
    return result;
  }

  Future<ManagedContentAdmission> _admit(
    ManagedStageIntent intent,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    _available();
    if (session.scope.isLocal) {
      throw const AppFailure(FailureKind.authentication);
    }
    final request = ManagedContentAccessRequest(
      session.scope,
      session.generation,
      deviceId,
      intent,
    );
    final generation = _generation;
    final sourceGeneration = source.generation;
    final authority = source.authority;
    final policy = source.policy;
    final token = session.beginWork();
    _tokens.add(token);
    final removeCancellation = cancellation.onCancel(token.cancel);
    var retained = false;
    void release() {
      removeCancellation();
      _tokens.remove(token);
      session.finishWork(token);
    }

    try {
      _available();
      token.check();
      final grant = await source.authorize(request, token);
      final provenance = grant.provenance;
      void checkOwned() {
        token.check();
        if (_closed ||
            _suspended ||
            _generation != generation ||
            session.generation != request.sessionGeneration ||
            session.scope.key != request.scope.key) {
          throw const AppFailure(FailureKind.cancelled);
        }
      }

      DateTime check() {
        token.check();
        _available();
        final currentSourceGeneration = source.generation;
        final currentAuthority = source.authority;
        final currentPolicy = source.policy;
        final now = _now().toUtc();
        final denied =
            grant.fixture ||
            grant.state != AccessState.verified ||
            grant.scope.key != request.scope.key ||
            grant.sessionGeneration != request.sessionGeneration ||
            grant.deviceId != deviceId ||
            provenance.authority != authority ||
            provenance.policy != policy ||
            !provenance.manifest.binds(
              item: intent.item,
              packageId: intent.packageId,
              packageVersion: intent.version,
              manifestSha256: intent.manifestSha256,
              payloadSize: intent.expectedSize,
              payloadSha256: intent.expectedSha256,
            ) ||
            provenance.observedAt.isAfter(now) ||
            now.difference(provenance.observedAt) >= maximumGrantAge ||
            !provenance.expiresAt.isAfter(now);
        // External getters and the injected clock can synchronously invalidate
        // or close. Recheck owned state after all calls, without another getter.
        checkOwned();
        if (currentSourceGeneration != sourceGeneration ||
            currentAuthority != authority ||
            currentPolicy != policy) {
          throw const AppFailure(FailureKind.cancelled);
        }
        if (denied) throw const AppFailure(FailureKind.denied);
        return now;
      }

      final checkedAt = check();
      final maximumExpiry = provenance.observedAt.add(maximumGrantAge);
      final expiry = provenance.expiresAt.isBefore(maximumExpiry)
          ? provenance.expiresAt
          : maximumExpiry;
      final lifetime = expiry.difference(checkedAt);
      checkOwned();
      if (lifetime <= Duration.zero) throw const AppFailure(FailureKind.denied);
      late ManagedContentAdmission ticket;
      ticket = ManagedContentAdmission._(
        request,
        provenance,
        session,
        token,
        () {
          check();
        },
        () {
          _tickets.remove(ticket);
          release();
        },
        lifetime,
      );
      _tickets.add(ticket);
      retained = true;
      return ticket;
    } on AppFailure {
      rethrow;
    } on Object {
      throw const AppFailure(FailureKind.invalidData);
    } finally {
      if (!retained) release();
    }
  }

  void _available() {
    if (_closed || _suspended) throw const AppFailure(FailureKind.cancelled);
    final available = source.available;
    if (_closed || _suspended) throw const AppFailure(FailureKind.cancelled);
    if (!available) throw const AppFailure(FailureKind.unsupported);
  }

  void _invalidate() {
    _generation++;
    (Object, StackTrace)? failure;
    for (final token in List<CancellationToken>.of(_tokens)) {
      try {
        token.cancel();
      } catch (error, stack) {
        failure ??= (error, stack);
      }
    }
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }

  void interrupt() {
    _suspended = true;
    _invalidate();
  }

  void cancelPending() => _invalidate();
  void resume() {
    if (!_closed) _suspended = false;
  }

  Future<void> drain() async {
    try {
      await _running;
    } on Object {
      /* Originating caller owns the result. */
    }
  }

  Future<void> close() {
    final existing = _closing;
    if (existing != null) return existing;
    final completion = Completer<void>();
    // Cancellation listeners may synchronously call close again.
    _closing = completion.future;
    unawaited(_settle(completion, _close, () {}));
    return completion.future;
  }

  Future<void> _close() async {
    _closed = true;
    (Object, StackTrace)? failure;
    void clean(void Function() action) {
      try {
        action();
      } catch (error, stack) {
        failure ??= (error, stack);
      }
    }

    clean(() => source.removeInvalidationListener(_invalidate));
    clean(_invalidate);
    for (final ticket in List<ManagedContentAdmission>.of(_tickets)) {
      clean(ticket.close);
    }
    // A listener failure is not evidence that the source read has settled.
    await drain();
    final retained = failure;
    if (retained != null) Error.throwWithStackTrace(retained.$1, retained.$2);
  }

  Future<void> _settle<T>(
    Completer<T> completion,
    Future<T> Function() work,
    void Function() release,
  ) async {
    try {
      completion.complete(await work());
    } catch (error, stack) {
      completion.completeError(error, stack);
    } finally {
      release();
    }
  }
}

class ManagedContentAdmission {
  ManagedContentAdmission._(
    this.request,
    this.provenance,
    this._session,
    this._token,
    this._check,
    this._release,
    Duration lifetime,
  ) {
    _expiry = Timer(lifetime.isNegative ? Duration.zero : lifetime, () {
      try {
        _token.cancel();
      } catch (error, stack) {
        // A timer has no caller to receive cancellation-listener failures.
        // Keep the first failure owned by this ticket until its close path.
        _expiryFailure ??= (error, stack);
      }
    });
  }
  final ManagedContentAccessRequest request;
  final ManagedManifestProvenance provenance;
  final SessionController _session;
  final CancellationToken _token;
  final void Function() _check, _release;
  late final Timer _expiry;
  (Object, StackTrace)? _expiryFailure;
  bool _claiming = false, _claimed = false, _closed = false;

  ManagedContentAdmissionGuard claim({
    required ManagedStageIntent intent,
    required SessionController session,
    required String deviceId,
  }) {
    if (_closed || _claimed || _claiming) {
      throw const AppFailure(FailureKind.conflict);
    }
    // Reserve the claim before request comparison or authority getters can
    // synchronously reenter. A rejected identity does not consume the ticket.
    _claiming = true;
    try {
      if (!identical(session, _session) ||
          deviceId != request.deviceId ||
          !intent.sameRequest(request.intent)) {
        throw const AppFailure(FailureKind.conflict);
      }
      _check();
      if (_closed) throw const AppFailure(FailureKind.cancelled);
      _claimed = true;
      return ManagedContentAdmissionGuard._(this);
    } finally {
      _claiming = false;
    }
  }

  void close() {
    if (_closed) {
      _throwExpiryFailure();
      return;
    }
    _closed = true;
    _expiry.cancel();
    var failure = _expiryFailure;
    try {
      _token.cancel();
    } catch (error, stack) {
      failure ??= (error, stack);
    }
    try {
      _release();
    } catch (error, stack) {
      failure ??= (error, stack);
    }
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }

  void _throwExpiryFailure() {
    final failure = _expiryFailure;
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }
}

class ManagedContentAdmissionGuard {
  ManagedContentAdmissionGuard._(this._ticket);
  final ManagedContentAdmission _ticket;
  CancellationToken get cancellation => _ticket._token;
  ManagedManifestProvenance get provenance => _ticket.provenance;
  void check() {
    _ticket._throwExpiryFailure();
    if (_ticket._closed) throw const AppFailure(FailureKind.cancelled);
    _ticket._check();
  }

  void close() => _ticket.close();
}
