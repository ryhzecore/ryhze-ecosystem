import 'dart:async';
import 'dart:convert';

/// These codes are deliberately closed: transport errors may contain VM URLs.
enum JourneyFailure {
  connectTimeout,
  connectFailed,
  requestTimeout,
  requestFailed,
  invalidResponse,
  frameworkTestsFailed,
  missingReport,
  unknownReport,
  ambiguousReport,
  incompleteReport,
  cleanupIncomplete,
  invalidReport,
  closeTimeout,
  closeFailed,
}

/// FlutterDriver.connect itself has no real deadline on Windows. A driver that
/// arrives after this deadline is closed without delaying the failed receipt.
Future<T> connectJourneyDriver<T>({
  required Future<T> Function() connect,
  required Future<void> Function(T) close,
  Duration timeout = const Duration(seconds: 60),
  Duration closeTimeout = const Duration(seconds: 10),
}) async {
  var expired = false;
  final pending = Future<T>.sync(connect);
  unawaited(
    pending.then<void>((driver) async {
      if (!expired) return;
      try {
        await Future<void>.sync(() => close(driver)).timeout(closeTimeout);
      } catch (_) {
        // The connection already failed its deadline. Never log transport errors.
      }
    }, onError: (Object error, StackTrace stack) {}),
  );
  return pending.timeout(
    timeout,
    onTimeout: () {
      expired = true;
      throw TimeoutException('connectTimeout');
    },
  );
}

class JourneyDriverResult {
  JourneyDriverResult({
    required this.data,
    required this.frameworkAllTestsPassed,
    required this.failure,
  });

  factory JourneyDriverResult.failed(JourneyFailure failure) =>
      JourneyDriverResult(
        data: const {},
        frameworkAllTestsPassed: null,
        failure: failure,
      );

  final Map<String, Object?> data;
  final bool? frameworkAllTestsPassed;
  final JourneyFailure? failure;
  JourneyFailure? closeFailure;
  Map<String, Object?> allocationBefore = const {'available': false};
  Map<String, Object?> allocationAfter = const {'available': false};

  bool get passed => failure == null && closeFailure == null;

  Map<String, Object?> toJson() => {
    ...data,
    'framework_all_tests_passed': frameworkAllTestsPassed,
    'all_tests_passed': passed,
    'driver_status': passed ? 'completed' : 'failed',
    'driver_failure': failure?.name,
    'driver_close_failure': closeFailure?.name,
    'allocation_before_journey': allocationBefore,
    'allocation_after_journey': allocationAfter,
  };
}

/// Reads the integration_test wire envelope without retaining exception details.
/// A parseable partial report is preserved even when its envelope cannot pass.
JourneyDriverResult parseJourneyResponse(String response) {
  Map<String, Object?> data = const {};
  bool? frameworkPassed;
  try {
    if (response.length > 4 * 1024 * 1024) {
      return JourneyDriverResult.failed(JourneyFailure.invalidResponse);
    }
    final envelope = jsonDecode(response);
    if (envelope is! Map<String, dynamic>) {
      return JourneyDriverResult.failed(JourneyFailure.invalidResponse);
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
        !failures.every((entry) => entry is String) ||
        (frameworkPassed && failures.isNotEmpty) ||
        (envelope['data'] != null &&
            envelope['data'] is! Map<String, dynamic>)) {
      return JourneyDriverResult(
        data: data,
        frameworkAllTestsPassed: frameworkPassed,
        failure: JourneyFailure.invalidResponse,
      );
    }
    return JourneyDriverResult(
      data: data,
      frameworkAllTestsPassed: frameworkPassed,
      failure: frameworkPassed
          ? validateJourneyReport(data)
          : JourneyFailure.frameworkTestsFailed,
    );
  } catch (_) {
    return JourneyDriverResult(
      data: data,
      frameworkAllTestsPassed: frameworkPassed,
      failure: JourneyFailure.invalidResponse,
    );
  }
}

/// A driver success must contain one recognized, fully settled workload.
JourneyFailure? validateJourneyReport(Map<String, Object?> data) {
  final known = const [
    'journey',
    'providers_journey',
    'artwork_journey',
  ].where(data.containsKey).toList();
  if (known.length > 1) return JourneyFailure.ambiguousReport;
  if (known.isEmpty) {
    return data.isEmpty
        ? JourneyFailure.missingReport
        : JourneyFailure.unknownReport;
  }
  final report = data[known.single];
  if (report is! Map<String, dynamic>) return JourneyFailure.invalidReport;
  if (report['status'] != 'completed') return JourneyFailure.incompleteReport;
  if (report['cleanup_completed'] != true) {
    return JourneyFailure.cleanupIncomplete;
  }
  if (!_text(report['source'], maxLength: 160)) {
    return JourneyFailure.invalidReport;
  }
  final valid = switch (known.single) {
    'journey' => _commonReport(report),
    'providers_journey' => _providerReport(report),
    'artwork_journey' => _artworkReport(report),
    _ => false,
  };
  return valid ? null : JourneyFailure.invalidReport;
}

bool _artworkReport(Map<String, dynamic> report) {
  const expectedPhases = [
    'live public catalogue reference',
    'native managed cache clear',
    'live artwork network fetch and native persistence',
    'repository restart reads native disk',
    'warm memory read',
    'Store and detail image presentation',
    'injected offline retention and visible label',
    'explicit cancellation and native drain',
    'explicit cache clear without automatic refill',
  ];
  const expectedLimits = [
    'Public artwork is presentation metadata, not gameplay or ownership evidence.',
    'Offline failure and +2h age are injected; the physical network remains unchanged.',
    'Repository/channel re-creation does not prove process or operating-system restart.',
    'Cancellation exercises a public read request; socket abort is not instrumented.',
    'No physical power, screen reader, installer or image-golden acceptance.',
  ];
  if (report['workload'] != 'windows-public-artwork-native-cache-v1' ||
      !_hex(report['source'], 40) ||
      report['native_cache_available'] != true ||
      report.containsKey('pending_operation') ||
      report.containsKey('failed_operation') ||
      !_exactList(report['completed_phases'], expectedPhases) ||
      !_exactList(report['limits'], expectedLimits) ||
      report['restart_scope'] !=
          'Repository and channel client; same Windows process.' ||
      report['performance_scope'] !=
          'Single-run operation timings; no optimization comparison.') {
    return false;
  }
  final catalogue = report['catalogue'];
  final image = report['image'];
  final timings = report['read_timings_us'];
  if (catalogue is! Map ||
      catalogue['quality'] != 'complete' ||
      !_boundedInt(catalogue['public_metadata_count'], 1, 1000) ||
      !_artworkTimestamp(catalogue['observed_at_utc']) ||
      image is! Map ||
      !_text(image['item_id'], maxLength: 128) ||
      !RegExp(r'^[a-z0-9]+(?:-[a-z0-9]+)*$')
          .hasMatch(image['item_id'] as String) ||
      !_boundedInt(image['revision'], 0, 0x7fffffff) ||
      !_hex(image['key'], 64) ||
      !_hex(image['sha256'], 64) ||
      !_text(image['path'], maxLength: 160) ||
      !RegExp(r'^/art/[a-z0-9][a-z0-9-]*\.png$')
          .hasMatch(image['path'] as String) ||
      !_text(image['note'], maxLength: 160) ||
      RegExp(r'[\x00-\x1f\x7f-\x9f]').hasMatch(image['note'] as String) ||
      !(image['note'] as String).toLowerCase().contains('not gameplay') ||
      !_boundedInt(image['encoded_bytes'], 1, 8 * 1024 * 1024) ||
      !_boundedInt(image['width'], 1, 8192) ||
      !_boundedInt(image['height'], 1, 8192) ||
      (image['width'] as int) * (image['height'] as int) > 16 * 1024 * 1024 ||
      !_artworkTimestamp(image['stored_at_utc']) ||
      timings is! Map ||
      timings.length != 4 ||
      !const [
        'network',
        'disk',
        'memory',
        'offline',
      ].every((key) => _boundedInt(timings[key], 1, 25 * 1000 * 1000))) {
    return false;
  }
  for (final key in ['network', 'disk', 'memory']) {
    final read = report['${key}_read'];
    if (read is! Map ||
        read['origin'] != key ||
        read['stale'] != false ||
        read['sha256'] != image['sha256']) {
      return false;
    }
    if (key == 'network' && read['native_persisted'] != true) return false;
  }
  final presentation = report['presentation'];
  final offline = report['offline'];
  final cancellation = report['cancellation'];
  if (presentation is! Map ||
      presentation['store_note'] != image['note'] ||
      presentation['detail_note'] != image['note'] ||
      presentation['detail_back_completed'] != true ||
      presentation['aspect_ratio'] != '16:9' ||
      offline is! Map ||
      offline['injected_failure'] != 'network' ||
      offline['injected_clock_offset_ms'] is! int ||
      offline['injected_clock_offset_ms'] != 7200000 ||
      offline['origin'] != 'disk' ||
      offline['stale'] != true ||
      offline['sha256'] != image['sha256'] ||
      offline['visible_caption'] !=
          '${image['note']} · Offline artwork may be out of date' ||
      !_boundedInt(offline['failed_transport_attempts'], 1, 32) ||
      cancellation is! Map ||
      cancellation['failure'] != 'cancelled' ||
      cancellation['repository_closed'] != true ||
      cancellation['native_drained'] != true ||
      cancellation['socket_abort'] != 'not instrumented') {
    return false;
  }
  for (final prefix in ['initial_', '']) {
    final before =
        report[prefix.isEmpty ? 'cache_before_clear' : 'initial_cache_health'];
    final after =
        report[prefix.isEmpty ? 'cache_after_clear' : 'initial_cleared_health'];
    final clear = report['${prefix}clear'];
    if (!_artworkHealth(before) ||
        !_artworkHealth(after) ||
        clear is! Map ||
        !_boundedInt(clear['removed'], 0, 64) ||
        !_boundedInt(clear['preserved'], 0, 128) ||
        clear['removed'] != (before as Map)['entries'] ||
        clear['preserved'] != before['preserved'] ||
        (after as Map)['entries'] != 0 ||
        after['bytes'] != 0 ||
        after['preserved'] != before['preserved'] ||
        (prefix.isEmpty && (before['entries'] as int) < 1)) {
      return false;
    }
  }
  final clearPresentation = report['clear_presentation'];
  return clearPresentation is Map &&
      const [
        'visible_images',
        'memory_entries',
        'memory_bytes',
        'active',
        'queued',
        'automatic_refetches',
      ].every(
        (key) => clearPresentation[key] is int && clearPresentation[key] == 0,
      );
}

bool _exactList(Object? value, List<String> expected) =>
    value is List &&
    value.length == expected.length &&
    List.generate(
      expected.length,
      (i) => i,
    ).every((i) => value[i] == expected[i]);
bool _hex(Object? value, int length) =>
    value is String &&
    value.length == length &&
    RegExp(r'^[a-f0-9]+$').hasMatch(value);
bool _boundedInt(Object? value, int minimum, int maximum) =>
    value is int && value >= minimum && value <= maximum;
bool _artworkHealth(Object? value) =>
    value is Map &&
    _boundedInt(value['entries'], 0, 64) &&
    _boundedInt(value['bytes'], 0, 64 * 1024 * 1024) &&
    _boundedInt(value['preserved'], 0, 128);
bool _artworkTimestamp(Object? value) =>
    _utcTimestamp(value) &&
    DateTime.parse(value as String).toUtc().toIso8601String() == value;

/// The request timeout is an actual Future deadline, unlike flutter_driver's
/// diagnostic timeout. Closing the transport is always attempted and bounded;
/// this does not claim that a timed-out test body or native work was cancelled.
Future<JourneyDriverResult> collectJourneyResult({
  required Future<String> Function() request,
  required Future<void> Function() close,
  Future<Map<String, Object?>> Function()? allocation,
  Duration requestTimeout = const Duration(minutes: 3),
  Duration closeTimeout = const Duration(seconds: 10),
  Duration allocationTimeout = const Duration(seconds: 5),
}) async {
  var result = JourneyDriverResult.failed(JourneyFailure.requestFailed);
  var before = <String, Object?>{'available': false};
  var after = <String, Object?>{'available': false};
  try {
    before = await _allocation(allocation, allocationTimeout);
    try {
      final response = await Future<String>.sync(request)
          .timeout(requestTimeout);
      result = parseJourneyResponse(response);
    } on TimeoutException {
      result = JourneyDriverResult.failed(JourneyFailure.requestTimeout);
    } catch (_) {
      result = JourneyDriverResult.failed(JourneyFailure.requestFailed);
    }
    after = await _allocation(allocation, allocationTimeout);
  } finally {
    try {
      await Future<void>.sync(close).timeout(closeTimeout);
    } on TimeoutException {
      result.closeFailure = JourneyFailure.closeTimeout;
    } catch (_) {
      result.closeFailure = JourneyFailure.closeFailed;
    }
  }
  result.allocationBefore = before;
  result.allocationAfter = after;
  return result;
}

Future<Map<String, Object?>> _allocation(
  Future<Map<String, Object?>> Function()? sample,
  Duration timeout,
) async {
  if (sample == null) return {'available': false, 'reason': 'unsupported'};
  try {
    final result = await Future<Map<String, Object?>>.sync(sample)
        .timeout(timeout);
    // Profiling is optional, and a malformed sample must not break the receipt.
    jsonEncode(result);
    return result;
  } on TimeoutException {
    return {'available': false, 'reason': 'timeout'};
  } catch (_) {
    return {'available': false, 'reason': 'unavailable'};
  }
}

bool _commonReport(Map<String, dynamic> report) {
  final durations = report['navigation_round_trip_us'];
  return _shellAcceptance(report) &&
      report['fixture'] ==
          'bundled-catalogue-51-synthetic-activity-isolated-journal' &&
      durations is List &&
      durations.length == 12 &&
      durations.every(_count) &&
      _count(report['bootstrap_to_settled_frame_us']) &&
      _count(report['rss_before_bytes']) &&
      _count(report['rss_after_bytes']) &&
      _count(report['max_rss_bytes']) &&
      _counters(report['process_counters_before']) &&
      _counters(report['process_counters_after']) &&
      _text(report['profile_directory'], maxLength: 32767) &&
      report['physical_power_screen_reader_installer'] ==
          'not exercised by this test';
}

bool _counters(Object? value) =>
    value is Map &&
    const [
      'cpu_kernel_100ns',
      'cpu_user_100ns',
      'read_operations',
      'write_operations',
      'read_bytes',
      'write_bytes',
      'other_operations',
      'other_bytes',
    ].every((key) => _count(value[key]));

bool _providerReport(Map<String, dynamic> report) {
  final steam = report['steam_adapter_available'];
  final activation = report['native_activation_available'];
  final phases = report['completed_phases'];
  final limits = report['limits'];
  if (!_shellAcceptance(report) ||
      !_supportArtworkClear(report['artwork_support_clear']) ||
      report['workload'] !=
          'production-anonymous-catalogue-readonly-steam-isolated-journal' ||
      steam is! bool ||
      activation is! bool ||
      phases is! List ||
      limits is! List ||
      limits.isEmpty ||
      !limits.every(_text)) {
    return false;
  }
  final expected = [
    'initial truthful catalogue quality',
    'Library and Store navigation',
    'Support artwork clear preserves catalogue and user state',
    'Settings and cache health/clear UI',
    if (steam) 'on-demand native Steam observation',
    'Workspace Escape and menu focus restoration',
    'explicit catalogue request cancellation',
    'truthful recovery and repeat refresh',
    if (activation)
      'same-executable forwarding queued behind Search then Activity drain',
  ];
  if (phases.length != expected.length ||
      !List.generate(
        expected.length,
        (i) => i,
      ).every((i) => phases[i] == expected[i])) {
    return false;
  }
  if (!const [
        'initial_catalogue',
        'recovery_catalogue',
        'repeat_catalogue',
      ].every((key) => _catalogue(report[key])) ||
      !_cacheStates.contains(report['cache_before_clear']) ||
      !_text(report['repeat_refresh_note'])) {
    return false;
  }
  final inspection = report['steam_inspection'];
  if (inspection is! Map) return false;
  if (steam) {
    final quality = inspection['quality'];
    final failure = inspection['failure'];
    final count = inspection['observation_count'];
    if (inspection['paths_titles_and_ids'] != 'not included' ||
        (quality != null && !_qualities.contains(quality)) ||
        (count != null && !_count(count)) ||
        (quality == 'complete'
            ? failure != null || !_count(count)
            : !_failures.contains(failure)) ||
        (quality == null && count != null)) {
      return false;
    }
  } else if (inspection['state'] != 'adapter unavailable; not exercised') {
    return false;
  }
  return activation
      ? report['activity_forward_exit_code'] is int &&
            report['activity_forward_exit_code'] == 0
      : report['activity_forwarding'] ==
            'native activation unavailable; not exercised';
}

// Revision 2 requires the observed route and focus after Escape. Dispatching a
// key or disposing the whole app during teardown cannot substitute for these.
bool _shellAcceptance(Map<String, dynamic> report) {
  if (report['acceptance_revision'] is! int ||
      report['acceptance_revision'] != 2 ||
      report.containsKey('pending_operation') ||
      report.containsKey('failed_operation')) {
    return false;
  }
  final dismissal = report['workspace_dismissal'];
  if (dismissal is! Map ||
      dismissal.length != 3 ||
      dismissal['escape_dispatched'] != true ||
      !_workspaceFocus(dismissal['before']) ||
      !_workspaceFocus(dismissal['after'])) {
    return false;
  }
  final before = dismissal['before'] as Map;
  final after = dismissal['after'] as Map;
  return before['workspace_present'] == true &&
      before['workspace_current'] == true &&
      before['overlay_open'] == true &&
      before['focused_route'] == 'workspace' &&
      before['menu_focus_restored'] == false &&
      after['workspace_present'] == false &&
      after['workspace_current'] == false &&
      after['overlay_open'] == false &&
      after['focused_route'] == 'other' &&
      after['menu_focus_restored'] == true;
}

// Only closed, non-identifying diagnostic values are accepted in focus records.
bool _workspaceFocus(Object? value) =>
    value is Map &&
    value.length == 6 &&
    const [
      'workspace_present',
      'workspace_current',
      'overlay_open',
      'menu_focus_restored',
    ].every((key) => value[key] is bool) &&
    const {'none', 'workspace', 'other'}.contains(value['focused_route']) &&
    const {
      'detached',
      'resumed',
      'inactive',
      'hidden',
      'paused',
      'unknown',
    }.contains(value['lifecycle']);

bool _supportArtworkClear(Object? value) {
  if (value is! Map ||
      value['native_cache_available'] != true ||
      value['ui_confirmation_matched'] != true ||
      value['isolated_user_state_unchanged'] != true ||
      value['catalogue_unchanged'] != true ||
      value['library_unchanged'] != true ||
      !_supportArtworkHealth(value['before']) ||
      !_supportArtworkHealth(value['after']) ||
      !_boundedInt(value['reported_removed'], 0, 64) ||
      !_boundedInt(value['reported_preserved'], 0, 128) ||
      !_boundedInt(value['bound_image_widgets_before'], 0, 2002) ||
      value['bound_image_widgets_after'] is! int ||
      value['bound_image_widgets_after'] != 0) {
    return false;
  }
  final before = value['before'] as Map;
  final after = value['after'] as Map;
  return value['reported_removed'] == before['entries'] &&
      value['reported_preserved'] == before['preserved'] &&
      after['entries'] == 0 &&
      after['bytes'] == 0 &&
      after['preserved'] == before['preserved'] &&
      after['memory_entries'] == 0 &&
      after['memory_bytes'] == 0;
}

bool _supportArtworkHealth(Object? value) =>
    _artworkHealth(value) &&
    value is Map &&
    _boundedInt(value['memory_entries'], 0, 8) &&
    _boundedInt(value['memory_bytes'], 0, 16 * 1024 * 1024) &&
    value['active'] is int &&
    value['active'] == 0 &&
    value['queued'] is int &&
    value['queued'] == 0;

bool _catalogue(Object? value) {
  if (value is! Map) return false;
  final quality = value['quality'];
  final failure = value['failure'];
  final count = value['public_metadata_count'];
  if (!_qualities.contains(quality) ||
      !_cacheStates.contains(value['cache_state']) ||
      !value.containsKey('failure') ||
      !value.containsKey('public_metadata_count')) {
    return false;
  }
  if (quality == 'complete' ? failure != null : !_failures.contains(failure)) {
    return false;
  }
  if (quality == 'complete' || quality == 'stale') {
    if (!_count(count) || !_utcTimestamp(value['observed_at_utc'])) {
      return false;
    }
  } else if (count != null) {
    return false;
  }
  return !value.containsKey('observed_at_utc') ||
      _utcTimestamp(value['observed_at_utc']);
}

const _qualities = {'complete', 'partial', 'stale', 'unavailable'};
const _cacheStates = {
  'empty',
  'fresh',
  'stale',
  'expired',
  'invalid',
  'unavailable',
};
const _failures = {
  'network',
  'denied',
  'unsupported',
  'authentication',
  'invalidData',
  'storage',
  'cancelled',
  'conflict',
  'recovery',
};

bool _count(Object? value) => value is int && value >= 0;
bool _text(Object? value, {int maxLength = 2048}) =>
    value is String && value.trim().isNotEmpty && value.length <= maxLength;
bool _utcTimestamp(Object? value) =>
    value is String &&
    value.length <= 64 &&
    value.endsWith('Z') &&
    DateTime.tryParse(value) != null;
