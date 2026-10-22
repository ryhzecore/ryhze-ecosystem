import '../../core/app_failure.dart';
import '../../domain/contracts.dart';
import '../entertainment/model/catalog.dart';

abstract interface class CatalogRepository {
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  );
}

abstract interface class CatalogLookupRepository {
  Future<EntertainmentTitle?> lookup(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  );
}

/// Optional exact-ID lookup for display metadata whose quality must stay visible.
/// Empty degraded reads do not prove absence. No read grants ownership, playback,
/// installation or launch authority; callers retain these separate boundaries.
abstract interface class CatalogMetadataLookupRepository {
  Future<RepositoryRead<EntertainmentTitle>> lookupRead(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  );
}

class LocalCatalogRepository
    implements CatalogRepository, CatalogLookupRepository {
  @override
  Future<EntertainmentTitle?> lookup(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    for (final item in Catalog.search('', category: category)) {
      if (item.id == id) return item;
    }
    return null;
  }

  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    return RepositoryRead(
      Catalog.search(query, category: category),
      quality: ReadQuality.complete,
      observedAt: DateTime.now().toUtc(),
    );
  }
}
