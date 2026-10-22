import 'dart:typed_data';

import '../features/store/artwork_maintenance.dart';
export '../features/store/artwork_maintenance.dart';

class PublicArtworkCached {
  PublicArtworkCached(Uint8List bytes, this.storedAt)
    : bytes = Uint8List.fromList(bytes).asUnmodifiableView();
  final Uint8List bytes;
  final DateTime storedAt;
}

/// Optional anonymous encoded-byte cache; never user data or content authority.
abstract interface class PublicArtworkCache {
  Future<PublicArtworkCached?> get(String key);
  Future<void> put(String key, Uint8List bytes, DateTime storedAt);
  Future<PublicArtworkCacheHealth> health();
  Future<PublicArtworkClearResult> clear();

  /// Acknowledges actual native settlement, including a previously timed-out put.
  Future<void> drain();
}
