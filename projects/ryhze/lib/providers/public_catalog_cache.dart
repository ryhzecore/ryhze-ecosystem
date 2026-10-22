import 'dart:convert';

import '../core/app_failure.dart';
import '../data/transaction_store.dart';
import 'public_catalog_codec.dart';
import '../features/store/catalog_maintenance.dart';

/// One replaceable public metadata snapshot. The application owns store lifetime.
/// No account data, credentials, entitlement or executable authority is stored.
class PublicCatalogCache {
  PublicCatalogCache(this.store);

  static const storageKey = 'ryhzePublicCatalog';
  static const source = 'https://ryhze.com/api/discover';
  static const maxBytes = 1024 * 1024;
  final TransactionStore store;
  int _epoch = 0;

  CatalogCacheHealth health(DateTime now, Duration ttl, Duration retention) {
    final durable = store.durable;
    try {
      final data = store.read();
      if (!data.containsKey(storageKey)) {
        return CatalogCacheHealth(
          state: CatalogCacheState.empty,
          durable: durable,
        );
      }
      final record = data[storageKey];
      if (record is! Map ||
          record.length != 4 ||
          record['version'] != 1 ||
          record['source'] != source ||
          record['observedAt'] is! String ||
          record['payload'] is! String) {
        return CatalogCacheHealth(
          state: CatalogCacheState.invalid,
          durable: durable,
        );
      }
      final observed = DateTime.tryParse(record['observedAt'] as String);
      final payload = record['payload'] as String;
      if (observed == null ||
          !observed.isUtc ||
          observed.isAfter(now) ||
          payload.length > maxBytes) {
        return CatalogCacheHealth(
          state: CatalogCacheState.invalid,
          durable: durable,
        );
      }
      final bytes = utf8.encode(payload).length;
      if (bytes > maxBytes) {
        return CatalogCacheHealth(
          state: CatalogCacheState.invalid,
          durable: durable,
        );
      }
      try {
        _validate(payload);
      } catch (_) {
        return CatalogCacheHealth(
          state: CatalogCacheState.invalid,
          durable: durable,
        );
      }
      final age = now.difference(observed);
      return CatalogCacheHealth(
        state: age > retention
            ? CatalogCacheState.expired
            : age < ttl
            ? CatalogCacheState.fresh
            : CatalogCacheState.stale,
        durable: durable,
        observedAt: observed,
        age: age,
        itemCount: (jsonDecode(payload) as List).length,
        payloadBytes: bytes,
      );
    } catch (_) {
      return CatalogCacheHealth(
        state: CatalogCacheState.unavailable,
        durable: durable,
      );
    }
  }

  Future<void> clear() async {
    // Invalidate even writes already queued behind another store transaction.
    ++_epoch;
    try {
      await store.transact((data) => data.remove(storageKey));
    } catch (_) {
      throw const AppFailure(FailureKind.storage);
    }
  }

  PublicCatalogSnapshot? read(DateTime now, Duration retention) {
    try {
      final record = store.read()[storageKey];
      if (record is! Map ||
          record.length != 4 ||
          record['version'] != 1 ||
          record['source'] != source ||
          record['observedAt'] is! String ||
          record['payload'] is! String) {
        return null;
      }
      final observedAt = DateTime.tryParse(record['observedAt'] as String);
      final payload = record['payload'] as String;
      if (observedAt == null ||
          !observedAt.isUtc ||
          observedAt.isAfter(now) ||
          now.difference(observedAt) > retention ||
          payload.length > maxBytes ||
          utf8.encode(payload).length > maxBytes) {
        return null;
      }
      _validate(payload);
      return PublicCatalogSnapshot(payload, observedAt);
    } catch (_) {
      // Optional cache corruption/unavailability never invalidates user data.
      return null;
    }
  }

  static void _validate(String payload) {
    final decoded = jsonDecode(payload);
    PublicCatalogCodec.parse(decoded);
    for (final entry in decoded as List) {
      if (entry['kind'] != 'game' ||
          entry['hidden'] != false ||
          (entry as Map).keys.any(
            (key) => !const {
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
            }.contains(key),
          )) {
        throw const AppFailure(FailureKind.invalidData);
      }
    }
  }

  Future<void> write(
    PublicCatalogSnapshot snapshot,
    CancellationToken cancellation, {
    required bool Function() isCurrent,
  }) async {
    cancellation.check();
    final epoch = _epoch;
    if (snapshot.payload.length > maxBytes ||
        utf8.encode(snapshot.payload).length > maxBytes) {
      throw const AppFailure(FailureKind.invalidData);
    }
    _validate(snapshot.payload);
    await store.transact((data) {
      cancellation.check();
      if (epoch != _epoch || !isCurrent()) return;
      data[storageKey] = {
        'version': 1,
        'source': source,
        'observedAt': snapshot.observedAt.toUtc().toIso8601String(),
        'payload': snapshot.payload,
      };
    });
    cancellation.check();
  }
}

class PublicCatalogSnapshot {
  const PublicCatalogSnapshot(this.payload, this.observedAt);
  final String payload;
  final DateTime observedAt;
}
