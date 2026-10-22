import 'dart:convert';

import '../core/app_failure.dart';
import '../domain/contracts.dart';
import '../features/entertainment/model/catalog.dart';
import '../features/store/catalog_repository.dart';
import '../features/store/catalog_maintenance.dart';
import 'provider_request_policy.dart';
import 'public_catalog_transport.dart';
import 'public_catalog_cache.dart';
import 'public_catalog_codec.dart';

/// Public RYHZE metadata is not entitlement, ownership, an offer or a download
/// permission. Only fully validated public fields enter memory or optional disk
/// cache. Freshness is five minutes by default; display retention is seven days.
class RyhzeCatalogRepository
    implements
        CatalogRepository,
        CatalogLookupRepository,
        CatalogMetadataLookupRepository,
        CatalogMaintenance {
  RyhzeCatalogRepository({
    PublicCatalogTransport? transport,
    ProviderRequestPolicy? policy,
    DateTime Function()? clock,
    this.ttl = const Duration(minutes: 5),
    CatalogRepository? fallback,
    this.cache,
    this.retention = const Duration(days: 7),
  }) : transport = transport ?? PublicCatalogTransport(),
       policy = policy ?? ProviderRequestPolicy(),
       clock = clock ?? DateTime.now,
       fallback = fallback ?? LocalCatalogRepository() {
    if (ttl.isNegative ||
        ttl > const Duration(hours: 1) ||
        retention < ttl ||
        retention > const Duration(days: 30)) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  static const maxItems = PublicCatalogCodec.maxItems;
  static const maxResponseBytes = 1024 * 1024;
  static const metadataAvailability = PublicCatalogCodec.metadataAvailability;
  final PublicCatalogTransport transport;
  final ProviderRequestPolicy policy;
  final DateTime Function() clock;
  final Duration ttl;
  final CatalogRepository fallback;
  final PublicCatalogCache? cache;
  final Duration retention;
  bool _hydrated = false;
  RepositoryRead<EntertainmentTitle>? _cache;
  int _generation = 0;
  bool _clearing = false;
  final _active = <CancellationToken>{};

  @override
  CatalogCacheHealth cacheHealth() {
    final now = clock().toUtc();
    if (cache != null) return cache!.health(now, ttl, retention);
    final value = _cache;
    if (value == null) {
      return const CatalogCacheHealth(
        state: CatalogCacheState.empty,
        durable: false,
      );
    }
    final age = now.difference(value.observedAt);
    return CatalogCacheHealth(
      state: age.isNegative
          ? CatalogCacheState.invalid
          : age > retention
          ? CatalogCacheState.expired
          : age < ttl
          ? CatalogCacheState.fresh
          : CatalogCacheState.stale,
      durable: false,
      observedAt: value.observedAt,
      age: age.isNegative ? null : age,
      itemCount: value.values.length,
    );
  }

  @override
  Future<void> clearCache() async {
    if (_clearing) throw const AppFailure(FailureKind.conflict);
    _clearing = true;
    ++_generation;
    _cache = null;
    // Do not rehydrate an old disk value if removal fails. Its actual persisted
    // health remains visible, and a later clear can retry without touching data.
    _hydrated = true;
    for (final token in List.of(_active)) {
      token.cancel();
    }
    try {
      await cache?.clear();
    } finally {
      _clearing = false;
    }
  }

  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    if (_clearing) throw const AppFailure(FailureKind.conflict);
    final request = CancellationToken();
    final removeListener = cancellation.onCancel(request.cancel);
    _active.add(request);
    try {
      final result = await _search(category, query, request);
      request.check();
      return result;
    } finally {
      _active.remove(request);
      removeListener();
    }
  }

  Future<RepositoryRead<EntertainmentTitle>> _search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    if (query.length > 512 || _controls.hasMatch(query)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final now = clock().toUtc();
    if (category != EntertainmentCategory.games) {
      // No film metadata API is configured. Display the bundled public preview
      // with its quality intact; the game endpoint cannot establish film data.
      const unavailable = AppFailure(FailureKind.unsupported);
      try {
        final local = await fallback.search(category, query, cancellation);
        cancellation.check();
        return RepositoryRead(
          local.values,
          quality: switch (local.quality) {
            ReadQuality.complete || ReadQuality.partial => ReadQuality.partial,
            ReadQuality.stale => ReadQuality.stale,
            ReadQuality.unavailable => ReadQuality.unavailable,
          },
          observedAt: local.observedAt,
          failure: local.failure ?? unavailable,
        );
      } catch (error) {
        cancellation.check();
        if (error is AppFailure && error.kind == FailureKind.cancelled) rethrow;
        return RepositoryRead(
          [],
          quality: ReadQuality.unavailable,
          observedAt: now,
          failure: unavailable,
        );
      }
    }
    if (!_hydrated) {
      _hydrated = true;
      final saved = cache?.read(now, retention);
      if (saved != null) {
        try {
          _cache = RepositoryRead(
            PublicCatalogCodec.parse(jsonDecode(saved.payload)),
            quality: ReadQuality.complete,
            observedAt: saved.observedAt,
          );
        } catch (_) {
          // Invalid optional metadata is ignored, never repaired into authority.
        }
      }
    }
    if (_cache != null &&
        (now.isBefore(_cache!.observedAt) ||
            now.difference(_cache!.observedAt) > retention)) {
      _cache = null;
    }
    final cached = _cache;
    if (cached != null &&
        !now.isBefore(cached.observedAt) &&
        now.difference(cached.observedAt) < ttl) {
      return _filter(cached, query);
    }
    final generation = ++_generation;
    try {
      final bytes = await policy.run(
        cancellation,
        () => transport.fetch(cancellation),
      );
      cancellation.check();
      if (bytes.length > maxResponseBytes) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final decoded = jsonDecode(utf8.decode(bytes, allowMalformed: false));
      final values = PublicCatalogCodec.parse(decoded);
      cancellation.check();
      final read = RepositoryRead(
        values,
        quality: ReadQuality.complete,
        observedAt: clock().toUtc(),
      );
      // Separate requests never share cancellation. A slow earlier completion
      // may serve its own caller but cannot replace a newer cache snapshot.
      if (generation == _generation) {
        if (cache != null) {
          // Persist only allowlisted visible metadata, not unknown server fields.
          final payload = jsonEncode([
            for (final entry in decoded as List)
              if (entry['hidden'] == false && entry['kind'] == 'game')
                {
                  for (final key in const [
                    'id',
                    'kind',
                    'title',
                    'description',
                    'status',
                    'categories',
                    'hidden',
                    'revision',
                    'image',
                    'imageNote',
                  ])
                    if (entry.containsKey(key)) key: entry[key],
                },
          ]);
          try {
            await cache!.write(
              PublicCatalogSnapshot(payload, read.observedAt),
              cancellation,
              isCurrent: () => generation == _generation,
            );
          } catch (_) {
            // Saving optional metadata cannot downgrade a valid remote read.
            cancellation.check();
          }
          cancellation.check();
        }
      }
      if (generation == _generation) _cache = read;
      return _filter(read, query);
    } catch (error) {
      cancellation.check();
      final failure = error is AppFailure
          ? error
          : const AppFailure(FailureKind.invalidData);
      if (failure.kind == FailureKind.cancelled) rethrow;
      final previous = _cache;
      final failedAt = clock().toUtc();
      if (previous != null &&
          !failedAt.isBefore(previous.observedAt) &&
          failedAt.difference(previous.observedAt) <= retention) {
        return _filter(
          RepositoryRead(
            previous.values,
            quality: ReadQuality.stale,
            observedAt: previous.observedAt,
            failure: failure,
          ),
          query,
        );
      }
      try {
        final local = await fallback.search(category, query, cancellation);
        cancellation.check();
        return RepositoryRead(
          local.values,
          quality: local.values.isEmpty
              ? ReadQuality.unavailable
              : ReadQuality.partial,
          observedAt: local.observedAt,
          failure: failure,
        );
      } catch (_) {
        cancellation.check();
        return RepositoryRead(
          [],
          quality: ReadQuality.unavailable,
          observedAt: now,
          failure: failure,
        );
      }
    }
  }

  @override
  Future<EntertainmentTitle?> lookup(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  ) async {
    final read = await lookupRead(category, id, cancellation);
    cancellation.check();
    // This legacy lookup contract cannot expose degraded quality. Never silently
    // turn a failed remote lookup into a successful local or stale lookup.
    if (read.quality != ReadQuality.complete) {
      throw read.failure ?? const AppFailure(FailureKind.network);
    }
    return read.values.firstOrNull;
  }

  @override
  Future<RepositoryRead<EntertainmentTitle>> lookupRead(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    if (!_id.hasMatch(id) || id.length > 128) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final read = await search(category, '', cancellation);
    cancellation.check();
    return RepositoryRead(
      read.values.where((value) => value.id == id),
      quality: read.quality,
      observedAt: read.observedAt,
      failure: read.failure,
    );
  }

  static final _controls = RegExp(r'[\x00-\x1f\x7f-\x9f]');
  static final _id = RegExp(r'^[a-z0-9]+(?:-[a-z0-9]+)*$');

  static RepositoryRead<EntertainmentTitle> _filter(
    RepositoryRead<EntertainmentTitle> read,
    String query,
  ) {
    final words = query.trim().toLowerCase().split(RegExp(r'\s+'));
    return RepositoryRead(
      read.values.where(
        (title) => words.every(
          (word) => '${title.title} ${title.tags.join(' ')}'
              .toLowerCase()
              .contains(word),
        ),
      ),
      quality: read.quality,
      observedAt: read.observedAt,
      failure: read.failure,
    );
  }
}
