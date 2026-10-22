import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';

import '../test_driver/journey_result.dart';
import '../test_driver/transfer_journey_result.dart';

Map<String, Object?> report() => {
  'source': 'a' * 40,
  'workload': 'windows-native-transfer-loopback-v2',
  'acceptance_revision': 2,
  'status': 'completed',
  'cleanup_completed': true,
  'operation_timed_out': false,
  'completed_phases': transferJourneyPhases,
  'limits': transferJourneyLimits,
  'isolation_verified': true,
  'isolation_nonce': 'b' * 64,
  'isolation_root_identity': '${'1' * 16}:${'2' * 32}',
  'device_id': '${'1' * 16}:${'3' * 32}',
  'operation_id': 'c' * 64,
  'payload_sha256': 'd' * 64,
  'slot': 0,
  'native_transfer_available': true,
  'native_staging_available': true,
  'native_discard_available': true,
  'prefix_bytes': 65536,
  'payload_bytes': 600000,
  'network_requests': 2,
  'resume_range': 'bytes=65536-599999',
  'revalidation_refusal_verified': true,
  'incoming_absence_verified': true,
  'staging_absence_verified': true,
  'owned_files_removed': true,
  'journal_reopened': true,
  'retry_ui_pipeline_micros': 100,
  'phase_micros': {for (final phase in transferJourneyPhases) phase: 100},
};

String envelope(Map<String, Object?> value, {bool passed = true}) =>
    jsonEncode({
      'result': passed ? 'true' : 'false',
      'failureDetails': passed ? [] : ['failed'],
      'data': {'transfer_journey': value},
    });

void main() {
  test('accepts only complete isolated native transfer evidence', () {
    expect(parseTransferJourneyResponse(envelope(report())).passed, isTrue);
  });
  test('framework failure preserves partial evidence without passing', () {
    final value = report()
      ..['status'] = 'failed'
      ..['body_failure'] = {'error': 'prefix failure', 'stack': 'body stack'}
      ..['responder_failures'] = [
        {'operation': 'loopback response 1', 'error': 'fixture failure'},
      ]
      ..['cleanup_failures'] = [
        {'operation': 'settle response assertions', 'error': 'fixture failure'},
      ];
    final result = parseTransferJourneyResponse(envelope(value, passed: false));
    expect(result.passed, isFalse);
    expect(result.failure, JourneyFailure.frameworkTestsFailed);
    expect(result.data['transfer_journey'], value);
  });
  for (final field in [
    'body_failure',
    'responder_failures',
    'cleanup_failures',
  ]) {
    test('$field prevents claimed-complete acceptance', () {
      for (final diagnostic in <Object?>[
        {'error': 'fixture failure'},
        null,
        <Object?>[],
      ]) {
        final value = report()..[field] = diagnostic;
        final result = parseTransferJourneyResponse(envelope(value));
        expect(result.failure, JourneyFailure.invalidReport);
        expect(result.data['transfer_journey'], value);
      }
    });
  }
  for (final field in [
    'isolation_verified',
    'isolation_nonce',
    'isolation_root_identity',
    'device_id',
    'native_transfer_available',
    'native_staging_available',
    'native_discard_available',
    'revalidation_refusal_verified',
    'incoming_absence_verified',
    'staging_absence_verified',
    'resume_range',
    'payload_sha256',
    'source',
    'phase_micros',
    'retry_ui_pipeline_micros',
  ]) {
    test('missing $field cannot pass', () {
      final value = report()..remove(field);
      expect(parseTransferJourneyResponse(envelope(value)).passed, isFalse);
    });
  }
  for (final field in [
    'cleanup_completed',
    'owned_files_removed',
    'journal_reopened',
  ]) {
    test('incomplete $field cannot pass', () {
      final value = report()..[field] = false;
      expect(
        parseTransferJourneyResponse(envelope(value)).failure,
        JourneyFailure.cleanupIncomplete,
      );
    });
  }
  test('pending work and extra HTTP requests cannot pass', () {
    for (final entry in [
      const MapEntry('pending_operation', 'drain'),
      const MapEntry('failed_operation', 'response'),
      const MapEntry('operation_timed_out', true),
      const MapEntry('network_requests', 3),
      const MapEntry('completed_phases', <String>[]),
    ]) {
      final value = report()..[entry.key] = entry.value;
      expect(parseTransferJourneyResponse(envelope(value)).passed, isFalse);
    }
  });
  test('malformed envelopes cannot pass', () {
    for (final value in ['not json', '[]', '{}', '{"result":"true"}']) {
      expect(parseTransferJourneyResponse(value).passed, isFalse);
    }
  });
}
