import 'dart:typed_data';

import '../../core/app_failure.dart';
import 'content_staging_backend.dart';
import 'managed_installation.dart';

/// Integrity/ownership observations only; never publisher or execution rights.
enum ContentTransferState { writing, checkpoint, complete, missing, discarded }

class ContentTransferRequest {
  ContentTransferRequest({
    required this.slot,
    required this.operationId,
    required this.expectedSize,
    required this.expectedSha256,
    this.attempt = 0,
    this.rootIdentity = '',
    this.directoryIdentity = '',
    this.sourceIdentity = '',
    this.observedBytes = 0,
    this.observedSha256 = '',
  }) {
    if (slot < 0 ||
        slot >= managedStageSlotCount ||
        attempt < 0 ||
        attempt > 1 ||
        expectedSize < 1 ||
        expectedSize > managedStageMaximumBytes ||
        !ContentStagingRequest.digest.hasMatch(operationId) ||
        !ContentStagingRequest.digest.hasMatch(expectedSha256) ||
        !ContentStagingRequest.validIdentity(rootIdentity) ||
        !ContentStagingRequest.validIdentity(directoryIdentity) ||
        !ContentStagingRequest.validIdentity(sourceIdentity) ||
        observedBytes < 0 ||
        observedBytes > expectedSize ||
        (observedSha256.isNotEmpty &&
            !ContentStagingRequest.digest.hasMatch(observedSha256))) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
  final int slot, attempt, expectedSize, observedBytes;
  final String operationId, expectedSha256;
  final String rootIdentity, directoryIdentity, sourceIdentity, observedSha256;

  String get sourceRelative => 'transfer-$slot/$operationId-$attempt.bin';
}

class ContentTransferReceipt {
  const ContentTransferReceipt({
    required this.state,
    required this.rootIdentity,
    required this.directoryIdentity,
    required this.sourceIdentity,
    required this.sourceRelative,
    required this.observedBytes,
    required this.observedSha256,
  });
  final ContentTransferState state;

  /// For discarded, sourceIdentity/observedBytes/observedSha256 identify the
  /// reconciled durable removal plan, including an already absent file. They
  /// are not a fresh present-file observation in that state.
  final String rootIdentity, directoryIdentity, sourceIdentity, sourceRelative;
  final int observedBytes;
  final String observedSha256;
}

abstract interface class ContentTransferBackend {
  /// Same fixed content-staging directory identity used by ContentStagingBackend.
  String get deviceId;

  /// Creates a new exclusive writer. Optional resume is a freshly verified,
  /// durably recorded old attempt, replayed internally through a pinned handle.
  /// The caller must reserve the operation durably before calling begin.
  /// Persist the returned directory and source identity before network writes.
  /// A present file without that recorded identity cannot be adopted by inspect.
  Future<ContentTransferReceipt> begin(
    ContentTransferRequest request,
    CancellationToken cancellation, {
    ContentTransferRequest? resume,
  });
  Future<ContentTransferReceipt> append(
    ContentTransferRequest request,
    Uint8List bytes,
    CancellationToken cancellation,
  );
  Future<ContentTransferReceipt> finish(
    ContentTransferRequest request,
    CancellationToken cancellation,
  );
  Future<ContentTransferReceipt> inspect(
    ContentTransferRequest request,
    CancellationToken cancellation,
  );

  /// Requires the durable exact directory/file identity, observed size and hash.
  /// Never recursively removes the slot or touches unknown files.
  /// A reservation with no source identity may reconcile only actual absence,
  /// with zero observed bytes and no hash; it never removes a present file.
  Future<ContentTransferReceipt> discard(
    ContentTransferRequest request,
    CancellationToken cancellation,
  );

  /// Requests cancellation only; does not prove settlement.
  Future<void> cancel();

  /// Awaits actual native work and releases an idle writer preserving its bytes.
  Future<void> drain();
  Future<void> close();
}
