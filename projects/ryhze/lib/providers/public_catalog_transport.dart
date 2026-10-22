import 'dart:typed_data';

import '../core/app_failure.dart';
import 'public_catalog_transport_stub.dart'
    if (dart.library.io) 'public_catalog_transport_io.dart'
    as platform;

/// Anonymous public metadata only. No caller-supplied URL, headers or cookies.
abstract interface class PublicCatalogTransport {
  factory PublicCatalogTransport() => platform.createPublicCatalogTransport();
  Future<Uint8List> fetch(CancellationToken cancellation);
}
