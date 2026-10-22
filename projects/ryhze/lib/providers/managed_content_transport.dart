import 'dart:typed_data';

import '../core/app_failure.dart';
import 'managed_content_transport_stub.dart'
    if (dart.library.io) 'managed_content_transport_io.dart'
    as platform;

const managedTransferMaximumBytes = 1024 * 1024 * 1024;
const managedTransferMaximumChunkBytes = 256 * 1024;

/// A transport is bound to one composition-selected resource. Constructing it
/// does not authenticate a manifest, establish ownership or authorize writing.
/// Admission and the native writer independently enforce those boundaries.
abstract interface class ManagedContentTransport {
  factory ManagedContentTransport.approvedHttps(Uri source) =>
      platform.createManagedContentTransport(source);

  /// [offset] is an independently revalidated retained prefix, never a claim
  /// made by the server. A resumed response must match [strongEtag] exactly.
  ///
  /// Callbacks are serialized and awaited. Cancellation aborts network work but
  /// does not complete this future until an outstanding callback has settled.
  /// [onResponse] runs only after status, lengths, encoding and range validation,
  /// before any bytes reach [onChunk]. This transport neither hashes nor stores
  /// bytes: the admitted native writer verifies the complete resulting payload.
  Future<ManagedContentTransferReceipt> transfer({
    required int expectedBytes,
    int offset = 0,
    String? strongEtag,
    required Future<void> Function(String? strongEtag) onResponse,
    required Future<void> Function(Uint8List bytes) onChunk,
    required CancellationToken cancellation,
  });
}

class ManagedContentTransferReceipt {
  const ManagedContentTransferReceipt({
    required this.totalBytes,
    required this.transferredBytes,
    required this.strongEtag,
  });

  final int totalBytes, transferredBytes;
  final String? strongEtag;
}

/// Bounded ASCII strong entity tags only. Weak validators cannot establish
/// byte-for-byte continuity. Tags are opaque, not authenticity or ownership
/// evidence, and must not be displayed or logged.
bool isManagedContentStrongEtag(String value) =>
    value.length <= 256 &&
    RegExp(r'^"[\x21\x23-\x7e]*"$').firstMatch(value)?.end == value.length;

/// No credential-bearing queries, user information, fragments or cleartext
/// production resources. Endpoint selection belongs to application composition.
bool isManagedContentHttpsSource(Uri source) =>
    source.scheme == 'https' &&
    source.hasAuthority &&
    source.host.isNotEmpty &&
    source.userInfo.isEmpty &&
    !source.hasQuery &&
    !source.hasFragment;
