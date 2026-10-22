enum CatalogCacheState { empty, fresh, stale, expired, invalid, unavailable }

/// Health of the optional public metadata snapshot, not account or rights data.
class CatalogCacheHealth {
  const CatalogCacheHealth({
    required this.state,
    required this.durable,
    this.observedAt,
    this.age,
    this.itemCount,
    this.payloadBytes,
  });
  final CatalogCacheState state;
  final bool durable;
  final DateTime? observedAt;
  final Duration? age;
  final int? itemCount;
  final int? payloadBytes;
}

abstract interface class CatalogMaintenance {
  CatalogCacheHealth cacheHealth();

  /// Clears only public catalogue metadata. Throws on failed persistent removal.
  Future<void> clearCache();
}
