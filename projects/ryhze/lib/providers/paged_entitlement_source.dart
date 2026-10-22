import '../core/app_failure.dart';
import '../domain/contracts.dart';
import '../features/library/data/evidence_library_repository.dart';
import '../features/session/session_controller.dart';
import 'provider_request_policy.dart';

class EntitlementPage {
  EntitlementPage(Iterable<Entitlement> records, {this.nextCursor})
    : records = List.unmodifiable(records);
  final List<Entitlement> records;
  final String? nextCursor;
}

/// Operational implementations still need approved API access and parser tests.
/// Fixture status is explicit and cannot be removed by pagination or retries.
abstract interface class EntitlementPageAdapter {
  String get authority;
  String get provider;
  bool get fixture;
  Future<EntitlementPage> fetch(
    AccountScope scope,
    String? cursor,
    CancellationToken cancellation,
  );
}

class PagedEntitlementSource implements EntitlementSource {
  PagedEntitlementSource(
    this.adapter, {
    ProviderRequestPolicy? policy,
    this.maxPages = 100,
    this.maxItems = 20000,
    DateTime Function()? clock,
  }) : policy = policy ?? ProviderRequestPolicy(),
       clock = clock ?? DateTime.now {
    if ([adapter.authority, adapter.provider].any(
          (value) =>
              value.isEmpty ||
              value.length > 256 ||
              RegExp(r'[\x00-\x1f]').hasMatch(value),
        ) ||
        maxPages < 1 ||
        maxPages > 100 ||
        maxItems < 1 ||
        maxItems > 20000) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
  final EntitlementPageAdapter adapter;
  final ProviderRequestPolicy policy;
  final int maxPages, maxItems;
  final DateTime Function() clock;
  @override
  String get authority => adapter.authority;
  @override
  String get provider => adapter.provider;
  @override
  Future<RepositoryRead<Entitlement>> read(
    AccountScope scope,
    CancellationToken cancellation,
  ) async {
    final records = <Entitlement>[];
    final identities = <ProviderItemRef>{};
    final cursors = <String>{};
    String? cursor;
    final now = clock().toUtc();
    RepositoryRead<Entitlement> result(
      ReadQuality quality, {
      AppFailure? failure,
    }) => RepositoryRead(
      records,
      quality: quality,
      observedAt: now,
      failure: failure,
    );
    for (var pageIndex = 0; pageIndex < maxPages; pageIndex++) {
      cancellation.check();
      try {
        final page = await policy.run(
          cancellation,
          () => adapter.fetch(scope, cursor, cancellation),
        );
        cancellation.check();
        if (records.length + page.records.length > maxItems) {
          return result(
            records.isEmpty ? ReadQuality.unavailable : ReadQuality.partial,
            failure: const AppFailure(FailureKind.unsupported),
          );
        }
        for (final record in page.records) {
          if (record.account != scope.key ||
              record.authority != authority ||
              record.item.provider != provider ||
              record.fixture != adapter.fixture ||
              record.observedAt.isAfter(now) ||
              !identities.add(record.item)) {
            throw const AppFailure(FailureKind.invalidData);
          }
          records.add(record);
        }
        cursor = page.nextCursor;
        if (cursor == null) return result(ReadQuality.complete);
        if (cursor.isEmpty || cursor.length > 2048 || !cursors.add(cursor)) {
          throw const AppFailure(FailureKind.invalidData);
        }
      } on AppFailure catch (failure) {
        if (failure.kind == FailureKind.cancelled) rethrow;
        if (failure.kind == FailureKind.invalidData ||
            failure.kind == FailureKind.authentication ||
            failure.kind == FailureKind.denied) {
          return RepositoryRead(
            [],
            quality: ReadQuality.unavailable,
            observedAt: now,
            failure: failure,
          );
        }
        return result(
          records.isEmpty ? ReadQuality.unavailable : ReadQuality.partial,
          failure: failure,
        );
      } catch (_) {
        // Unclassified exceptions are parser/adapter failures, never ownership.
        return RepositoryRead(
          [],
          quality: ReadQuality.unavailable,
          observedAt: now,
          failure: const AppFailure(FailureKind.invalidData),
        );
      }
    }
    return result(ReadQuality.partial);
  }
}
