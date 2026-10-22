import 'dart:convert';

import '../../core/app_failure.dart';
import '../session/session_controller.dart';
import 'content_transfer_backend.dart';
import 'managed_content_manifest.dart';
import 'managed_installation.dart';

enum ManagedTransferState {
  prepared,
  receiving,
  recoveryRequired,
  downloaded,
  stagingAdmitted,
  discarding,
  discarded,
}

/// Durable incoming-file ownership and integrity, never persisted permission.
class ManagedTransferRecord {
  ManagedTransferRecord._({
    required this.scope,
    required this.deviceId,
    required this.intent,
    required this.manifest,
    required this.sourceKey,
    required this.state,
    required this.revision,
    required this.attempt,
    required this.request,
    required this.previous,
    required this.strongEtag,
    required this.updatedAt,
    required this.discardAttempt,
  });

  final AccountScope scope;
  final String deviceId, sourceKey;
  final ManagedStageIntent intent;
  final ManagedManifestProvenance manifest;
  final ManagedTransferState state;
  final int revision, attempt, discardAttempt;
  final ContentTransferRequest request;
  final ContentTransferRequest? previous;
  final String? strongEtag;
  final DateTime updatedAt;
  String get operationId => intent.operationId;
  String get attemptId => '$operationId.transfer.$attempt';
  String get discardAttemptId =>
      '$operationId.incoming-discard.$discardAttempt';
  bool get reservesContent => state != ManagedTransferState.discarded;
  ManagedStageIntent get stagingIntent => ManagedStageIntent(
    operationId: intent.operationId,
    item: intent.item,
    packageId: intent.packageId,
    version: intent.version,
    manifestSha256: intent.manifestSha256,
    sourceRelative: request.sourceRelative,
    expectedSize: intent.expectedSize,
    expectedSha256: intent.expectedSha256,
    slot: intent.slot,
  );

  static ManagedTransferRecord decode(Map value) {
    try {
      if (value['recordVersion'] is! int ||
          value['recordVersion'] != 1 ||
          (value['manifest'] as Map?)?['version'] != 1) {
        throw const AppFailure(FailureKind.unsupported);
      }
      final account = value['account'] as Map;
      final scope = AccountScope.verified(
        account['authority'] as String,
        account['accountId'] as String,
      );
      validateManagedScope(scope);
      if (scope.isLocal) {
        throw const AppFailure(FailureKind.denied);
      }
      final device = value['deviceId'] as String;
      validateManagedDeviceId(device);
      final intent = ManagedStageIntent.decode(value['intent'] as Map);
      final manifest = ManagedManifestProvenance.decode(value['manifest']);
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
      final sourceKey = value['sourceKey'] as String;
      validateManagedTransferSourceKey(sourceKey);
      final state = ManagedTransferState.values.byName(
        value['state'] as String,
      );
      final revision = value['revision'] as int;
      final attempt = value['attempt'] as int;
      final discardAttempt = value['discardAttempt'] as int? ?? 0;
      final request = decodeTransferRequest(value['request'] as Map);
      final previous = value['previous'] == null
          ? null
          : decodeTransferRequest(value['previous'] as Map);
      final etag = value['strongEtag'] as String?;
      validateManagedTransferEtag(etag);
      final stamp = value['updatedAt'] as String;
      final updated = DateTime.tryParse(stamp);
      if (revision < 1 ||
          revision > 0x7fffffff ||
          attempt < 0 ||
          attempt > 999 ||
          request.attempt != attempt % 2 ||
          discardAttempt < 0 ||
          discardAttempt > 999 ||
          updated == null ||
          !stamp.endsWith('Z') ||
          stamp.length > 32 ||
          !transferRequestMatchesIntent(request, intent) ||
          (previous != null &&
              (!transferRequestMatchesIntent(previous, intent) ||
                  previous.attempt == request.attempt ||
                  previous.directoryIdentity.isEmpty ||
                  (request.directoryIdentity.isNotEmpty &&
                      request.directoryIdentity !=
                          previous.directoryIdentity) ||
                  (request.rootIdentity.isNotEmpty &&
                      request.rootIdentity != previous.rootIdentity))) ||
          ((state == ManagedTransferState.downloaded ||
                  state == ManagedTransferState.stagingAdmitted) &&
              !completeTransferRequest(request)) ||
          (state == ManagedTransferState.discarded && previous != null) ||
          (state == ManagedTransferState.discarding && discardAttempt == 0)) {
        throw const AppFailure(FailureKind.invalidData);
      }
      return ManagedTransferRecord._(
        scope: scope,
        deviceId: device,
        intent: intent,
        manifest: manifest,
        sourceKey: sourceKey,
        state: state,
        revision: revision,
        attempt: attempt,
        request: request,
        previous: previous,
        strongEtag: etag,
        updatedAt: updated,
        discardAttempt: discardAttempt,
      );
    } on AppFailure {
      rethrow;
    } on Object {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
}

bool transferRequestMatchesIntent(
  ContentTransferRequest request,
  ManagedStageIntent intent,
) =>
    request.operationId == intent.operationId &&
    request.slot == intent.slot &&
    request.expectedSize == intent.expectedSize &&
    request.expectedSha256 == intent.expectedSha256;

bool completeTransferRequest(ContentTransferRequest request) =>
    request.rootIdentity.isNotEmpty &&
    request.directoryIdentity.isNotEmpty &&
    request.sourceIdentity.isNotEmpty &&
    request.observedBytes == request.expectedSize &&
    request.observedSha256 == request.expectedSha256;

/// Typed participant of the staging inventory-and-job transaction. The revision
/// binds the reviewed transfer to freshly authorized matching staging metadata.
void admitManagedTransferStageIn(
  Map<String, Object?> data,
  ManagedInstallationRecord stage, {
  int? expectedRevision,
}) {
  final envelope = data['managedTransfers'];
  if (!supportsManagedTransfers(envelope)) {
    throw const AppFailure(FailureKind.unsupported);
  }
  final records = (envelope as Map?)?['records'] as Map?;
  final raw = records?[stage.operationId] as Map?;
  if (raw == null) {
    if (expectedRevision != null) {
      throw const AppFailure(FailureKind.conflict);
    }
    return;
  }
  final transfer = ManagedTransferRecord.decode(raw);
  if (expectedRevision == null ||
      transfer.revision != expectedRevision ||
      transfer.deviceId != stage.deviceId ||
      transfer.scope.key != stage.scope.key ||
      (transfer.state != ManagedTransferState.downloaded &&
          transfer.state != ManagedTransferState.stagingAdmitted) ||
      !completeTransferRequest(transfer.request) ||
      !transfer.stagingIntent.sameRequest(stage.intent) ||
      stage.manifest == null ||
      !transfer.manifest.sameArtifact(stage.manifest!) ||
      transfer.request.rootIdentity != stage.rootIdentity ||
      transfer.request.sourceIdentity != stage.sourceIdentity ||
      stage.state != ManagedInstallationState.prepared) {
    throw const AppFailure(FailureKind.conflict);
  }
  if (transfer.state == ManagedTransferState.downloaded) {
    records![stage.operationId] = {
      ...Map<String, Object?>.from(raw),
      'state': ManagedTransferState.stagingAdmitted.name,
      'revision': transfer.revision + 1,
      'updatedAt': DateTime.now().toUtc().toIso8601String(),
    };
  }
  validateManagedTransfers(envelope);
  validateManagedContentReservations(data);
}

Map<String, Object?> encodeTransferRequest(ContentTransferRequest request) => {
  'slot': request.slot,
  'operationId': request.operationId,
  'expectedSize': request.expectedSize,
  'expectedSha256': request.expectedSha256,
  'attempt': request.attempt,
  'rootIdentity': request.rootIdentity,
  'directoryIdentity': request.directoryIdentity,
  'sourceIdentity': request.sourceIdentity,
  'observedBytes': request.observedBytes,
  'observedSha256': request.observedSha256,
};

ContentTransferRequest decodeTransferRequest(Map value) {
  final result = ContentTransferRequest(
    slot: value['slot'] as int,
    operationId: value['operationId'] as String,
    expectedSize: value['expectedSize'] as int,
    expectedSha256: value['expectedSha256'] as String,
    attempt: value['attempt'] as int,
    rootIdentity: value['rootIdentity'] as String,
    directoryIdentity: value['directoryIdentity'] as String,
    sourceIdentity: value['sourceIdentity'] as String,
    observedBytes: value['observedBytes'] as int,
    observedSha256: value['observedSha256'] as String,
  );
  // File bytes are observations only when tied to exact directory/file identity.
  if ((result.directoryIdentity.isNotEmpty && result.rootIdentity.isEmpty) ||
      (result.sourceIdentity.isNotEmpty && result.directoryIdentity.isEmpty) ||
      (result.observedSha256.isNotEmpty && result.sourceIdentity.isEmpty) ||
      (result.observedBytes != 0 && result.sourceIdentity.isEmpty)) {
    throw const AppFailure(FailureKind.invalidData);
  }
  return result;
}

void validateManagedTransferSourceKey(String value) {
  if (RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$').firstMatch(value)?.end !=
      value.length) {
    throw const AppFailure(FailureKind.invalidData);
  }
}

void validateManagedTransferEtag(String? value) {
  // Strong ASCII entity tags only. Persisted source keys and tags contain no URL
  // or authorization header, and are never interpreted as network authority.
  if (value != null &&
      (value.length > 256 ||
          RegExp(r'^"[\x21\x23-\x7e]*"$').firstMatch(value)?.end !=
              value.length)) {
    throw const AppFailure(FailureKind.invalidData);
  }
}

bool supportsManagedTransfers(Object? value) {
  if (value == null) {
    return true;
  }
  if (value is! Map || value['version'] != 1 || value['records'] is! Map) {
    return false;
  }
  return (value['records'] as Map).values.every(
    (raw) =>
        raw is Map &&
        raw['recordVersion'] is int &&
        raw['recordVersion'] == 1 &&
        raw['manifest'] is Map &&
        (raw['manifest'] as Map)['version'] is int &&
        (raw['manifest'] as Map)['version'] == 1,
  );
}

void validateManagedTransfers(Object? value) {
  if (value == null) {
    return;
  }
  try {
    _bounded(value, 0);
    if (value is! Map ||
        utf8.encode(jsonEncode(value)).length > 1024 * 1024 ||
        value['version'] is! int ||
        (value['version'] as int) < 1 ||
        (value['version'] as int) > 0x7fffffff) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (value['version'] != 1) {
      return;
    }
    final records = value['records'];
    if (records is! Map || records.length > managedInstallationMaximumRecords) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (!supportsManagedTransfers(value)) {
      return;
    }
    var reserve = 0;
    for (final entry in records.entries) {
      final record = ManagedTransferRecord.decode(entry.value as Map);
      if (entry.key != record.operationId) {
        throw const AppFailure(FailureKind.invalidData);
      }
      // Consume reserved headroom as known metadata grows; a fixed reserve would
      // admit a near-limit record and then reject its very first observation.
      if (record.reservesContent) {
        reserve += _remainingRecordGrowth(entry.value as Map, record);
      }
    }
    if (utf8.encode(jsonEncode(value)).length + reserve > 1024 * 1024) {
      throw const AppFailure(FailureKind.conflict);
    }
  } on AppFailure {
    rethrow;
  } on Object {
    throw const AppFailure(FailureKind.invalidData);
  }
}

int _remainingRecordGrowth(Map value, ManagedTransferRecord record) {
  final requestMaximum = <String, Object?>{
    ...encodeTransferRequest(record.request),
    'rootIdentity': '${'f' * 16}:${'f' * 32}',
    'directoryIdentity': '${'f' * 16}:${'f' * 32}',
    'sourceIdentity': '${'f' * 16}:${'f' * 32}',
    'observedBytes': record.intent.expectedSize,
    'observedSha256': 'f' * 64,
  };
  var result = _remainingFields(value, {
    'state': 'recoveryRequired', 'revision': 0x7fffffff,
    'attempt': 999, 'discardAttempt': 999,
    'updatedAt': '0' * 32,
    // Each permitted backslash expands to two JSON bytes.
    'strongEtag': '"${'\\' * 254}"',
  });
  result += _remainingFields(value['request'] as Map, requestMaximum);
  if (value['previous'] is Map) {
    result += _remainingFields(value['previous'] as Map, requestMaximum);
  } else {
    result +=
        _jsonFieldBytes('previous', requestMaximum) -
        (value.containsKey('previous')
            ? _jsonFieldBytes('previous', value['previous'])
            : 0);
  }
  return result;
}

int _remainingFields(Map current, Map<String, Object?> maximum) {
  var result = 0;
  for (final entry in maximum.entries) {
    if (!current.containsKey(entry.key)) {
      result += _jsonFieldBytes(entry.key, entry.value);
    } else {
      final growth =
          utf8.encode(jsonEncode(entry.value)).length -
          utf8.encode(jsonEncode(current[entry.key])).length;
      if (growth > 0) {
        result += growth;
      }
    }
  }
  return result;
}

int _jsonFieldBytes(String key, Object? value) =>
    utf8.encode(jsonEncode({key: value})).length - 1;

/// One device-wide slot/item budget spanning incoming and inert staging. Unknown
/// future capabilities remain preserved; repositories refuse new admission when
/// either inventory cannot be interpreted safely.
void validateManagedContentReservations(Map<String, Object?> data) {
  if (!supportsManagedTransfers(data['managedTransfers']) ||
      !supportsManagedInstallations(data['managedInstallations'])) {
    return;
  }
  final incoming =
      ((data['managedTransfers'] as Map?)?['records'] as Map? ?? {}).values
          .map((raw) => ManagedTransferRecord.decode(raw as Map))
          .where((record) => record.reservesContent)
          .toList();
  final envelope = data['managedInstallations'] as Map?;
  final stages = (envelope?['records'] as Map? ?? {}).values
      .map(
        (raw) => ManagedInstallationRecord.decode(
          raw as Map,
          envelopeVersion: envelope?['version'] as int? ?? 1,
        ),
      )
      .where((record) => record.reservesContent)
      .toList();
  final slots = <String, String>{}, items = <String, String>{};
  final bytes = <String, int>{};
  void reserve(String device, ManagedStageIntent intent, int amount) {
    final slot = jsonEncode([device, intent.slot]);
    final item = jsonEncode([
      device,
      intent.item.provider,
      intent.item.namespace,
      intent.item.item,
    ]);
    if ((slots.containsKey(slot) && slots[slot] != intent.operationId) ||
        (items.containsKey(item) && items[item] != intent.operationId)) {
      throw const AppFailure(FailureKind.conflict);
    }
    slots[slot] = intent.operationId;
    items[item] = intent.operationId;
    bytes[device] = (bytes[device] ?? 0) + amount;
    if (bytes[device]! > managedStageSlotCount * managedStageMaximumBytes) {
      throw const AppFailure(FailureKind.conflict);
    }
  }

  for (final stage in stages) {
    reserve(stage.deviceId, stage.intent, stage.intent.expectedSize);
  }
  for (final record in incoming) {
    final matching = stages
        .where((stage) => stage.operationId == record.operationId)
        .toList();
    if (matching.isNotEmpty) {
      final stage = matching.single;
      if (record.deviceId != stage.deviceId ||
          record.scope.key != stage.scope.key ||
          (record.state != ManagedTransferState.downloaded &&
              record.state != ManagedTransferState.stagingAdmitted &&
              record.state != ManagedTransferState.discarding) ||
          !record.stagingIntent.sameRequest(stage.intent) ||
          (stage.manifest == null ||
              !record.manifest.sameArtifact(stage.manifest!)) ||
          record.request.rootIdentity != stage.rootIdentity ||
          record.request.sourceIdentity != stage.sourceIdentity) {
        throw const AppFailure(FailureKind.conflict);
      }
    }
    reserve(
      record.deviceId,
      record.intent,
      record.intent.expectedSize * (matching.isEmpty ? 3 : 2),
    );
  }
}

void _bounded(Object? value, int depth) {
  if (depth > 24) {
    throw const AppFailure(FailureKind.invalidData);
  }
  if (value is Map) {
    if (value.length > 8192 || value.keys.any((key) => key is! String)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    for (final child in value.values) {
      _bounded(child, depth + 1);
    }
  } else if (value is List) {
    if (value.length > 8192) {
      throw const AppFailure(FailureKind.invalidData);
    }
    for (final child in value) {
      _bounded(child, depth + 1);
    }
  } else if (value != null &&
      value is! String &&
      value is! num &&
      value is! bool) {
    throw const AppFailure(FailureKind.invalidData);
  }
}
