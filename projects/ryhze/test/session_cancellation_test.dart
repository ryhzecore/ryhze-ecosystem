import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/features/session/session_controller.dart';

void main() {
  for (final disposing in [false, true]) {
    test('session cancellation owns reentrant work; dispose=$disposing', () {
      final session = SessionController();
      session.replace(const AccountScope.verified('fixture', 'account'));
      final first = session.beginWork();
      final second = session.beginWork();
      CancellationToken? nested;
      first.onCancel(() {
        session.finishWork(first);
        nested = session.beginWork();
      });
      expect(disposing ? session.dispose : session.signOut, returnsNormally);
      expect(first.isCancelled, isTrue);
      expect(second.isCancelled, isTrue);
      expect(nested!.isCancelled, isTrue);
      if (disposing) {
        expect(session.beginWork().isCancelled, isTrue);
      } else {
        expect(session.scope.isLocal, isTrue);
        expect(session.beginWork().isCancelled, isFalse);
        session.dispose();
      }
    });
  }

  test('newer nested replacement wins and cancellation guard stays raised', () {
    final session = SessionController();
    addTearDown(session.dispose);
    final first = session.beginWork();
    final second = session.beginWork();
    final during = <CancellationToken>[];
    final notified = <CancellationToken>[];
    final oldWorkCancelledAtNotification = <bool>[];
    session.addListener(() {
      oldWorkCancelledAtNotification.add(second.isCancelled);
      notified.add(session.beginWork());
    });
    first.onCancel(() {
      session.replace(const AccountScope.verified('fixture', 'newer'));
      during.add(session.beginWork());
    });
    second.onCancel(() => during.add(session.beginWork()));
    session.replace(const AccountScope.verified('fixture', 'older'));
    expect(session.generation, 2);
    expect(session.scope.accountId, 'newer');
    expect(during.every((token) => token.isCancelled), isTrue);
    expect(oldWorkCancelledAtNotification, [true]);
    expect(notified.single.isCancelled, isFalse);
    expect(session.beginWork().isCancelled, isFalse);
  });

  test('replacement notifications admit normal work in the new scope', () {
    final session = SessionController();
    addTearDown(session.dispose);
    CancellationToken? notified;
    session.addListener(() => notified = session.beginWork());
    session.replace(const AccountScope.verified('fixture', 'next'));
    expect(notified!.isCancelled, isFalse);
  });

  test('nested dispose prevents an older replacement from publishing', () {
    final session = SessionController();
    session.replace(const AccountScope.verified('fixture', 'initial'));
    var notified = 0;
    session.addListener(() => notified++);
    session.beginWork().onCancel(session.dispose);
    expect(session.signOut, returnsNormally);
    expect(session.generation, 3);
    expect(session.scope.accountId, 'initial');
    expect(notified, 0);
    expect(session.beginWork().isCancelled, isTrue);
  });

  test(
    'throwing callback cannot leave later listeners or work uncancelled',
    () {
      final session = SessionController();
      addTearDown(session.dispose);
      session.replace(const AccountScope.verified('fixture', 'account'));
      final first = session.beginWork();
      final second = session.beginWork();
      final expected = StateError('synthetic listener failure');
      var laterListenerCalled = false;
      first.onCancel(() => throw expected);
      first.onCancel(() => laterListenerCalled = true);
      expect(session.signOut, throwsA(same(expected)));
      expect(laterListenerCalled, isTrue);
      expect(second.isCancelled, isTrue);
      expect(session.scope.isLocal, isTrue);
      expect(session.beginWork().isCancelled, isFalse);
    },
  );

  for (final nested in [false, true]) {
    test('disposal waits for notifier dispatch; nested=$nested', () {
      final errors = <FlutterErrorDetails>[];
      final previousHandler = FlutterError.onError;
      FlutterError.onError = errors.add;
      addTearDown(() => FlutterError.onError = previousHandler);
      final session = SessionController();
      var firstNotification = true;
      session.addListener(() {
        if (!firstNotification) return;
        firstNotification = false;
        if (nested) {
          session.beginWork().onCancel(session.dispose);
          session.signOut();
        } else {
          session.dispose();
        }
      });
      // A second listener exposes premature destruction of the listener array.
      session.addListener(
        () => expect(session.beginWork().isCancelled, isTrue),
      );
      session.replace(const AccountScope.verified('fixture', 'next'));
      expect(session.beginWork().isCancelled, isTrue);
      expect(session.dispose, returnsNormally);
      expect(errors, isEmpty);
    });
  }
}
