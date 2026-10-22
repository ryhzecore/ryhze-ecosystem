import 'dart:typed_data';

import '../core/app_failure.dart';
import '../design/artwork_reference.dart';
import 'public_artwork_transport_stub.dart'
    if (dart.library.io) 'public_artwork_transport_io.dart'
    as platform;

abstract interface class PublicArtworkTransport {
  factory PublicArtworkTransport() => platform.createPublicArtworkTransport();
  Future<Uint8List> fetch(
    ArtworkReference reference,
    CancellationToken cancellation,
  );
}
