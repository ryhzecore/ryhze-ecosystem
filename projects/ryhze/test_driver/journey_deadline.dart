import 'dart:async';

/// An owned body deadline reserves time for separately bounded teardown before
/// the test package's backstop fires. The injected clock must be monotonic.
/// Operations are factories so expired work is rejected before it starts.
/// Timeout does not cancel an admitted native operation; its owner must close.
class JourneyBodyDeadline {
  JourneyBodyDeadline({required Duration budget, Duration Function()? clock})
    : _budget = budget,
      _clock = clock ?? _stopwatchClock() {
    if (budget <= Duration.zero) {
      throw ArgumentError.value(budget, 'budget', 'Must be positive.');
    }
    _startedAt = _clock();
  }

  final Duration _budget;
  final Duration Function() _clock;
  late final Duration _startedAt;
  bool _stopped = false;

  static Duration Function() _stopwatchClock() {
    final watch = Stopwatch()..start();
    return () => watch.elapsed;
  }

  Duration get remaining {
    if (_stopped) return Duration.zero;
    final left = _budget - (_clock() - _startedAt);
    return left > Duration.zero ? left : Duration.zero;
  }

  void stop() => _stopped = true;

  Future<T> run<T>(
    Future<T> Function() operation, {
    required Duration timeout,
  }) async {
    if (timeout <= Duration.zero) {
      throw ArgumentError.value(timeout, 'timeout', 'Must be positive.');
    }
    final left = remaining;
    if (left == Duration.zero) {
      stop();
      throw TimeoutException('Journey body deadline reached.');
    }
    try {
      final value = await Future<T>.sync(operation)
          .timeout(timeout < left ? timeout : left);
      // A delayed event loop must not accept a late success before its timer.
      if (remaining == Duration.zero) {
        throw TimeoutException('Journey body deadline reached.');
      }
      return value;
    } on TimeoutException {
      stop();
      rethrow;
    }
  }
}
