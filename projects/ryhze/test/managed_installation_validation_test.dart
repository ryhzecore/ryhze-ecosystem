import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/managed_installation.dart';

void main() {
  ManagedStageIntent intent({
    String source = 'payload.bin',
    int size = 1,
    int slot = 0,
    String? digest,
  }) => ManagedStageIntent(
    operationId: '1'.padLeft(64, '0'),
    item: ProviderItemRef('Provider', 'Title', namespace: 'Games'),
    packageId: 'Package',
    version: 'Version',
    manifestSha256: digest ?? '2'.padLeft(64, '0'),
    sourceRelative: source,
    expectedSize: size,
    expectedSha256: '3'.padLeft(64, '0'),
    slot: slot,
  );

  final invalid = throwsA(
    isA<AppFailure>().having(
      (failure) => failure.kind,
      'kind',
      FailureKind.invalidData,
    ),
  );

  test('bounded payload and fixed slots match native staging contract', () {
    final maximum = intent(size: managedStageMaximumBytes, slot: 7);
    expect(maximum.expectedChunks, 4096);
    for (final size in [-1, 0, managedStageMaximumBytes + 1]) {
      expect(() => intent(size: size), invalid);
    }
    for (final slot in [-1, 8]) {
      expect(() => intent(slot: slot), invalid);
    }
    expect(() => intent(digest: 'A' * 64), invalid);
    expect(() => intent(digest: '0' * 63), invalid);
  });

  test(
    'relative source rejects traversal aliases devices and alternate streams',
    () {
      for (final source in [
        '',
        '/payload',
        '../payload',
        'folder/../payload',
        'folder/./payload',
        'C:/payload',
        r'folder\payload',
        '//host/share',
        'folder//payload',
        'payload:stream',
        'payload.',
        'payload ',
        'NUL.bin',
        'con.txt',
        'folder/COM0.bin',
        'LPT9',
        'a' * 256,
        'payload?query',
      ]) {
        expect(() => intent(source: source), invalid, reason: source);
      }
      expect(
        intent(source: 'Payload.v1/final-part_2.bin').sourceRelative,
        'Payload.v1/final-part_2.bin',
      );
    },
  );

  test('native identities are exact lower hex observations', () {
    for (final identity in [
      '7:8',
      '0' * 49,
      '${'A' * 16}:${'0' * 32}',
      '${'0' * 16}:${'0' * 31}',
    ]) {
      expect(
        () => ManagedStageObservation(
          operationId: '1'.padLeft(64, '0'),
          state: ManagedStageObservationState.checkpoint,
          verifiedBytes: 0,
          verifiedChunks: 0,
          rootIdentity: identity,
          sourceIdentity: identity,
        ),
        invalid,
      );
    }
  });

  test(
    'schema extension rejects malformed known metadata and remains bounded',
    () {
      validateManagedInstallations(null);
      validateManagedInstallations({'version': 1, 'records': {}});
      validateManagedInstallations({
        'version': 9,
        'future': ['preserved'],
      });
      for (final value in [
        {},
        {'version': 0},
        {'version': 1, 'records': []},
        {
          'version': 1,
          'records': {'operation': {}},
        },
        {'version': 2, 'data': 'x' * (1024 * 1024)},
      ]) {
        expect(() => validateManagedInstallations(value), invalid);
      }
    },
  );

  test(
    'checkpoints can contain only full verified chunks and the final tail',
    () {
      final request = intent(size: managedStageChunkBytes + 7);
      validateManagedCheckpoint(request, 0, 0);
      validateManagedCheckpoint(request, managedStageChunkBytes, 1);
      validateManagedCheckpoint(request, request.expectedSize, 2);
      expect(() => validateManagedCheckpoint(request, 3, 1), invalid);
      expect(
        () => validateManagedCheckpoint(request, request.expectedSize, 1),
        invalid,
      );
      expect(
        () => validateManagedCheckpoint(request, managedStageChunkBytes, 2),
        invalid,
      );
    },
  );
}
