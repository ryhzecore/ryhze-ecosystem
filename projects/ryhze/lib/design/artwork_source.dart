import 'package:flutter/foundation.dart';

import '../core/app_failure.dart';
import 'artwork_reference.dart';

enum ArtworkOrigin { network, memory, disk }

class ArtworkRead {
  ArtworkRead({
    required Uint8List bytes,
    required this.storedAt,
    required this.origin,
    this.stale = false,
  }) : bytes = bytes.asUnmodifiableView();
  final Uint8List bytes;
  final DateTime storedAt;
  final ArtworkOrigin origin;
  final bool stale;
}

/// Presentation depends on this contract, never on an operational provider.
abstract interface class ArtworkSource {
  /// Clear/interruption invalidates visible bytes. The view must not immediately
  /// refill a cleared cache; a new view or explicit retry may request artwork.
  ValueListenable<int> get changes;
  Future<ArtworkRead> read(ArtworkReference reference, CancellationToken token);
}
