import 'dart:typed_data';

import '../core/app_failure.dart';
import 'managed_content_transport.dart';

ManagedContentTransport createManagedContentTransport(Uri source) {
  if (!isManagedContentHttpsSource(source)) {
    throw const AppFailure(FailureKind.invalidData);
  }
  return _Unavailable();
}

class _Unavailable implements ManagedContentTransport {
  @override
  Future<ManagedContentTransferReceipt> transfer({
    required int expectedBytes,
    int offset = 0,
    String? strongEtag,
    required Future<void> Function(String? strongEtag) onResponse,
    required Future<void> Function(Uint8List bytes) onChunk,
    required CancellationToken cancellation,
  }) async {
    cancellation.check();
    throw const AppFailure(FailureKind.unsupported);
  }
}
