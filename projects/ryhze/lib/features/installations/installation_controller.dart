import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../../domain/contracts.dart';
import 'installation_repository.dart';

/// Device inspection is independent of ownership and has no launch authority.
class InstallationController extends ChangeNotifier {
  InstallationRepository? _repository;
  CancellationToken? _request;
  bool _disposed = false, _invalidating = false;
  bool _pendingNotification = false, _notifierDisposed = false;
  int _notificationDepth = 0;
  RepositoryRead<InstallationObservation>? read;
  AppFailure? failure;
  bool get available => _repository != null;
  bool get busy => _request != null;

  void attach(InstallationRepository repository) {
    if (_disposed) return;
    _repository = repository;
    _invalidate(clear: true);
  }

  Future<void> inspect() async {
    if (_disposed || _invalidating || busy || _repository == null) return;
    final repository = _repository!;
    final token = _request = CancellationToken();
    failure = null;
    notifyListeners();
    try {
      // Notification callbacks can replace the scope or dispose this owner.
      if (_disposed || token.isCancelled || !identical(_request, token)) return;
      final result = await repository.inspect(token);
      if (_disposed || token.isCancelled || !identical(_request, token)) return;
      if (result.quality == ReadQuality.unavailable && read != null) {
        read = RepositoryRead(
          read!.values,
          quality: ReadQuality.stale,
          observedAt: read!.observedAt,
          failure: result.failure,
        );
      } else {
        read = result;
      }
      failure = result.failure;
    } catch (error) {
      if (_disposed || token.isCancelled || !identical(_request, token)) return;
      failure = error is AppFailure
          ? error
          : const AppFailure(FailureKind.unsupported);
      if (read != null) {
        read = RepositoryRead(
          read!.values,
          quality: ReadQuality.stale,
          observedAt: read!.observedAt,
          failure: failure,
        );
      }
    } finally {
      if (!_disposed && identical(_request, token)) {
        _request = null;
        notifyListeners();
      }
    }
  }

  void cancel({bool clear = false}) {
    if (_disposed) return;
    _invalidate(clear: clear);
  }

  void _invalidate({bool clear = false, bool stale = false}) {
    final wasInvalidating = _invalidating;
    _invalidating = true;
    final request = _request;
    _request = null;
    if (clear) {
      read = null;
      failure = null;
    } else if (stale) {
      final previous = read;
      if (previous != null && previous.quality != ReadQuality.unavailable) {
        read = RepositoryRead(
          previous.values,
          quality: ReadQuality.stale,
          observedAt: previous.observedAt,
          failure: previous.failure,
        );
      }
    }
    if (!_disposed) _pendingNotification = true;
    (Object, StackTrace)? cleanupFailure;
    try {
      request?.cancel();
    } catch (error, stack) {
      cleanupFailure = (error, stack);
    } finally {
      _invalidating = wasInvalidating;
    }
    // Nested invalidation publishes its state immediately; only the outer
    // sweep announces the settled boundary after every old callback returns.
    if (!_disposed && !_invalidating && _pendingNotification) {
      _pendingNotification = false;
      notifyListeners();
    }
    if (cleanupFailure != null) {
      Error.throwWithStackTrace(cleanupFailure.$1, cleanupFailure.$2);
    }
  }

  /// Power/lifecycle interruption invalidates the previous device observation.
  void interrupt() {
    if (_disposed) return;
    _invalidate(stale: true);
  }

  @override
  void notifyListeners() {
    if (_disposed) return;
    if (_invalidating) {
      _pendingNotification = true;
      return;
    }
    _notificationDepth++;
    try {
      super.notifyListeners();
    } finally {
      _notificationDepth--;
      _disposeNotifierIfReady();
    }
  }

  void _disposeNotifierIfReady() {
    if (_disposed && _notificationDepth == 0 && !_notifierDisposed) {
      _notifierDisposed = true;
      super.dispose();
    }
  }

  @override
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _pendingNotification = false;
    _repository = null;
    try {
      _invalidate(clear: true);
    } finally {
      // Dispatch owns deferred cleanup; ordinary disposal calls its superclass
      // here so the ChangeNotifier lifecycle contract remains explicit.
      if (_notificationDepth == 0 && !_notifierDisposed) {
        _notifierDisposed = true;
        super.dispose();
      }
    }
  }
}
