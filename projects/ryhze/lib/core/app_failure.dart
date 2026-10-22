/// Messages are selected from a closed vocabulary at construction. Provider
/// responses, exception strings, tokens and paths cannot become UI messages.
enum FailureKind {
  network,
  denied,
  unsupported,
  authentication,
  invalidData,
  storage,
  cancelled,
  conflict,
  recovery,
}

class AppFailure implements Exception {
  const AppFailure(this.kind);
  final FailureKind kind;
  bool get retryable =>
      kind == FailureKind.network || kind == FailureKind.storage;
  String get message => switch (kind) {
    FailureKind.network => 'The service is unavailable. Try again later.',
    FailureKind.denied => 'Permission was not granted.',
    FailureKind.unsupported => 'This capability is not available.',
    FailureKind.authentication => 'Connect an authorized account to continue.',
    FailureKind.invalidData => 'The data could not be validated.',
    FailureKind.storage =>
      'Local data could not be saved. Your previous data is preserved.',
    FailureKind.cancelled => 'The operation was cancelled.',
    FailureKind.conflict => 'A conflicting operation is already active.',
    FailureKind.recovery =>
      'Recovery is required before this operation can continue.',
  };
  @override
  String toString() => message;
}

class CancellationToken {
  bool _cancelled = false;
  final List<void Function()> _listeners = [];
  bool get isCancelled => _cancelled;
  void Function() onCancel(void Function() listener) {
    if (_cancelled) {
      listener();
      return () {};
    } else {
      _listeners.add(listener);
      return () => _listeners.remove(listener);
    }
  }

  void cancel() {
    if (_cancelled) return;
    _cancelled = true;
    final listeners = List.of(_listeners);
    _listeners.clear();
    (Object, StackTrace)? failure;
    for (final listener in listeners) {
      try {
        listener();
      } catch (error, stack) {
        failure ??= (error, stack);
      }
    }
    // Cancellation owns every registered listener even if one fails. Preserve
    // the first failure for its caller after the remaining cleanup has run.
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }

  void check() {
    if (_cancelled) throw const AppFailure(FailureKind.cancelled);
  }
}
