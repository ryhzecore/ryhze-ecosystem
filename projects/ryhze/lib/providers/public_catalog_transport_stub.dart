import 'dart:typed_data';

import '../core/app_failure.dart';
import 'public_catalog_transport.dart';

PublicCatalogTransport createPublicCatalogTransport() =>
    _UnsupportedTransport();

class _UnsupportedTransport implements PublicCatalogTransport {
  @override
  Future<Uint8List> fetch(CancellationToken cancellation) async {
    cancellation.check();
    throw const AppFailure(FailureKind.unsupported);
  }
}
