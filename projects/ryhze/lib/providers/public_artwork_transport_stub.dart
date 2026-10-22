import 'dart:typed_data';

import '../core/app_failure.dart';
import '../design/artwork_reference.dart';
import 'public_artwork_transport.dart';

PublicArtworkTransport createPublicArtworkTransport() => _Unavailable();

class _Unavailable implements PublicArtworkTransport {
  @override
  Future<Uint8List> fetch(
    ArtworkReference reference,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    throw const AppFailure(FailureKind.unsupported);
  }
}
