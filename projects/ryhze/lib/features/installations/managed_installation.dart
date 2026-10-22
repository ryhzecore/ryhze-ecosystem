import 'dart:convert';
import 'dart:typed_data';

import '../../core/app_failure.dart';
import '../../domain/contracts.dart';
import '../session/session_controller.dart';
import 'managed_content_manifest.dart';

const managedStageMaximumBytes = 1024 * 1024 * 1024;
const managedStageChunkBytes = 256 * 1024;
const managedStageSlotCount = 8;
const managedInstallationMaximumRecords = 128;

/// This inventory describes inert staging, never installation, entitlement,
/// authenticated distribution, executable validation, or launch authority.
enum ManagedInstallationState {
  prepared,
  resumePrepared,
  staging,
  staged,
  recoveryRequired,
  cancelled,
  discardPrepared,
  discarding,
  discardRecoveryRequired,
  discarded,
}

bool isManagedDiscardState(ManagedInstallationState state) => switch (state) {
  ManagedInstallationState.discardPrepared ||
  ManagedInstallationState.discarding ||
  ManagedInstallationState.discardRecoveryRequired ||
  ManagedInstallationState.discarded => true,
  _ => false,
};

/// Durable deletion intent. Native code alone interprets and verifies the plan.
/// Retain it after completion so stale jobs cannot acquire a newly reused slot.
class ManagedDiscardMetadata {
  ManagedDiscardMetadata._(
    this.plan,
    this.attempt,
    this.verifiedBytes,
    this.verifiedChunks,
    this.removedBytes,
    this.removedFiles,
  );
  final Uint8List plan;
  final int attempt, verifiedBytes, verifiedChunks, removedBytes, removedFiles;
  int get fileCount => verifiedChunks + 1;

  static ManagedDiscardMetadata decode(Map value, ManagedStageIntent intent) {
    final encoded = value['plan'];
    if (value['version'] != 1 ||
        encoded is! String ||
        encoded.isEmpty ||
        encoded.length > 699052) {
      throw const AppFailure(FailureKind.invalidData);
    }
    late Uint8List plan;
    try {
      plan = base64Decode(encoded);
    } on FormatException {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (plan.isEmpty ||
        plan.length > 512 * 1024 ||
        base64Encode(plan) != encoded) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final attempt = value['attempt'] as int;
    final bytes = value['verifiedBytes'] as int;
    final chunks = value['verifiedChunks'] as int;
    final removedBytes = value['removedBytes'] as int;
    final removedFiles = value['removedFiles'] as int;
    validateManagedCheckpoint(intent, bytes, chunks);
    if (attempt < 1 ||
        attempt > 999 ||
        removedBytes < 0 ||
        removedBytes > bytes ||
        removedFiles < 0 ||
        removedFiles > chunks + 1) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return ManagedDiscardMetadata._(
      plan.asUnmodifiableView(),
      attempt,
      bytes,
      chunks,
      removedBytes,
      removedFiles,
    );
  }
}

enum ManagedStageObservationState {
  staged,
  checkpoint,
  cancelled,
  invalid,
  conflict,
  recovery,
  quota,
  unavailable,
}

class ManagedStageIntent {
  ManagedStageIntent({
    required this.operationId,
    required this.item,
    required this.packageId,
    required this.version,
    required this.manifestSha256,
    required this.sourceRelative,
    required this.expectedSize,
    required this.expectedSha256,
    required this.slot,
  }) {
    _digest(operationId);
    _digest(manifestSha256);
    _digest(expectedSha256);
    _text(packageId);
    _text(version);
    _sourcePath(sourceRelative);
    if (expectedSize < 1 ||
        expectedSize > managedStageMaximumBytes ||
        slot < 0 ||
        slot >= managedStageSlotCount) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  final String operationId, packageId, version, manifestSha256;
  final ProviderItemRef item;

  /// Relative to native composition's fixed private incoming directory. The
  /// Windows adapter translates '/' once; exact spelling remains significant.
  final String sourceRelative;
  final int expectedSize, slot;
  final String expectedSha256;

  int get expectedChunks =>
      (expectedSize + managedStageChunkBytes - 1) ~/ managedStageChunkBytes;

  Map<String, Object?> toJson() => {
    'operationId': operationId,
    'item': {
      'provider': item.provider,
      'item': item.item,
      'namespace': item.namespace,
    },
    'packageId': packageId,
    'version': version,
    'manifestSha256': manifestSha256,
    'sourceRelative': sourceRelative,
    'expectedSize': expectedSize,
    'expectedSha256': expectedSha256,
    'slot': slot,
  };

  static ManagedStageIntent decode(Map value) {
    final item = value['item'] as Map;
    return ManagedStageIntent(
      operationId: value['operationId'] as String,
      item: ProviderItemRef(
        item['provider'] as String,
        item['item'] as String,
        namespace: item['namespace'] as String,
      ),
      packageId: value['packageId'] as String,
      version: value['version'] as String,
      manifestSha256: value['manifestSha256'] as String,
      sourceRelative: value['sourceRelative'] as String,
      expectedSize: value['expectedSize'] as int,
      expectedSha256: value['expectedSha256'] as String,
      slot: value['slot'] as int,
    );
  }

  bool sameRequest(ManagedStageIntent other) =>
      jsonEncode(toJson()) == jsonEncode(other.toJson());
}

/// A native adapter binds its response to the exact request operationId.
/// Identities are observations while handles were pinned, not reusable handles.
class ManagedStageObservation {
  ManagedStageObservation({
    required this.operationId,
    required this.state,
    required this.verifiedBytes,
    required this.verifiedChunks,
    required this.rootIdentity,
    required this.sourceIdentity,
  }) {
    _digest(operationId);
    if (rootIdentity.isNotEmpty) _nativeIdentity(rootIdentity);
    if (sourceIdentity.isNotEmpty) _nativeIdentity(sourceIdentity);
    if (verifiedBytes < 0 ||
        verifiedBytes > managedStageMaximumBytes ||
        verifiedChunks < 0 ||
        verifiedChunks > managedStageMaximumBytes ~/ managedStageChunkBytes) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  final String operationId, rootIdentity, sourceIdentity;
  final ManagedStageObservationState state;
  final int verifiedBytes, verifiedChunks;

  bool get hasIdentities =>
      rootIdentity.isNotEmpty && sourceIdentity.isNotEmpty;
}

class ManagedInstallationRecord {
  ManagedInstallationRecord._({
    required this.scope,
    required this.deviceId,
    required this.intent,
    required this.rootIdentity,
    required this.sourceIdentity,
    required this.state,
    required this.revision,
    required this.verifiedBytes,
    required this.verifiedChunks,
    required this.updatedAt,
    required this.attempt,
    required this.resumeFloorBytes,
    required this.discard,
    required this.manifest,
  });

  final AccountScope scope;
  final String deviceId;
  final ManagedStageIntent intent;
  final String rootIdentity, sourceIdentity;
  final ManagedInstallationState state;
  final int revision, verifiedBytes, verifiedChunks;
  final int attempt, resumeFloorBytes;
  final ManagedDiscardMetadata? discard;
  final ManagedManifestProvenance? manifest;
  final DateTime updatedAt;
  String get operationId => intent.operationId;
  String get owningInstaller => 'ryhze';
  String get attemptId => managedAttemptId(operationId, attempt);
  String? get discardAttemptId =>
      discard == null ? null : '$operationId.discard.${discard!.attempt}';

  /// Staged and interrupted content retains its slot. Only cancellation before
  /// effects or verified native discard completion releases the reservation.
  bool get reservesContent =>
      state != ManagedInstallationState.cancelled &&
      state != ManagedInstallationState.discarded;

  static ManagedInstallationRecord decode(
    Map value, {
    int envelopeVersion = 1,
  }) {
    final account = value['account'] as Map;
    final scope = AccountScope.verified(
      account['authority'] as String,
      account['accountId'] as String,
    );
    validateManagedScope(scope);
    final deviceId = value['deviceId'] as String;
    validateManagedDeviceId(deviceId);
    final intent = ManagedStageIntent.decode(value['intent'] as Map);
    ManagedManifestProvenance? manifest;
    final hasMarker = value.containsKey('recordVersion');
    final hasManifest = value.containsKey('managedManifest');
    if (envelopeVersion == 2 && hasMarker != hasManifest) {
      throw const AppFailure(FailureKind.unsupported);
    }
    if (envelopeVersion == 2 && hasMarker) {
      if (value['recordVersion'] is! int || value['recordVersion'] != 2) {
        throw const AppFailure(FailureKind.unsupported);
      }
      manifest = ManagedManifestProvenance.decode(value['managedManifest']);
      if (!manifest.manifest.binds(
        item: intent.item,
        packageId: intent.packageId,
        packageVersion: intent.version,
        manifestSha256: intent.manifestSha256,
        payloadSize: intent.expectedSize,
        payloadSha256: intent.expectedSha256,
      )) {
        throw const AppFailure(FailureKind.invalidData);
      }
    }
    final rootIdentity = value['rootIdentity'] as String;
    final sourceIdentity = value['sourceIdentity'] as String;
    _nativeIdentity(rootIdentity);
    _nativeIdentity(sourceIdentity);
    final state = ManagedInstallationState.values.byName(
      value['state'] as String,
    );
    final revision = value['revision'] as int;
    final bytes = value['verifiedBytes'] as int;
    final chunks = value['verifiedChunks'] as int;
    final attempt = value['attempt'] as int? ?? 0;
    final resumeFloorBytes = value['resumeFloorBytes'] as int? ?? bytes;
    final discard = value['discard'] == null
        ? null
        : ManagedDiscardMetadata.decode(value['discard'] as Map, intent);
    if (isManagedDiscardState(state) != (discard != null) ||
        (state == ManagedInstallationState.discarded &&
            (discard!.removedBytes != discard.verifiedBytes ||
                discard.removedFiles != discard.fileCount))) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final timestamp = value['updatedAt'] as String;
    final updatedAt = DateTime.tryParse(timestamp);
    if (value['owningInstaller'] != 'ryhze' ||
        revision < 1 ||
        revision > 0x7fffffff ||
        attempt < 0 ||
        attempt > 999 ||
        resumeFloorBytes < 0 ||
        resumeFloorBytes > intent.expectedSize ||
        updatedAt == null ||
        !timestamp.endsWith('Z') ||
        timestamp.length > 32) {
      throw const AppFailure(FailureKind.invalidData);
    }
    validateManagedCheckpoint(intent, bytes, chunks);
    validateManagedCheckpoint(
      intent,
      resumeFloorBytes,
      (resumeFloorBytes + managedStageChunkBytes - 1) ~/ managedStageChunkBytes,
    );
    if ((state == ManagedInstallationState.resumePrepared && attempt == 0) ||
        ((state == ManagedInstallationState.prepared ||
                state == ManagedInstallationState.cancelled) &&
            attempt != 0)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if ((state == ManagedInstallationState.prepared ||
            state == ManagedInstallationState.cancelled) &&
        (bytes != 0 || chunks != 0)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (state == ManagedInstallationState.staged &&
        (bytes != intent.expectedSize || chunks != intent.expectedChunks)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return ManagedInstallationRecord._(
      scope: scope,
      deviceId: deviceId,
      intent: intent,
      rootIdentity: rootIdentity,
      sourceIdentity: sourceIdentity,
      state: state,
      revision: revision,
      verifiedBytes: bytes,
      verifiedChunks: chunks,
      updatedAt: updatedAt,
      attempt: attempt,
      resumeFloorBytes: resumeFloorBytes,
      discard: discard,
      manifest: manifest,
    );
  }
}

String managedAttemptId(String operationId, int attempt) =>
    attempt == 0 ? operationId : '$operationId.retry.$attempt';

void validateManagedScope(AccountScope scope) {
  _text(scope.authority);
  _text(scope.accountId);
}

void validateManagedDeviceId(String deviceId) => _text(deviceId);

/// Counts describe complete reread-verified chunks, with one shorter final chunk.
void validateManagedCheckpoint(
  ManagedStageIntent intent,
  int bytes,
  int chunks,
) {
  if (bytes < 0 ||
      bytes > intent.expectedSize ||
      chunks < 0 ||
      chunks > intent.expectedChunks ||
      bytes !=
          (chunks == intent.expectedChunks
              ? intent.expectedSize
              : chunks * managedStageChunkBytes)) {
    throw const AppFailure(FailureKind.invalidData);
  }
}

/// Optional versioned extension of the existing store schema. Unknown future
/// envelope versions remain bounded and preserved; this client cannot use them.
void validateManagedInstallations(Object? value) {
  if (value == null) return;
  try {
    _boundedJson(value, 0);
    if (utf8.encode(jsonEncode(value)).length > 1024 * 1024 || value is! Map) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final version = value['version'];
    if (version is! int || version < 1 || version > 0x7fffffff) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (version != 1 && version != 2) return;
    final records = value['records'];
    if (records is! Map || records.length > managedInstallationMaximumRecords) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (version == 2 && !supportsManagedInstallations(value)) return;
    var futureCleanup = 0;
    final roots = <String>{};
    final items = <String>{};
    for (final entry in records.entries) {
      final record = ManagedInstallationRecord.decode(
        entry.value as Map,
        envelopeVersion: version,
      );
      if (entry.key != record.operationId) {
        throw const AppFailure(FailureKind.invalidData);
      }
      if (version == 2 && record.reservesContent) {
        // Reserve remaining growth, so later mutations consume their headroom.
        futureCleanup += _managedFutureGrowth(entry.value as Map, record);
      }
      if (record.reservesContent) {
        if (!roots.add(managedRootReservation(record)) ||
            !items.add(managedItemReservation(record))) {
          throw const AppFailure(FailureKind.invalidData);
        }
      }
    }
    if (version == 2 &&
        utf8.encode(jsonEncode(value)).length + futureCleanup > 1024 * 1024) {
      throw const AppFailure(FailureKind.conflict);
    }
  } on AppFailure {
    rethrow;
  } on Object {
    throw const AppFailure(FailureKind.invalidData);
  }
}

/// Unknown optional envelope/record/provenance versions stay preserved, with
/// only this capability disabled. They must never fall back to inert execution.
bool supportsManagedInstallations(Object? value) {
  if (value == null) return true;
  if (value is! Map || value['version'] is! int) return false;
  if (value['version'] == 1) return true;
  if (value['version'] != 2 || value['records'] is! Map) return false;
  for (final raw in (value['records'] as Map).values) {
    if (raw is! Map) return false;
    final hasMarker = raw.containsKey('recordVersion');
    final hasManifest = raw.containsKey('managedManifest');
    if (!hasMarker && !hasManifest) continue;
    if (!hasMarker ||
        !hasManifest ||
        raw['recordVersion'] is! int ||
        raw['recordVersion'] != 2 ||
        raw['managedManifest'] is! Map ||
        (raw['managedManifest'] as Map)['version'] is! int ||
        (raw['managedManifest'] as Map)['version'] != 1) {
      return false;
    }
  }
  return true;
}

// At most 270 bytes of future metadata growth, excluding a first plan's
// 699052 base64 characters. Count remaining growth rather than charging the
// entire reserve again after each transition. Unknown fields stay in actual
// encoded bytes and are never overwritten by these bounded mutations.
int _managedFutureGrowth(Map value, ManagedInstallationRecord record) {
  var growth = _remainingJsonFieldGrowth(value, const {
    'state': 'discardRecoveryRequired',
    'revision': 0x7fffffff,
    'verifiedBytes': managedStageMaximumBytes,
    'verifiedChunks': managedStageMaximumBytes ~/ managedStageChunkBytes,
    'resumeFloorBytes': managedStageMaximumBytes,
    'updatedAt': '00000000000000000000000000000000', // Maximum encoded width.
    'attempt': 999,
  });
  final discard = record.discard;
  if (discard == null) {
    final future = {
      'version': 1,
      'plan': '',
      'attempt': 999,
      'verifiedBytes': managedStageMaximumBytes,
      'verifiedChunks': managedStageMaximumBytes ~/ managedStageChunkBytes,
      'removedBytes': managedStageMaximumBytes,
      'removedFiles': managedStageMaximumBytes ~/ managedStageChunkBytes + 1,
    };
    growth += 699052 + _jsonFieldBytes('discard', future);
    if (value.containsKey('discard')) {
      growth -= _jsonFieldBytes('discard', value['discard']);
    }
  } else {
    // Retried plans/verified counts are immutable. Only attempt and removal
    // counters can grow inside the existing object; its unknown fields remain.
    growth += _remainingJsonFieldGrowth(value['discard'] as Map, {
      'attempt': 999,
      'removedBytes': discard.verifiedBytes,
      'removedFiles': discard.fileCount,
    });
  }
  return growth;
}

int _remainingJsonFieldGrowth(Map current, Map<String, Object?> maximum) {
  var growth = 0;
  for (final entry in maximum.entries) {
    if (!current.containsKey(entry.key)) {
      growth += _jsonFieldBytes(entry.key, entry.value);
    } else {
      final difference =
          utf8.encode(jsonEncode(entry.value)).length -
          utf8.encode(jsonEncode(current[entry.key])).length;
      if (difference > 0) growth += difference;
    }
  }
  return growth;
}

// All affected parent objects are nonempty: include a comma and field framing,
// exclude the temporary outer braces. ASCII base64 needs no JSON escaping.
int _jsonFieldBytes(String key, Object? value) =>
    utf8.encode(jsonEncode({key: value})).length - 1;

String managedRootReservation(ManagedInstallationRecord record) =>
    jsonEncode([record.deviceId, record.rootIdentity, record.intent.slot]);

String managedItemReservation(ManagedInstallationRecord record) => jsonEncode([
  record.deviceId,
  record.intent.item.provider,
  record.intent.item.namespace,
  record.intent.item.item,
]);

void _text(String value) {
  if (value.trim().isEmpty ||
      value.length > 256 ||
      RegExp(r'[\x00-\x1f\x7f]').hasMatch(value)) {
    throw const AppFailure(FailureKind.invalidData);
  }
}

void _digest(String value) {
  if (!RegExp(r'^[0-9a-f]{64}$').hasMatch(value)) {
    throw const AppFailure(FailureKind.invalidData);
  }
}

void _nativeIdentity(String value) {
  if (!RegExp(r'^[0-9a-f]{16}:[0-9a-f]{32}$').hasMatch(value)) {
    throw const AppFailure(FailureKind.invalidData);
  }
}

void _sourcePath(String value) {
  if (value.isEmpty || value.length > 512) {
    throw const AppFailure(FailureKind.invalidData);
  }
  for (final part in value.split('/')) {
    final stem = part.split('.').first.toUpperCase();
    if (!RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$').hasMatch(part) ||
        part.endsWith('.') ||
        {'CON', 'PRN', 'AUX', 'NUL'}.contains(stem) ||
        RegExp(r'^(COM|LPT)[0-9]$').hasMatch(stem)) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
}

void _boundedJson(Object? value, int depth) {
  if (depth > 24) throw const AppFailure(FailureKind.invalidData);
  if (value is Map) {
    if (value.length > 8192 || value.keys.any((key) => key is! String)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    for (final child in value.values) {
      _boundedJson(child, depth + 1);
    }
  } else if (value is List) {
    if (value.length > 8192) throw const AppFailure(FailureKind.invalidData);
    for (final child in value) {
      _boundedJson(child, depth + 1);
    }
  } else if (value != null &&
      value is! String &&
      value is! bool &&
      value is! num) {
    throw const AppFailure(FailureKind.invalidData);
  }
}
