import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/store/catalog_repository.dart';
import 'package:ryhze/providers/provider_request_policy.dart';
import 'package:ryhze/providers/public_catalog_transport.dart';
import 'package:ryhze/providers/ryhze_catalog_repository.dart';

Map<String, Object> entry(
  String id, {
  bool hidden = false,
  String kind = 'game',
}) => {
  'id': id,
  'kind': kind,
  'title': 'Synthetic $id',
  'description': 'Synthetic public metadata.',
  'status': 'Coming soon',
  'categories': ['Driving'],
  'hidden': hidden,
  'revision': 3,
};
Uint8List encode(Object value) =>
    Uint8List.fromList(utf8.encode(jsonEncode(value)));

class Transport implements PublicCatalogTransport {
  Transport(this.responses);
  final List<FutureOr<Uint8List> Function(CancellationToken)> responses;
  int calls = 0;
  @override
  Future<Uint8List> fetch(CancellationToken cancellation) async =>
      responses[calls++](cancellation);
}

class HeldFallback implements CatalogRepository {
  final result = Completer<RepositoryRead<EntertainmentTitle>>();
  EntertainmentCategory? category;
  String? query;
  CancellationToken? token;

  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) {
    this.category = category;
    this.query = query;
    token = cancellation;
    return result.future;
  }
}

final cancelled = isA<AppFailure>().having(
  (error) => error.kind,
  'kind',
  FailureKind.cancelled,
);
void main() {
  const games = EntertainmentCategory.games;
  const films = EntertainmentCategory.films;
  final start = DateTime.utc(2026, 10, 10);
  test(
    'film preview stays scoped, partial and non-playable without HTTP',
    () async {
      final transport = Transport([]);
      final repo = RyhzeCatalogRepository(transport: transport);
      final read = await repo.search(films, 'tommy riley', CancellationToken());
      expect(read.quality, ReadQuality.partial);
      expect(read.failure?.kind, FailureKind.unsupported);
      expect(read.values.single, same(Catalog.tommyRiley));
      expect(
        read.values.single.description,
        'Lured by money, an influencer joins an underground organisation. '
        'When he regrets his allegiance, he turns against it.',
      );
      expect(read.values.single.status, 'In development');
      expect(
        read.values.single.availability,
        'The film is not available to watch. No release date is available yet.',
      );
      expect(read.values.single.artwork, isNull);
      expect(read.values.single.tags, isEmpty);
      final missing = await repo.search(
        films,
        'larcenous',
        CancellationToken(),
      );
      expect(missing.values, isEmpty);
      expect(missing.quality, ReadQuality.partial);
      expect(missing.failure?.kind, FailureKind.unsupported);
      expect(transport.calls, 0);
    },
  );
  test(
    'film search rejects cancelled and invalid input before fallback',
    () async {
      final transport = Transport([]);
      final fallback = HeldFallback();
      final repo = RyhzeCatalogRepository(
        transport: transport,
        fallback: fallback,
      );
      await expectLater(
        repo.search(films, '', CancellationToken()..cancel()),
        throwsA(cancelled),
      );
      for (final query in ['x' * 513, 'tommy\n']) {
        await expectLater(
          repo.search(films, query, CancellationToken()),
          throwsA(
            isA<AppFailure>().having(
              (error) => error.kind,
              'kind',
              FailureKind.invalidData,
            ),
          ),
        );
      }
      expect(fallback.token, isNull);
      expect(transport.calls, 0);
    },
  );
  test(
    'film cancellation rejects a late fallback without sending HTTP',
    () async {
      final transport = Transport([]);
      final fallback = HeldFallback();
      final repo = RyhzeCatalogRepository(
        transport: transport,
        fallback: fallback,
      );
      final token = CancellationToken();
      final read = repo.search(films, 'tommy', token);
      final checked = expectLater(read, throwsA(cancelled));
      expect(fallback.category, films);
      expect(fallback.query, 'tommy');
      token.cancel();
      expect(fallback.token!.isCancelled, isTrue);
      fallback.result.complete(
        RepositoryRead(
          Catalog.films,
          quality: ReadQuality.complete,
          observedAt: start,
        ),
      );
      await checked;
      expect(transport.calls, 0);
    },
  );
  test(
    'metadata lookup carries partial film quality and exact-ID absence',
    () async {
      final transport = Transport([]);
      final repo = RyhzeCatalogRepository(transport: transport);
      final CatalogMetadataLookupRepository metadata = repo;
      final read = await metadata.lookupRead(
        films,
        'tommy-riley',
        CancellationToken(),
      );
      expect(read.values.single, same(Catalog.tommyRiley));
      expect(read.quality, ReadQuality.partial);
      expect(read.failure?.kind, FailureKind.unsupported);
      final missing = await metadata.lookupRead(
        films,
        'tommy',
        CancellationToken(),
      );
      expect(missing.values, isEmpty);
      expect(missing.quality, ReadQuality.partial);
      expect(missing.failure?.kind, FailureKind.unsupported);
      await expectLater(
        repo.lookup(films, 'tommy-riley', CancellationToken()),
        throwsA(
          isA<AppFailure>().having(
            (error) => error.kind,
            'kind',
            FailureKind.unsupported,
          ),
        ),
      );
      expect(transport.calls, 0);
    },
  );
  test(
    'metadata lookup retains stale observation, failure and exact identity',
    () async {
      var now = start;
      final transport = Transport([
        (_) => encode([entry('one'), entry('one-extra')]),
        (_) => throw const AppFailure(FailureKind.denied),
        (_) => throw const AppFailure(FailureKind.denied),
        (_) => throw const AppFailure(FailureKind.denied),
      ]);
      final repo = RyhzeCatalogRepository(
        transport: transport,
        clock: () => now,
      );
      await repo.search(games, '', CancellationToken());
      now = start.add(const Duration(minutes: 6));
      final read = await repo.lookupRead(games, 'one', CancellationToken());
      expect(read.values.map((value) => value.id), ['one']);
      expect(read.quality, ReadQuality.stale);
      expect(read.observedAt, start);
      expect(read.failure?.kind, FailureKind.denied);
      expect(
        read.values.single.availability,
        RyhzeCatalogRepository.metadataAvailability,
      );
      final missing = await repo.lookupRead(games, 'on', CancellationToken());
      expect(missing.values, isEmpty);
      expect(missing.quality, ReadQuality.stale);
      expect(missing.observedAt, start);
      expect(missing.failure?.kind, FailureKind.denied);
      await expectLater(
        repo.lookup(games, 'one', CancellationToken()),
        throwsA(
          isA<AppFailure>().having(
            (error) => error.kind,
            'kind',
            FailureKind.denied,
          ),
        ),
      );
    },
  );
  test(
    'metadata lookup retains fallback quality without granting availability',
    () async {
      final repo = RyhzeCatalogRepository(
        transport: Transport([
          (_) => throw const AppFailure(FailureKind.network),
        ]),
        policy: ProviderRequestPolicy(maxAttempts: 1),
      );
      final read = await repo.lookupRead(
        games,
        Catalog.larcenous.id,
        CancellationToken(),
      );
      expect(read.values.single, same(Catalog.larcenous));
      expect(read.quality, ReadQuality.partial);
      expect(read.failure?.kind, FailureKind.network);
      expect(read.values.single.availability, Catalog.availability);
    },
  );
  test(
    'metadata lookup validates IDs and cancellation before reading',
    () async {
      final transport = Transport([]);
      final repo = RyhzeCatalogRepository(transport: transport);
      for (final id in ['../tommy-riley', '', 'x' * 129]) {
        await expectLater(
          repo.lookupRead(films, id, CancellationToken()),
          throwsA(
            isA<AppFailure>().having(
              (error) => error.kind,
              'kind',
              FailureKind.invalidData,
            ),
          ),
        );
      }
      await expectLater(
        repo.lookupRead(films, 'tommy-riley', CancellationToken()..cancel()),
        throwsA(cancelled),
      );
      expect(transport.calls, 0);
    },
  );
  test(
    'validates all metadata, filters hidden/film, caches and disclaims rights',
    () async {
      final transport = Transport([
        (_) => encode([
          entry('one'),
          entry('hidden', hidden: true),
          entry('film', kind: 'film'),
        ]),
      ]);
      final repo = RyhzeCatalogRepository(
        transport: transport,
        clock: () => start,
      );
      final read = await repo.search(
        games,
        'synthetic driving',
        CancellationToken(),
      );
      expect(read.quality, ReadQuality.complete);
      expect(read.values.map((e) => e.id), ['one']);
      expect(
        read.values.single.availability,
        RyhzeCatalogRepository.metadataAvailability,
      );
      expect((await repo.lookup(games, 'one', CancellationToken()))?.id, 'one');
      expect(await repo.lookup(games, 'hidden', CancellationToken()), isNull);
      expect(transport.calls, 1);
      final films = await repo.search(
        EntertainmentCategory.films,
        '',
        CancellationToken(),
      );
      expect(films.quality, ReadQuality.partial);
      expect(films.failure?.kind, FailureKind.unsupported);
      expect(films.values.single, same(Catalog.tommyRiley));
      expect(transport.calls, 1);
      expect(
        () => read.values.single.tags.add('mutation'),
        throwsUnsupportedError,
      );
    },
  );
  test('whole response rejects duplicates, malformed hidden data and bounded fields', () async {
    final malformed = <Object>[
      {},
      [entry('same'), entry('same')],
      [entry('bad')..['title'] = 'bad\ntext'],
      [
        entry('bad')..['categories'] = ['Driving', 'Driving'],
      ],
      [entry('bad')..['kind'] = 'unexpected'],
      [entry('bad')..['revision'] = 3.5],
      [entry('bad')..['hidden'] = 'false'],
      [entry('bad')..['description'] = 'x' * 8001],
      [entry('bad')..['categories'] = List.filled(17, 'tag')],
      [entry('../bad')],
      [entry('ok'), entry('bad', hidden: true)..['title'] = ''],
      List.generate(1001, (i) => entry('id-$i')),
    ];
    for (final body in malformed) {
      final repo = RyhzeCatalogRepository(
        transport: Transport([(_) => encode(body)]),
      );
      final read = await repo.search(games, '', CancellationToken());
      expect(read.quality, ReadQuality.partial);
      expect(read.failure?.kind, FailureKind.invalidData);
      expect(read.values.any((item) => item.id == 'ok'), isFalse);
    }
    for (final bytes in [
      Uint8List.fromList([0xff]),
      Uint8List(1024 * 1024 + 1),
    ]) {
      final read = await RyhzeCatalogRepository(
        transport: Transport([(_) => bytes]),
      ).search(games, '', CancellationToken());
      expect(read.failure?.kind, FailureKind.invalidData);
    }
  });
  test(
    'TTL refresh failure keeps observed time and exposes stale quality',
    () async {
      var now = start;
      final transport = Transport([
        (_) => encode([entry('one')]),
        (_) => throw const AppFailure(FailureKind.denied),
        (_) => throw const AppFailure(FailureKind.denied),
      ]);
      final repo = RyhzeCatalogRepository(
        transport: transport,
        clock: () => now,
      );
      await repo.search(games, '', CancellationToken());
      now = now.add(const Duration(minutes: 6));
      final stale = await repo.search(games, '', CancellationToken());
      expect(stale.quality, ReadQuality.stale);
      expect(stale.observedAt, start);
      expect(stale.failure?.kind, FailureKind.denied);
      await expectLater(
        repo.lookup(games, 'one', CancellationToken()),
        throwsA(
          isA<AppFailure>().having((e) => e.kind, 'kind', FailureKind.denied),
        ),
      );
    },
  );
  test('network retry obeys policy; failed initial request is partial local metadata', () async {
    final delays = <Duration>[];
    final transport = Transport([
      (_) => throw const ProviderRequestFailure(
        AppFailure(FailureKind.network),
        retryAfter: Duration(seconds: 1),
      ),
      (_) => encode([entry('one')]),
    ]);
    final repo = RyhzeCatalogRepository(
      transport: transport,
      policy: ProviderRequestPolicy(
        random: () => .5,
        wait: (delay, token) async {
          delays.add(delay);
          token.check();
        },
      ),
    );
    expect(
      (await repo.search(games, '', CancellationToken())).quality,
      ReadQuality.complete,
    );
    expect(delays, [const Duration(seconds: 1)]);
    final failed = RyhzeCatalogRepository(
      transport: Transport([
        (_) =>
            throw const ProviderRequestFailure(AppFailure(FailureKind.network)),
      ]),
      policy: ProviderRequestPolicy(maxAttempts: 1),
    );
    final local = await failed.search(games, '', CancellationToken());
    expect(local.quality, ReadQuality.partial);
    expect(local.failure?.kind, FailureKind.network);
  });
  test(
    'cancellation suppresses late completion without cancelling another caller',
    () async {
      final first = Completer<Uint8List>();
      final second = Completer<Uint8List>();
      final transport = Transport([(_) => first.future, (_) => second.future]);
      final repo = RyhzeCatalogRepository(transport: transport);
      final token = CancellationToken();
      final old = repo.search(games, '', token);
      final expectation = expectLater(old, throwsA(cancelled));
      final current = repo.search(games, '', CancellationToken());
      token.cancel();
      second.complete(encode([entry('new')]));
      expect((await current).values.single.id, 'new');
      first.complete(encode([entry('old')]));
      await expectation;
      expect(
        (await repo.search(games, '', CancellationToken())).values.single.id,
        'new',
      );
      expect(transport.calls, 2);
    },
  );
  test('out of order successful callers cannot roll back the cache', () async {
    final first = Completer<Uint8List>(), second = Completer<Uint8List>();
    final repo = RyhzeCatalogRepository(
      transport: Transport([(_) => first.future, (_) => second.future]),
    );
    final old = repo.search(games, '', CancellationToken());
    final current = repo.search(games, '', CancellationToken());
    second.complete(encode([entry('new')]));
    await current;
    first.complete(encode([entry('old')]));
    expect((await old).values.single.id, 'old');
    expect(
      (await repo.search(games, '', CancellationToken())).values.single.id,
      'new',
    );
  });
}
