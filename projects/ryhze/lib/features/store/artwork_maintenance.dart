import '../../core/app_failure.dart';

class PublicArtworkCacheHealth {
  const PublicArtworkCacheHealth({
    required this.entries,
    required this.bytes,
    required this.preserved,
  });
  final int entries, bytes, preserved;
}

class PublicArtworkClearResult {
  const PublicArtworkClearResult({
    required this.removed,
    required this.preserved,
  });
  final int removed, preserved;
}

class PublicArtworkHealth {
  const PublicArtworkHealth({
    required this.memoryEntries,
    required this.memoryBytes,
    required this.active,
    required this.queued,
    this.disk,
    this.diskFailure,
  });
  final int memoryEntries, memoryBytes, active, queued;
  final PublicArtworkCacheHealth? disk;
  final AppFailure? diskFailure;
}

abstract interface class ArtworkMaintenance {
  Future<PublicArtworkHealth> health();
  Future<PublicArtworkClearResult> clear();
}
