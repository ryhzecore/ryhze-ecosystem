import '../../core/app_failure.dart';
import 'managed_installation.dart';

/// Local integrity evidence only. No publisher, entitlement or execution rights.
enum ContentStagingState {
  staged,
  checkpoint,
  cancelled,
  invalid,
  conflict,
  recovery,
  quota,
  unavailable,
}

class ContentStagingRequest {
  ContentStagingRequest({
    required this.slot,
    required this.operationId,
    required this.manifestSha256,
    required this.sourceRelative,
    required this.size,
    required this.sha256,
    this.rootIdentity = '',
    this.sourceIdentity = '',
    this.minimumVerifiedBytes = 0,
  }) {
    if (slot < 0 ||
        slot >= managedStageSlotCount ||
        size < 1 ||
        size > maximumBytes ||
        minimumVerifiedBytes < 0 ||
        minimumVerifiedBytes > size ||
        (minimumVerifiedBytes != size &&
            minimumVerifiedBytes % managedStageChunkBytes != 0) ||
        !digest.hasMatch(operationId) ||
        !digest.hasMatch(manifestSha256) ||
        !digest.hasMatch(sha256) ||
        !_path(sourceRelative) ||
        !validIdentity(rootIdentity) ||
        !validIdentity(sourceIdentity)) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
  static const maximumBytes = managedStageMaximumBytes;
  static bool _path(String value) =>
      value.isNotEmpty &&
      value.length <= 512 &&
      value.split('/').every((part) {
        final stem = part.split('.').first.toUpperCase();
        return RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$').hasMatch(part) &&
            !part.endsWith('.') &&
            !{'CON', 'PRN', 'AUX', 'NUL'}.contains(stem) &&
            !RegExp(r'^(COM|LPT)[0-9]$').hasMatch(stem);
      });
  static final digest = RegExp(r'^[0-9a-f]{64}$');
  static bool validIdentity(String value) =>
      value.isEmpty || RegExp(r'^[0-9a-f]{16}:[0-9a-f]{32}$').hasMatch(value);
  final int slot, size;

  /// Stage rejects loss of this already verified prefix before copying. Inspect
  /// still reports the actual retained bytes, including a shorter prefix.
  final int minimumVerifiedBytes;
  final String operationId, manifestSha256, sourceRelative, sha256;
  final String rootIdentity, sourceIdentity;
}

class ContentStagingReceipt {
  const ContentStagingReceipt({
    required this.state,
    required this.verifiedBytes,
    required this.verifiedChunks,
    required this.rootIdentity,
    required this.sourceIdentity,
  });
  final ContentStagingState state;
  final int verifiedBytes, verifiedChunks;
  final String rootIdentity, sourceIdentity;
}

abstract interface class ContentStagingBackend {
  String get deviceId;
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken cancellation,
  );
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken cancellation,
  );

  /// Requests cancellation; does not establish worker settlement.
  Future<void> cancel();

  /// Completes only after actual native work exits; failure is not settlement.
  Future<void> drain();

  /// Permanently closes admission, cancels, and drains the worker.
  Future<void> close();
}
