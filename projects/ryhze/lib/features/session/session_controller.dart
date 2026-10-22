import 'dart:convert';

import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';

class AccountScope {
  const AccountScope.local() : authority = 'local', accountId = 'guest';
  const AccountScope.verified(this.authority, this.accountId);
  final String authority;
  final String accountId;
  String get key => jsonEncode([authority, accountId]);
  bool get isLocal => authority == 'local';
}

/// A session boundary, not a sign-in implementation. Only a future authorized
/// identity adapter may supply verified scopes; there is no UI to invent one.
class SessionController extends ChangeNotifier {
  AccountScope _scope = const AccountScope.local();
  int _generation = 0;
  final Set<CancellationToken> _work = {};
  bool _invalidating = false, _disposed = false;
  bool _pendingNotification = false, _notifierDisposed = false;
  int _notificationDepth = 0;
  AccountScope get scope => _scope;
  int get generation => _generation;
  CancellationToken beginWork() {
    final token = CancellationToken();
    if (_invalidating || _disposed) {
      token.cancel();
    } else {
      _work.add(token);
    }
    return token;
  }

  void finishWork(CancellationToken token) => _work.remove(token);
  void replace(AccountScope scope) {
    if (_disposed) throw const AppFailure(FailureKind.cancelled);
    if ([scope.authority, scope.accountId].any(
      (part) =>
          part.isEmpty ||
          part.length > 256 ||
          RegExp(r'[\x00-\x1f]').hasMatch(part),
    )) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final generation = ++_generation;
    final failure = _cancelWork();
    // A cancellation callback may replace or dispose this session. Its newer
    // boundary wins; an older replacement cannot publish over it.
    if (!_disposed && generation == _generation) {
      _scope = scope;
      _pendingNotification = true;
    }
    // A newer nested replacement waits for the outer sweep to cancel all old
    // work. Its listeners then receive one usable, settled session boundary.
    if (!_disposed && !_invalidating && _pendingNotification) {
      _pendingNotification = false;
      notifyListeners();
    }
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }

  (Object, StackTrace)? _cancelWork() {
    final wasInvalidating = _invalidating;
    _invalidating = true;
    final pending = List<CancellationToken>.of(_work);
    _work.clear();
    (Object, StackTrace)? failure;
    try {
      for (final token in pending) {
        try {
          token.cancel();
        } catch (error, stack) {
          failure ??= (error, stack);
        }
      }
    } finally {
      _invalidating = wasInvalidating;
    }
    return failure;
  }

  void signOut() => replace(const AccountScope.local());

  @override
  void notifyListeners() {
    if (_disposed) return;
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
    _generation++;
    final failure = _cancelWork();
    // ChangeNotifier cannot release its listener array during active dispatch.
    // Logical disposal and work cancellation are immediate; storage waits only
    // until the last notification stack has unwound.
    if (_notificationDepth == 0 && !_notifierDisposed) {
      _notifierDisposed = true;
      super.dispose();
    }
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }
}
