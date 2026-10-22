import 'dart:convert';

import 'journey_result.dart';

const transferJourneyPhases = [
  'exclusive root receipt and real native adapters',
  'durable preparation and actual native prefix',
  'UI cancellation and settled retained prefix',
  'current authority refusal without new HTTP or files',
  'explicit HTTP Range retry and native full hash',
  'repeated UI inspection without network replay',
  'absent staging slot and explicit native staging',
  'cancelled review and reviewed staging removal',
  'cancelled review and reviewed incoming removal',
  'owners closed and isolated journal reopened',
];
const transferJourneyLimits = [
  'Synthetic test-only authority and inert loopback bytes; no production rights or service integration.',
  'Separate native test executable injects one exclusively created pinned temporary root into both production native hosts.',
  'Cleanup removes exact reviewed owned content only; isolated journal, marker and native lock files are retained.',
  'Failure retains owned content and journal for review; no recursive or fallback deletion.',
  'Journal reopen is same-process persistence validation, not process restart or physical-power acceptance.',
  'Dedicated app-services composition excludes unrelated providers; production bootstrap and packaged execution require separate acceptance.',
  'Timing and process counters describe this workload only; no comparative startup, GPU, energy or rendering performance claim.',
  'No executable launch, installer, signing, account settings, screen-reader or production deployment acceptance.',
];

JourneyDriverResult parseTransferJourneyResponse(String response) {
  Map<String, Object?> data = const {};
  bool? frameworkPassed;
  JourneyDriverResult fail(JourneyFailure kind) => JourneyDriverResult(
    data: data,
    frameworkAllTestsPassed: frameworkPassed,
    failure: kind,
  );
  try {
    if (response.length > 1024 * 1024) {
      return fail(JourneyFailure.invalidResponse);
    }
    final envelope = jsonDecode(response);
    if (envelope is! Map<String, dynamic>) {
      return fail(JourneyFailure.invalidResponse);
    }
    if (envelope['data'] case final Map<String, dynamic> reported) {
      data = Map<String, Object?>.from(reported);
    }
    frameworkPassed = switch (envelope['result']) {
      'true' => true,
      'false' => false,
      _ => null,
    };
    final failures = envelope['failureDetails'];
    if (frameworkPassed == null ||
        failures is! List ||
        !failures.every((value) => value is String) ||
        (frameworkPassed && failures.isNotEmpty) ||
        envelope['data'] is! Map<String, dynamic>) {
      return fail(JourneyFailure.invalidResponse);
    }
    if (!frameworkPassed) return fail(JourneyFailure.frameworkTestsFailed);
    if (data.length != 1 || !data.containsKey('transfer_journey')) {
      return fail(JourneyFailure.unknownReport);
    }
    final report = data['transfer_journey'];
    if (report is! Map) return fail(JourneyFailure.invalidReport);
    if (report['status'] != 'completed') {
      return fail(JourneyFailure.incompleteReport);
    }
    if (report['cleanup_completed'] != true ||
        report['owned_files_removed'] != true ||
        report['journal_reopened'] != true) {
      return fail(JourneyFailure.cleanupIncomplete);
    }
    bool hex(Object? value, int length) =>
        value is String &&
        value.length == length &&
        RegExp(r'^[0-9a-f]+$').firstMatch(value)?.end == length;
    bool identity(Object? value) =>
        value is String &&
        value.length == 49 &&
        RegExp(r'^[0-9a-f]{16}:[0-9a-f]{32}$').hasMatch(value);
    bool exactList(Object? value, List<String> expected) =>
        value is List &&
        value.length == expected.length &&
        List.generate(
          expected.length,
          (index) => value[index] == expected[index],
        ).every((same) => same);
    final phases = report['phase_micros'];
    if (report['workload'] != 'windows-native-transfer-loopback-v2' ||
        report['acceptance_revision'] != 2 ||
        !hex(report['source'], 40) ||
        !hex(report['operation_id'], 64) ||
        !hex(report['payload_sha256'], 64) ||
        !hex(report['isolation_nonce'], 64) ||
        !identity(report['isolation_root_identity']) ||
        !identity(report['device_id']) ||
        report['isolation_verified'] != true ||
        report['slot'] != 0 ||
        report['native_transfer_available'] != true ||
        report['native_staging_available'] != true ||
        report['native_discard_available'] != true ||
        report['prefix_bytes'] != 65536 ||
        report['payload_bytes'] != 600000 ||
        report['network_requests'] != 2 ||
        report['resume_range'] != 'bytes=65536-599999' ||
        report['revalidation_refusal_verified'] != true ||
        report['incoming_absence_verified'] != true ||
        report['staging_absence_verified'] != true ||
        report['operation_timed_out'] != false ||
        report.containsKey('pending_operation') ||
        report.containsKey('failed_operation') ||
        report.containsKey('body_failure') ||
        report.containsKey('responder_failures') ||
        report.containsKey('cleanup_failures') ||
        !exactList(report['completed_phases'], transferJourneyPhases) ||
        !exactList(report['limits'], transferJourneyLimits) ||
        phases is! Map ||
        phases.length != transferJourneyPhases.length ||
        !transferJourneyPhases.every(
          (phase) => phases[phase] is int && phases[phase] > 0,
        ) ||
        report['retry_ui_pipeline_micros'] is! int ||
        report['retry_ui_pipeline_micros'] <= 0) {
      return fail(JourneyFailure.invalidReport);
    }
    return JourneyDriverResult(
      data: data,
      frameworkAllTestsPassed: true,
      failure: null,
    );
  } catch (_) {
    return fail(JourneyFailure.invalidResponse);
  }
}
