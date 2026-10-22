import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/store/catalog_maintenance.dart';
import 'package:ryhze/providers/public_catalog_cache.dart';
import 'package:ryhze/providers/ryhze_catalog_repository.dart';

import 'ryhze_catalog_repository_test.dart' show Transport, entry, encode;
import 'support/journal_fixture.dart';

class GatedStore extends MemoryTransactionStore {
  Completer<void>? gate;
  @override
  Future<void> transact(void Function(StoreData) update) async {
    await gate?.future;
    await super.transact(update);
  }
}

void main() {
  final start = DateTime.utc(2026, 10, 10);
  const games = EntertainmentCategory.games;
  Future<void> populate(RyhzeCatalogRepository repo) async {
    await repo.search(games, '', CancellationToken());
  }

  Matcher failure(FailureKind kind) =>
      throwsA(isA<AppFailure>().having((error) => error.kind, 'kind', kind));

  test(
    'health reports original age, bounds, invalid and empty without mutations',
    () async {
      final store = MemoryTransactionStore();
      final cache = PublicCatalogCache(store);
      var now = start;
      final repo = RyhzeCatalogRepository(
        cache: cache,
        clock: () => now,
        transport: Transport([
          (_) => encode([entry('one')]),
        ]),
      );
      expect(repo.cacheHealth().state, CatalogCacheState.empty);
      await populate(repo);
      var health = repo.cacheHealth();
      expect(health.state, CatalogCacheState.fresh);
      expect(health.durable, isFalse);
      expect(health.itemCount, 1);
      expect(health.payloadBytes, greaterThan(0));
      expect(health.observedAt, start);
      now = start.add(const Duration(minutes: 5));
      health = repo.cacheHealth();
      expect(health.state, CatalogCacheState.stale);
      expect(health.age, const Duration(minutes: 5));
      now = start.add(const Duration(days: 8));
      expect(repo.cacheHealth().state, CatalogCacheState.expired);
      final before = store.read();
      repo.cacheHealth();
      expect(store.read(), before);
      await store.transact(
        (data) => data[PublicCatalogCache.storageKey] = 'corrupt',
      );
      expect(repo.cacheHealth().state, CatalogCacheState.invalid);
      await repo.clearCache();
      expect(repo.cacheHealth().state, CatalogCacheState.empty);
    },
  );

  test(
    'clear persists across restart and preserves all non-cache state',
    () async {
      final root = await createJournalFixture('ryhze-cache-clear-');
      JournalStore? store;
      try {
        store = await JournalStore.open(root);
        await store.transact((data) {
          data['preferences'] = {'reduceMotion': true, 'privatePreviews': true};
          data['accounts'] = {
            jsonEncode(['test', 'account']): {
              'saved': {'saved-title': 'Saved title'},
              'libraryFavourites': [],
            },
          };
        });
        final before = store.read();
        final repo = RyhzeCatalogRepository(
          cache: PublicCatalogCache(store),
          clock: () => start,
          transport: Transport([
            (_) => encode([entry('one')]),
          ]),
        );
        await populate(repo);
        expect(repo.cacheHealth().durable, isTrue);
        await repo.clearCache();
        expect(store.read(), before);
        await store.close();
        store = await JournalStore.open(root);
        expect(store.read(), before);
        expect(
          PublicCatalogCache(store)
              .health(
                start,
                const Duration(minutes: 5),
                const Duration(days: 7),
              )
              .state,
          CatalogCacheState.empty,
        );
      } finally {
        await store?.close();
        await root.delete(recursive: true);
      }
    },
  );

  test(
    'clear invalidates queued writes and rejects reads until removal finishes',
    () async {
      final store = GatedStore();
      final cache = PublicCatalogCache(store);
      store.gate = Completer<void>();
      final queued = cache.write(
        PublicCatalogSnapshot(utf8.decode(encode([entry('old')])), start),
        CancellationToken(),
        isCurrent: () => true,
      );
      final repo = RyhzeCatalogRepository(
        cache: cache,
        clock: () => start,
        transport: Transport([
          (_) => encode([entry('new')]),
        ]),
      );
      final cleared = repo.clearCache();
      await expectLater(populate(repo), failure(FailureKind.conflict));
      store.gate!.complete();
      await queued;
      await cleared;
      expect(repo.cacheHealth().state, CatalogCacheState.empty);
      await populate(repo);
      expect((await repo.lookup(games, 'new', CancellationToken()))?.id, 'new');
    },
  );

  test('clear cancels network and suppresses old completion without cancelling caller', () async {
    final body = Completer<Uint8List>();
    final caller = CancellationToken();
    CancellationToken? transportToken;
    final store = MemoryTransactionStore();
    final repo = RyhzeCatalogRepository(
      cache: PublicCatalogCache(store),
      clock: () => start,
      transport: Transport([
        (token) {
          transportToken = token;
          return body.future;
        },
        (_) => encode([entry('new')]),
      ]),
    );
    final pending = repo.search(games, '', caller);
    await Future<void>.delayed(Duration.zero);
    final rejected = expectLater(pending, failure(FailureKind.cancelled));
    await repo.clearCache();
    expect(transportToken!.isCancelled, isTrue);
    expect(caller.isCancelled, isFalse);
    body.complete(encode([entry('old')]));
    await rejected;
    expect(repo.cacheHealth().state, CatalogCacheState.empty);
    await populate(repo);
    expect((await repo.lookup(games, 'new', CancellationToken()))?.id, 'new');
  });

  test(
    'cached reads pending completion are invalidated by clear too',
    () async {
      final repo = RyhzeCatalogRepository(
        clock: () => start,
        transport: Transport([
          (_) => encode([entry('one')]),
        ]),
      );
      await populate(repo);
      final pending = repo.search(games, '', CancellationToken());
      final rejected = expectLater(pending, failure(FailureKind.cancelled));
      await repo.clearCache();
      await rejected;
      expect(repo.cacheHealth().state, CatalogCacheState.empty);
    },
  );

  test(
    'failed removal reports storage error and does not claim persisted empty',
    () async {
      final store = MemoryTransactionStore();
      final repo = RyhzeCatalogRepository(
        cache: PublicCatalogCache(store),
        clock: () => start,
        transport: Transport([
          (_) => encode([entry('one')]),
        ]),
      );
      await populate(repo);
      final before = store.read();
      await store.close();
      await expectLater(repo.clearCache(), failure(FailureKind.storage));
      expect(store.read(), before);
      expect(repo.cacheHealth().state, CatalogCacheState.fresh);
    },
  );
}
