import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/design/artwork_reference.dart';
import 'package:ryhze/design/artwork_source.dart';
import 'package:ryhze/providers/public_artwork_cache.dart';
import 'package:ryhze/providers/public_artwork_repository.dart';
import 'package:ryhze/providers/public_artwork_transport.dart';

import 'artwork_fixture.dart';

class Transport implements PublicArtworkTransport {
  int calls = 0;
  Future<Uint8List> Function(CancellationToken)? action;
  @override
  Future<Uint8List> fetch(ArtworkReference ref, CancellationToken token) async {
    calls++;
    return action == null ? artworkPng() : action!(token);
  }
}

// Native/network cancellation can settle an awaiting operation synchronously.
// Keep that behavior visible instead of inserting an async fixture wrapper.
class _SynchronousCancellationTransport implements PublicArtworkTransport {
  final tokens = <CancellationToken>[];

  @override
  Future<Uint8List> fetch(ArtworkReference ref, CancellationToken token) {
    tokens.add(token);
    final result = Completer<Uint8List>.sync();
    token.onCancel(
      () => result.completeError(const AppFailure(FailureKind.cancelled)),
    );
    return result.future;
  }
}

class Cache implements PublicArtworkCache {
  final values = <String, PublicArtworkCached>{};
  int puts = 0, drains = 0, clears = 0;
  bool fail = false;
  Completer<void>? writing;
  @override
  Future<PublicArtworkCached?> get(String key) async {
    if (fail) throw const AppFailure(FailureKind.storage);
    return values[key];
  }

  @override
  Future<void> put(String key, Uint8List bytes, DateTime time) async {
    puts++;
    await writing?.future;
    if (fail) throw const AppFailure(FailureKind.storage);
    values[key] = PublicArtworkCached(bytes, time);
  }

  @override
  Future<PublicArtworkCacheHealth> health() async => PublicArtworkCacheHealth(
    entries: values.length,
    bytes: values.values.fold(0, (a, b) => a + b.bytes.length),
    preserved: 0,
  );
  @override
  Future<PublicArtworkClearResult> clear() async {
    clears++;
    final count = values.length;
    values.clear();
    return PublicArtworkClearResult(removed: count, preserved: 0);
  }

  @override
  Future<void> drain() async {
    drains++;
    await writing?.future;
  }
}

class _GatedClearCache extends Cache {
  final clearing = Completer<void>();
  bool clearStarted = false;

  @override
  Future<PublicArtworkClearResult> clear() async {
    clearStarted = true;
    await clearing.future;
    return super.clear();
  }
}

Future<void> until(bool Function() condition) async {
  for (var i = 0; i < 200 && !condition(); ++i) {
    await Future<void>.delayed(const Duration(milliseconds: 5));
  }
  expect(condition(), isTrue);
}

void main() {
  final now = DateTime.utc(2026, 10, 11);
  for (final boundary in ['interrupt', 'clear', 'close']) {
    test(
      '$boundary cancels all artwork and joins workers after callback failure',
      () async {
        final tokens = <CancellationToken>[];
        final release = Completer<Uint8List>();
        final expected = StateError('synthetic cancellation failure');
        final transport = Transport()
          ..action = (token) {
            tokens.add(token);
            if (tokens.length == 1) token.onCancel(() => throw expected);
            return release.future;
          };
        final cache = Cache();
        final repository = PublicArtworkRepository(
          transport: transport,
          cache: cache,
          validateDecoded: (_) async {},
        );
        final reads = [
          for (var i = 0; i < 3; i++)
            expectLater(
              repository.read(artworkRef(i), CancellationToken()),
              throwsA(
                isA<AppFailure>().having(
                  (e) => e.kind,
                  'kind',
                  FailureKind.cancelled,
                ),
              ),
            ),
        ];
        await until(() => tokens.length == 2);
        var completed = false;
        Future<void>? checked;
        final clearFailure = throwsA(
          isA<AppFailure>().having((e) => e.kind, 'kind', FailureKind.storage),
        );
        if (boundary == 'interrupt') {
          expect(repository.interrupt, throwsA(same(expected)));
        } else {
          final Future<Object?> operation = boundary == 'clear'
              ? repository.clear()
              : repository.close();
          checked = expectLater(
            operation.whenComplete(() => completed = true),
            boundary == 'close' ? throwsA(same(expected)) : clearFailure,
          );
        }
        await Future<void>.delayed(Duration.zero);
        expect(tokens.every((token) => token.isCancelled), isTrue);
        expect(
          transport.calls,
          2,
        ); // The cancelled queued request never fetched.
        expect(completed, isFalse);
        release.complete(artworkPng());
        await Future.wait(reads);
        await checked;
        expect(cache.puts, 0);
        expect(cache.clears, 0);
        if (boundary == 'clear') {
          // Close joins the retained clear owner and reports its failure after
          // draining again; a consumed clear error does not turn into success.
          final closing = repository.close();
          expect(repository.close(), same(closing));
          await expectLater(closing, clearFailure);
          expect(cache.drains, greaterThanOrEqualTo(2));
        } else if (boundary == 'interrupt') {
          await repository.close();
        }
      },
    );
  }
  Matcher failure(FailureKind kind) =>
      throwsA(isA<AppFailure>().having((e) => e.kind, 'kind', kind));
  PublicArtworkRepository repo(
    Transport transport, {
    Cache? cache,
    DateTime Function()? clock,
    Future<void> Function(Uint8List)? decoder,
  }) {
    final repository = PublicArtworkRepository(
      transport: transport,
      cache: cache,
      clock: clock ?? () => now,
      validateDecoded: decoder ?? (_) async {},
    );
    addTearDown(repository.close);
    return repository;
  }

  test(
    'network then memory, exact revision and immutable owned bytes',
    () async {
      final transport = Transport();
      final cache = Cache();
      final repository = repo(transport, cache: cache);
      final first = await repository.read(artworkRef(), CancellationToken());
      expect(first.origin, ArtworkOrigin.network);
      expect(() => first.bytes[0] = 0, throwsUnsupportedError);
      expect(
        (await repository.read(artworkRef(), CancellationToken())).origin,
        ArtworkOrigin.memory,
      );
      await repository.read(artworkRef(0, 2), CancellationToken());
      expect(transport.calls, 2);
      expect(cache.puts, 2);
    },
  );
  test('restart validates disk bytes and avoids fresh network work', () async {
    final cache = Cache();
    cache.values[artworkRef().key] = PublicArtworkCached(artworkPng(), now);
    final transport = Transport();
    var decoded = 0;
    final repository = repo(
      transport,
      cache: cache,
      decoder: (_) async {
        decoded++;
      },
    );
    final result = await repository.read(artworkRef(), CancellationToken());
    expect(result.origin, ArtworkOrigin.disk);
    expect(transport.calls, 0);
    expect(decoded, 1);
  });
  for (final age in [const Duration(hours: 1), const Duration(days: 7)]) {
    test('offline retained disk is labelled stale at $age', () async {
      final cache = Cache();
      cache.values[artworkRef().key] = PublicArtworkCached(
        artworkPng(),
        now.subtract(age),
      );
      final transport = Transport()
        ..action = (_) async => throw const AppFailure(FailureKind.network);
      final result = await repo(
        transport,
        cache: cache,
      ).read(artworkRef(), CancellationToken());
      expect(result.stale, isTrue);
      expect(result.storedAt, now.subtract(age));
    });
  }
  for (final age in [const Duration(days: 8), const Duration(seconds: -1)]) {
    test('expired or future cache cannot be used offline: $age', () async {
      final cache = Cache();
      cache.values[artworkRef().key] = PublicArtworkCached(
        artworkPng(),
        now.subtract(age),
      );
      final transport = Transport()
        ..action = (_) async => throw const AppFailure(FailureKind.network);
      await expectLater(
        repo(transport, cache: cache).read(artworkRef(), CancellationToken()),
        failure(FailureKind.network),
      );
    });
  }
  for (final kind in [FailureKind.denied, FailureKind.invalidData]) {
    test('$kind cannot resurrect a stale image', () async {
      final cache = Cache();
      cache.values[artworkRef().key] = PublicArtworkCached(
        artworkPng(),
        now.subtract(const Duration(hours: 2)),
      );
      final transport = Transport()
        ..action = (_) async => throw AppFailure(kind);
      await expectLater(
        repo(transport, cache: cache).read(artworkRef(), CancellationToken()),
        failure(kind),
      );
    });
  }
  test(
    'corrupt optional disk and unavailable storage do not hide valid network',
    () async {
      final cache = Cache()..fail = true;
      final transport = Transport();
      final repository = repo(transport, cache: cache);
      expect(
        (await repository.read(artworkRef(), CancellationToken())).origin,
        ArtworkOrigin.network,
      );
      expect((await repository.health()).memoryEntries, 1);
    },
  );
  test('invalid PNG and failed pixel decoder never enter cache', () async {
    final cache = Cache();
    final transport = Transport()..action = (_) async => Uint8List(60);
    final repository = repo(transport, cache: cache);
    await expectLater(
      repository.read(artworkRef(), CancellationToken()),
      failure(FailureKind.invalidData),
    );
    expect(cache.puts, 0);
    transport.action = null;
    final second = repo(
      transport,
      cache: cache,
      decoder: (_) async => throw const AppFailure(FailureKind.invalidData),
    );
    await expectLater(
      second.read(artworkRef(), CancellationToken()),
      failure(FailureKind.invalidData),
    );
    expect(cache.puts, 0);
  });
  test('coalesced subscribers cancel independently and last aborts', () async {
    final pending = Completer<Uint8List>();
    CancellationToken? observed;
    final transport = Transport()
      ..action = (token) {
        observed = token;
        return pending.future;
      };
    final repository = repo(transport);
    final one = CancellationToken(), two = CancellationToken();
    final first = repository.read(artworkRef(), one);
    final firstCheck = expectLater(first, failure(FailureKind.cancelled));
    final second = repository.read(artworkRef(), two);
    one.cancel();
    await firstCheck;
    expect(observed!.isCancelled, isFalse);
    expect(transport.calls, 1);
    pending.complete(artworkPng());
    expect((await second).origin, ArtworkOrigin.network);
  });
  test(
    'concurrency and queue are bounded; cancellation settles all jobs',
    () async {
      final transport = Transport()
        ..action = (token) {
          final done = Completer<Uint8List>();
          token.onCancel(
            () => done.completeError(const AppFailure(FailureKind.cancelled)),
          );
          return done.future;
        };
      final repository = repo(transport);
      final tokens = List.generate(10, (_) => CancellationToken());
      final checks = <Future<void>>[];
      for (var i = 0; i < 10; ++i) {
        checks.add(
          expectLater(
            repository.read(artworkRef(i), tokens[i]),
            failure(FailureKind.cancelled),
          ),
        );
      }
      expect(transport.calls, 2);
      await expectLater(
        repository.read(artworkRef(11), CancellationToken()),
        failure(FailureKind.conflict),
      );
      for (final token in tokens) {
        token.cancel();
      }
      await Future.wait(checks);
      await repository.close();
      expect(transport.calls, 2);
    },
  );
  test('clear joins in-flight write then deletes; no late refill', () async {
    final cache = Cache()..writing = Completer<void>();
    final repository = repo(Transport(), cache: cache);
    final read = repository.read(artworkRef(), CancellationToken());
    final checked = expectLater(read, failure(FailureKind.cancelled));
    await until(() => cache.puts == 1);
    final cleared = repository.clear();
    expect(cache.clears, 0);
    await expectLater(
      repository.read(artworkRef(), CancellationToken()),
      failure(FailureKind.conflict),
    );
    cache.writing!.complete();
    expect((await cleared).removed, 1);
    await checked;
    expect(cache.values, isEmpty);
    expect((await repository.health()).memoryEntries, 0);
    expect(repository.changes.value, 1);
  });
  test('close waits actual drain and refuses further reads', () async {
    final cache = Cache()..writing = Completer<void>();
    final repository = repo(Transport(), cache: cache);
    var closed = false;
    final closing = repository.close().then((_) => closed = true);
    await Future<void>.delayed(Duration.zero);
    expect(closed, isFalse);
    cache.writing!.complete();
    await closing;
    await expectLater(
      repository.read(artworkRef(), CancellationToken()),
      failure(FailureKind.cancelled),
    );
  });
  for (final action in ['interrupt', 'clear', 'close']) {
    test(
      '$action settles synchronous cancellation without starting queued reads',
      () async {
        final transport = _SynchronousCancellationTransport();
        final cache = Cache();
        final repository = PublicArtworkRepository(
          transport: transport,
          cache: cache,
        );
        final tokens = List.generate(11, (_) => CancellationToken());
        final checks = List.generate(
          11,
          (i) => expectLater(
            repository.read(artworkRef(i % 10), tokens[i]),
            failure(FailureKind.cancelled),
          ),
        );
        await until(() => transport.tokens.length == 2);
        expect((await repository.health()).queued, 8);
        tokens.first.cancel();
        await checks.first;
        expect(transport.tokens.first.isCancelled, isFalse);
        try {
          switch (action) {
            case 'interrupt':
              repository.interrupt();
            case 'clear':
              await repository.clear();
            case 'close':
              await repository.close();
          }
          await Future.wait(checks);
          final health = await repository.health();
          expect(health.active, 0);
          expect(health.queued, 0);
          expect(health.memoryEntries, 0);
          expect(transport.tokens, hasLength(2));
          expect(cache.clears, action == 'clear' ? 1 : 0);
        } finally {
          // Even a failed assertion must settle the fixture's own work.
          for (final token in tokens) {
            token.cancel();
          }
          await repository.close();
        }
      },
    );
  }
  test(
    'close called during clear owns the same pending cache settlement',
    () async {
      final cache = _GatedClearCache();
      final repository = repo(Transport(), cache: cache);
      Future<void>? closing;
      var requested = false, closed = false;
      repository.changes.addListener(() {
        if (requested) return;
        requested = true;
        closing = repository.close();
        unawaited(closing!.then((_) => closed = true));
      });
      final clearing = repository.clear();
      try {
        await until(() => cache.clearStarted);
        expect(closed, isFalse);
        expect(repository.close(), same(closing));
      } finally {
        cache.clearing.complete();
        await clearing;
        await closing;
      }
      expect(closed, isTrue);
      expect(cache.clears, 1);
      expect(cache.drains, greaterThanOrEqualTo(2));
    },
  );
  test(
    'close publishes its owner before synchronous change notification',
    () async {
      final cache = Cache()..writing = Completer<void>();
      final repository = repo(Transport(), cache: cache);
      Future<void>? nested;
      var requested = false, closed = false;
      repository.changes.addListener(() {
        if (requested) return;
        requested = true;
        nested = repository.close();
      });
      final closing = repository.close();
      unawaited(closing.then((_) => closed = true));
      try {
        expect(nested, same(closing));
        await Future<void>.delayed(Duration.zero);
        expect(closed, isFalse);
      } finally {
        cache.writing!.complete();
        await closing;
      }
      expect(closed, isTrue);
    },
  );
  test('encoded memory evicts by recency and entry limit', () async {
    final transport = Transport();
    final repository = repo(transport);
    for (var i = 0; i < 9; i++) {
      await repository.read(artworkRef(i), CancellationToken());
    }
    expect((await repository.health()).memoryEntries, 8);
    await repository.read(artworkRef(), CancellationToken());
    expect(transport.calls, 10);
    expect(
      (await repository.health()).memoryBytes,
      lessThanOrEqualTo(PublicArtworkRepository.maxMemoryBytes),
    );
  });
  test('encoded byte budget evicts even below the entry-count limit', () async {
    final large = artworkPng(padding: 6 * 1024 * 1024);
    final transport = Transport()..action = (_) async => large;
    final repository = repo(transport);
    for (var i = 0; i < 3; ++i) {
      await repository.read(artworkRef(i), CancellationToken());
    }
    final health = await repository.health();
    expect(health.memoryEntries, 2);
    expect(
      health.memoryBytes,
      lessThanOrEqualTo(PublicArtworkRepository.maxMemoryBytes),
    );
    await repository.read(artworkRef(), CancellationToken());
    expect(transport.calls, 4);
  });
}
