import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/features/library/data/library_repository.dart';
import 'package:ryhze/features/library/library_controller.dart';
import 'package:ryhze/features/library/model/library_entry.dart';

class _Repository implements LibraryRepository, CancellableLibraryRepository {
  final requests = <Completer<LibraryLoadResult>>[];
  final tokens = <CancellationToken>[];

  @override
  Future<LibraryLoadResult> load() =>
      throw StateError('The cancellation-aware route must be used.');

  @override
  Future<LibraryLoadResult> loadCancellable(CancellationToken cancellation) {
    final result = Completer<LibraryLoadResult>();
    requests.add(result);
    tokens.add(cancellation);
    return result.future;
  }
}

class _Controller extends LibraryController {
  _Controller(LibraryRepository repository) : super(repository: repository);
  bool get hasObservers => hasListeners;
}

LibraryLoadResult _result(String id) => LibraryLoadResult.verified([
  LibraryEntry(
    identity: LibraryIdentity(providerId: 'test-provider', titleId: id),
    ownership: const LibraryOwnership(
      state: LibraryOwnershipState.verifiedOwned,
      reason: 'Synthetic test evidence.',
    ),
    installation: const LibraryInstallation(
      state: LibraryInstallationState.unknown,
      reason: 'Not inspected.',
    ),
  ),
]);

void main() {
  test(
    'throwing cancellation clears old-account entries and rejects late data',
    () async {
      final old = _Repository();
      final replacement = _Repository();
      final controller = LibraryController(repository: old);
      addTearDown(controller.dispose);
      final initial = controller.load();
      old.requests.single.complete(_result('old-account'));
      await initial;
      final refresh = controller.refresh();
      expect(controller.entries.single.identity.titleId, 'old-account');
      var clearedBeforeCallback = false;
      old.tokens.last.onCancel(() {
        clearedBeforeCallback = controller.entries.isEmpty;
        throw StateError('Private cancellation detail.');
      });
      await controller.setRepository(replacement);
      expect(clearedBeforeCallback, isTrue);
      expect(controller.entries, isEmpty);
      expect(controller.status, LibraryStatus.error);
      expect(
        controller.statusMessage,
        'Library could not be loaded. Try again.',
      );
      expect(replacement.requests, isEmpty);
      old.requests.last.complete(_result('late-old-account'));
      await refresh;
      expect(controller.entries, isEmpty);
      final retry = controller.load();
      replacement.requests.single.complete(_result('new-account'));
      await retry;
      expect(controller.entries.single.identity.titleId, 'new-account');
    },
  );

  test(
    'reentrant source replacement wins over the cancelled outer load',
    () async {
      final old = _Repository();
      final intermediate = _Repository();
      final latest = _Repository();
      final controller = LibraryController(repository: old);
      addTearDown(controller.dispose);
      final oldLoad = controller.load();
      late Future<void> latestLoad;
      old.tokens.single.onCancel(() {
        latestLoad = controller.setRepository(latest);
        throw StateError('Later old-source cancellation failure.');
      });
      await controller.setRepository(intermediate);
      expect(intermediate.requests, isEmpty);
      expect(latest.requests, hasLength(1));
      expect(controller.status, LibraryStatus.loading);
      latest.requests.single.complete(_result('latest'));
      await latestLoad;
      old.requests.single.complete(_result('stale'));
      await oldLoad;
      expect(controller.entries.single.identity.titleId, 'latest');
      expect(controller.status, LibraryStatus.loaded);
    },
  );

  test('dispose releases notifier storage despite a throwing cancellation callback', () async {
    final repository = _Repository();
    final controller = _Controller(repository);
    var notifications = 0;
    controller.addListener(() => notifications++);
    final load = controller.load();
    final first = StateError('Synthetic disposal cancellation failure.');
    repository.tokens.single.onCancel(() => throw first);
    expect(controller.dispose, throwsA(same(first)));
    expect(controller.hasObservers, isFalse);
    expect(repository.tokens.single.isCancelled, isTrue);
    repository.requests.single.complete(_result('late'));
    await load;
    expect(controller.entries, isEmpty);
    expect(notifications, 1);
    expect(controller.dispose, returnsNormally);
  });
}
