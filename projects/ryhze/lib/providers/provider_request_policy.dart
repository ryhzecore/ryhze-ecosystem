import 'dart:async';
import 'dart:math';

import '../core/app_failure.dart';

/// Adapters may attach validated Retry-After duration, never a raw response.
class ProviderRequestFailure implements Exception {
  const ProviderRequestFailure(this.failure, {this.retryAfter});
  final AppFailure failure;
  final Duration? retryAfter;
}

typedef RetryWait = Future<void> Function(Duration, CancellationToken);

class ProviderRequestPolicy {
  ProviderRequestPolicy({
    this.maxAttempts = 3,
    this.delayBudget = const Duration(seconds: 30),
    double Function()? random,
    RetryWait? wait,
  }) : random = random ?? Random.secure().nextDouble,
       wait = wait ?? cancellableWait {
    if (maxAttempts < 1 ||
        maxAttempts > 5 ||
        delayBudget.isNegative ||
        delayBudget > const Duration(minutes: 2)) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
  final int maxAttempts;
  final Duration delayBudget;
  final double Function() random;
  final RetryWait wait;
  Future<T> run<T>(
    CancellationToken token,
    Future<T> Function() request,
  ) async {
    var spent = Duration.zero;
    for (var attempt = 0; ; attempt++) {
      token.check();
      try {
        final result = await request();
        token.check();
        return result;
      } on ProviderRequestFailure catch (error) {
        token.check();
        if (error.failure.kind != FailureKind.network ||
            attempt + 1 >= maxAttempts) {
          throw error.failure;
        }
        final sample = random();
        if (!sample.isFinite || sample < 0 || sample > 1) {
          throw const AppFailure(FailureKind.invalidData);
        }
        final jitter = Duration(
          milliseconds: (200 * (1 << attempt) * (0.5 + sample)).round(),
        );
        final requested = error.retryAfter;
        if (requested != null && requested.isNegative) {
          throw const AppFailure(FailureKind.invalidData);
        }
        final delay = requested != null && requested > jitter
            ? requested
            : jitter;
        // Never shorten a provider's limit: stop instead of retrying too early.
        if (spent + delay > delayBudget) throw error.failure;
        spent += delay;
        await wait(delay, token);
      }
    }
  }

  static Future<void> cancellableWait(Duration delay, CancellationToken token) {
    token.check();
    final completion = Completer<void>();
    late final Timer timer;
    late final void Function() remove;
    timer = Timer(delay, () {
      remove();
      completion.complete();
    });
    remove = token.onCancel(() {
      timer.cancel();
      if (!completion.isCompleted) {
        completion.completeError(const AppFailure(FailureKind.cancelled));
      }
    });
    return completion.future;
  }
}
