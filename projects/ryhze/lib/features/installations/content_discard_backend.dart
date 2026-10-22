import 'dart:typed_data';

import '../../core/app_failure.dart';
import 'content_staging_backend.dart';

const contentDiscardMaximumPlanBytes = 512 * 1024;

/// Ready is an inspection-only result: all remaining planned files validate.
/// Recovery, including corruption, never grants permission to continue removal.
enum ContentDiscardState {
  ready,
  completed,
  cancelled,
  invalid,
  conflict,
  recovery,
  unavailable,
}

class ContentDiscardPlanReceipt {
  ContentDiscardPlanReceipt({
    required this.state,
    required Uint8List? plan,
    required this.verifiedBytes,
    required this.verifiedChunks,
  }) : plan = plan == null
           ? null
           : Uint8List.fromList(plan).asUnmodifiableView();
  final ContentStagingState state;
  final Uint8List? plan;
  final int verifiedBytes, verifiedChunks;
}

class ContentDiscardReceipt {
  const ContentDiscardReceipt({
    required this.state,
    required this.removedBytes,
    required this.removedFiles,
  });
  final ContentDiscardState state;

  /// Cumulative planned payload bytes/files observed absent; checkpoint is one
  /// file, slot is not. Counts are observations, not proof of complete removal.
  final int removedBytes, removedFiles;
}

/// Native plan is opaque immutable data bound to its exact managed record.
/// This separate optional capability does not change staging-only backends.
abstract interface class ContentDiscardBackend {
  bool get discardAvailable;
  Future<ContentDiscardPlanReceipt> planDiscard(
    ContentStagingRequest request,
    CancellationToken cancellation,
  );
  Future<ContentDiscardReceipt> discard(
    Uint8List plan,
    CancellationToken cancellation,
  );
  Future<ContentDiscardReceipt> inspectDiscard(
    Uint8List plan,
    CancellationToken cancellation,
  );
  Future<void> cancel();
  Future<void> drain();
  Future<void> close();
}
