import 'dart:async';

import 'package:flutter/material.dart';
import 'package:ryhze/features/library/library_screen.dart';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/library/data/evidence_library_repository.dart';
import 'package:ryhze/features/library/data/library_repository.dart';
import 'package:ryhze/features/library/data/library_preferences.dart';
import 'package:ryhze/features/library/library_controller.dart';
import 'package:ryhze/features/session/session_controller.dart';

class EvidenceSource implements EntitlementSource {
  EvidenceSource(this.result);
  RepositoryRead<Entitlement> result;
  @override
  String get authority => 'test-authority';
  @override
  String get provider => 'test-provider';
  @override
  Future<RepositoryRead<Entitlement>> read(
    AccountScope scope,
    CancellationToken cancellation,
  ) async => result;
}

class DeferredLibrary
    implements LibraryRepository, CancellableLibraryRepository {
  final tokens = <CancellationToken>[];
  final requests = <Completer<LibraryLoadResult>>[];
  @override
  Future<LibraryLoadResult> load() => loadCancellable(CancellationToken());
  @override
  Future<LibraryLoadResult> loadCancellable(CancellationToken token) {
    tokens.add(token);
    final request = Completer<LibraryLoadResult>();
    requests.add(request);
    return request.future;
  }
}

void main() {
  final now = DateTime.utc(2026, 10, 7);
  const scope = AccountScope.verified('identity', 'account');
  RepositoryRead<Entitlement> records({
    bool fixture = false,
    ReadQuality quality = ReadQuality.complete,
  }) => RepositoryRead(
    [
      Entitlement(
        account: scope.key,
        item: ProviderItemRef('test-provider', 'Item'),
        authority: 'test-authority',
        observedAt: now,
        state: AccessState.verified,
        fixture: fixture,
      ),
    ],
    quality: quality,
    observedAt: now,
  );
  test(
    'adapter projects evidence, rejects fixtures, and preserves partial status',
    () async {
      final source = EvidenceSource(records(quality: ReadQuality.partial));
      final controller = LibraryController(
        repository: EvidenceLibraryRepository(source, scope, clock: () => now),
      );
      await controller.load();
      expect(controller.entries, hasLength(1));
      expect(controller.statusMessage, contains('Some ownership records'));
      source.result = records(fixture: true);
      await controller.load();
      expect(controller.entries, isEmpty);
      expect(controller.status, LibraryStatus.unavailable);
      controller.dispose();
    },
  );
  test(
    'favourites persist exact identities and never import demo membership',
    () async {
      final storage = MemoryTransactionStore();
      final preferences = LibraryPreferences(storage, scope);
      final repository = EvidenceLibraryRepository(
        EvidenceSource(records()),
        scope,
        clock: () => now,
      );
      var controller = LibraryController(
        repository: repository,
        preferences: preferences,
      );
      await controller.load();
      final id = controller.entries.single.identity;
      await controller.toggleFavourite(id);
      await controller.enterDemo();
      await controller.toggleFavourite(controller.entries.first.identity);
      controller.dispose();
      controller = LibraryController(
        repository: repository,
        preferences: preferences,
      );
      await controller.load();
      expect(controller.isFavourite(id), isTrue);
      expect(preferences.read(), {id});
      expect(
        LibraryPreferences(storage, const AccountScope.local()).read(),
        isEmpty,
      );
      controller.dispose();
      await storage.close();
    },
  );
  test(
    'refresh retains known data with stale banner; replacement cancels source',
    () async {
      final deferred = DeferredLibrary();
      final good = await EvidenceLibraryRepository(
        EvidenceSource(records()),
        scope,
        clock: () => now,
      ).load();
      final controller = LibraryController(repository: deferred);
      final first = controller.load();
      deferred.requests.last.complete(good);
      await first;
      final refresh = controller.refresh();
      expect(controller.entries, hasLength(1));
      deferred.requests.last.complete(
        LibraryLoadResult.failure('token=SECRET /private/path'),
      );
      await refresh;
      expect(controller.entries, hasLength(1));
      expect(controller.isStale, isTrue);
      expect(controller.statusMessage, contains('Previously verified'));
      expect(controller.statusMessage, isNot(contains('SECRET')));
      final old = controller.load();
      await controller.setRepository(const UnavailableLibraryRepository());
      expect(deferred.tokens.last.isCancelled, isTrue);
      deferred.requests.last.complete(good);
      await old;
      expect(controller.entries, isEmpty);
      controller.dispose();
    },
  );
  test(
    'compatibility result constructors cannot expose raw provider failures',
    () {
      const secret = 'Authorization: Bearer secret /private/account';
      expect(
        LibraryLoadResult.failure(secret).message,
        isNot(contains(secret)),
      );
      expect(
        LibraryLoadResult.unavailable(secret).message,
        isNot(contains(secret)),
      );
    },
  );
  testWidgets(
    'selected Library detail displays stale verification after refresh failure',
    (tester) async {
      final deferred = DeferredLibrary();
      final good = await EvidenceLibraryRepository(
        EvidenceSource(records()),
        scope,
        clock: () => now,
      ).load();
      final controller = LibraryController(repository: deferred);
      final loading = controller.load();
      deferred.requests.last.complete(good);
      await loading;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(body: LibraryScreen(controller: controller)),
        ),
      );
      controller.select(controller.entries.single.identity);
      await tester.pumpAndSettle();
      final refresh = controller.refresh();
      deferred.requests.last.complete(LibraryLoadResult.failure('offline'));
      await refresh;
      await tester.pumpAndSettle();
      expect(
        find.textContaining('Previously verified records'),
        findsOneWidget,
      );
      expect(find.textContaining('Actions remain disabled'), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      controller.dispose();
    },
  );
}
