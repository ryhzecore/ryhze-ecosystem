import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/providers/public_catalog_cache.dart';
import 'package:ryhze/providers/ryhze_catalog_repository.dart';

import 'ryhze_catalog_repository_test.dart' show Transport, entry, encode;
import 'support/journal_fixture.dart';

void main() {
  const games = EntertainmentCategory.games;
  final start = DateTime.utc(2026, 10, 10);
  Future<RepositoryRead<EntertainmentTitle>> read(
    RyhzeCatalogRepository repo,
  ) => repo.search(games, '', CancellationToken());

  test(
    'journal restart keeps bounded public metadata and original freshness',
    () async {
      final Directory root = await createJournalFixture('ryhze-public-cache-');
      JournalStore? store;
      try {
        store = await JournalStore.open(root);
        await store.transact((data) {
          (data['preferences'] as Map)['reduceMotion'] = true;
        });
        final repo = RyhzeCatalogRepository(
          cache: PublicCatalogCache(store),
          clock: () => start,
          transport: Transport([
            (_) => encode([
              entry('one')..['unrecognizedPrivateField'] = 'must not persist',
              entry('hidden', hidden: true),
              entry('film', kind: 'film'),
            ]),
          ]),
        );
        expect((await read(repo)).quality, ReadQuality.complete);
        final encoded = store.read()[PublicCatalogCache.storageKey].toString();
        expect(encoded, isNot(contains('must not persist')));
        expect(encoded, isNot(contains('unrecognizedPrivateField')));
        expect(encoded, isNot(contains('Synthetic hidden')));
        expect(encoded, isNot(contains('Synthetic film')));
        await store.close();
        store = await JournalStore.open(root);
        final unused = Transport([]);
        final fresh = await read(
          RyhzeCatalogRepository(
            cache: PublicCatalogCache(store),
            clock: () => start.add(const Duration(minutes: 4)),
            transport: unused,
          ),
        );
        expect(fresh.quality, ReadQuality.complete);
        expect(fresh.observedAt, start);
        expect(
          fresh.values.single.availability,
          RyhzeCatalogRepository.metadataAvailability,
        );
        expect(unused.calls, 0);
        expect((store.read()['preferences'] as Map)['reduceMotion'], isTrue);
        final stale = await read(
          RyhzeCatalogRepository(
            cache: PublicCatalogCache(store),
            clock: () => start.add(const Duration(minutes: 5)),
            transport: Transport([
              (_) => throw const AppFailure(FailureKind.denied),
            ]),
          ),
        );
        expect(stale.quality, ReadQuality.stale);
        expect(stale.observedAt, start);
        expect(stale.failure?.kind, FailureKind.denied);
        expect(stale.values.single.id, 'one');
      } finally {
        await store?.close();
        await root.delete(recursive: true);
      }
    },
  );

  test(
    'future, expired, wrong-source and malformed disk data are never fresh',
    () async {
      final seed = MemoryTransactionStore();
      await read(
        RyhzeCatalogRepository(
          cache: PublicCatalogCache(seed),
          clock: () => start,
          transport: Transport([
            (_) => encode([entry('cached')]),
          ]),
        ),
      );
      final good = Map<String, Object?>.from(
        seed.read()[PublicCatalogCache.storageKey] as Map,
      );
      final invalid = [
        {
          ...good,
          'observedAt': start.add(const Duration(seconds: 1)).toIso8601String(),
        },
        {
          ...good,
          'observedAt': start
              .subtract(const Duration(days: 8))
              .toIso8601String(),
        },
        {...good, 'source': 'https://unapproved.invalid/'},
        {...good, 'version': 2},
        {...good, 'payload': '[] broken'},
        {...good, 'payload': 'x' * (PublicCatalogCache.maxBytes + 1)},
        {
          ...good,
          'payload': String.fromCharCodes(
            encode([entry('same'), entry('same')]),
          ),
        },
        {
          ...good,
          'payload': String.fromCharCodes(
            encode([entry('bad')..['title'] = '\n']),
          ),
        },
      ];
      for (final record in invalid) {
        final store = MemoryTransactionStore();
        await store.transact(
          (data) => data[PublicCatalogCache.storageKey] = record,
        );
        final response = await read(
          RyhzeCatalogRepository(
            cache: PublicCatalogCache(store),
            clock: () => start,
            transport: Transport([
              (_) => throw const AppFailure(FailureKind.denied),
            ]),
          ),
        );
        expect(response.quality, ReadQuality.partial);
        expect(response.values.any((value) => value.id == 'cached'), isFalse);
        expect(store.read()[PublicCatalogCache.storageKey], record);
      }
    },
  );

  test(
    'cancelled or superseded queued writes cannot replace saved metadata',
    () async {
      final store = MemoryTransactionStore();
      final cache = PublicCatalogCache(store);
      final snapshot = PublicCatalogSnapshot(
        String.fromCharCodes(encode([entry('one')])),
        start,
      );
      final token = CancellationToken();
      final pending = cache.write(snapshot, token, isCurrent: () => true);
      token.cancel();
      await expectLater(
        pending,
        throwsA(
          isA<AppFailure>().having(
            (error) => error.kind,
            'kind',
            FailureKind.cancelled,
          ),
        ),
      );
      expect(store.read().containsKey(PublicCatalogCache.storageKey), isFalse);
      await cache.write(snapshot, CancellationToken(), isCurrent: () => false);
      expect(store.read().containsKey(PublicCatalogCache.storageKey), isFalse);
      await expectLater(
        cache.write(
          PublicCatalogSnapshot(
            String.fromCharCodes(
              encode([entry('one')..['credential'] = 'not allowed']),
            ),
            start,
          ),
          CancellationToken(),
          isCurrent: () => true,
        ),
        throwsA(isA<AppFailure>()),
      );
      expect(store.read().containsKey(PublicCatalogCache.storageKey), isFalse);
    },
  );

  test(
    'failed optional persistence does not downgrade validated remote metadata',
    () async {
      final store = MemoryTransactionStore();
      await store.close();
      final response = await read(
        RyhzeCatalogRepository(
          cache: PublicCatalogCache(store),
          clock: () => start,
          transport: Transport([
            (_) => encode([entry('one')]),
          ]),
        ),
      );
      expect(response.quality, ReadQuality.complete);
      expect(response.values.single.id, 'one');
      expect(response.failure, isNull);
    },
  );

  test(
    'retention expires in-memory data too and cannot be shorter than TTL',
    () async {
      var now = start;
      final repo = RyhzeCatalogRepository(
        clock: () => now,
        transport: Transport([
          (_) => encode([entry('one')]),
          (_) => throw const AppFailure(FailureKind.denied),
        ]),
      );
      await read(repo);
      now = start.add(const Duration(days: 8));
      final expired = await read(repo);
      expect(expired.quality, ReadQuality.partial);
      expect(expired.values.any((value) => value.id == 'one'), isFalse);
      expect(
        () => RyhzeCatalogRepository(retention: const Duration(minutes: 1)),
        throwsA(isA<AppFailure>()),
      );
      expect(
        () => RyhzeCatalogRepository(retention: const Duration(days: 31)),
        throwsA(isA<AppFailure>()),
      );
    },
  );
  test('late older request cannot overwrite durable newer metadata', () async {
    final first = Completer<Uint8List>();
    final second = Completer<Uint8List>();
    final store = MemoryTransactionStore();
    final repo = RyhzeCatalogRepository(
      cache: PublicCatalogCache(store),
      clock: () => start,
      transport: Transport([(_) => first.future, (_) => second.future]),
    );
    final older = read(repo);
    final newer = read(repo);
    second.complete(encode([entry('newer')]));
    expect((await newer).values.single.id, 'newer');
    first.complete(encode([entry('older')]));
    expect((await older).values.single.id, 'older');
    final restored = await read(
      RyhzeCatalogRepository(
        cache: PublicCatalogCache(store),
        clock: () => start,
        transport: Transport([]),
      ),
    );
    expect(restored.values.single.id, 'newer');
  });
}
