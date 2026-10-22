import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';

import '../test_driver/journey_deadline.dart';
import '../test_driver/journey_result.dart';

void main() {
  test('body deadline rejects invalid budgets and operation limits', () async {
    for (final invalid in [Duration.zero, const Duration(seconds: -1)]) {
      expect(() => JourneyBodyDeadline(budget: invalid), throwsArgumentError);
      var started = false;
      final deadline = JourneyBodyDeadline(budget: const Duration(seconds: 1));
      await expectLater(
        deadline.run(() async {
          started = true;
        }, timeout: invalid),
        throwsArgumentError,
      );
      expect(started, isFalse);
    }
  });

  test('successive operations consume one absolute body budget', () async {
    var clock = const Duration(seconds: 40);
    final deadline = JourneyBodyDeadline(
      budget: const Duration(seconds: 10),
      clock: () => clock,
    );
    expect(deadline.remaining, const Duration(seconds: 10));
    expect(
      await deadline.run(() async {
        clock += const Duration(seconds: 4);
        return 1;
      }, timeout: const Duration(seconds: 8)),
      1,
    );
    expect(deadline.remaining, const Duration(seconds: 6));
    expect(
      await deadline.run(() async {
        clock += const Duration(seconds: 5);
        return 2;
      }, timeout: const Duration(seconds: 8)),
      2,
    );
    expect(deadline.remaining, const Duration(seconds: 1));
    clock += const Duration(seconds: 1);
    var started = false;
    await expectLater(
      deadline.run(() async {
        started = true;
      }, timeout: const Duration(seconds: 8)),
      throwsA(isA<TimeoutException>()),
    );
    expect(started, isFalse);
    expect(deadline.remaining, Duration.zero);
  });

  test('body deadline rejects completion after its clock budget', () async {
    var clock = Duration.zero;
    final deadline = JourneyBodyDeadline(
      budget: const Duration(seconds: 1),
      clock: () => clock,
    );
    final pending = Completer<int>();
    final result = deadline.run(
      () => pending.future,
      timeout: const Duration(seconds: 1),
    );
    clock = const Duration(seconds: 2);
    pending.complete(7);
    await expectLater(result, throwsA(isA<TimeoutException>()));
    expect(deadline.remaining, Duration.zero);
  });

  for (final remainingBudgetWins in [false, true]) {
    test(
      'deadline bounds pending work and admits cleanup: bodyLimit=$remainingBudgetWins',
      () async {
        var clock = Duration.zero;
        final deadline = JourneyBodyDeadline(
          budget: const Duration(seconds: 10),
          clock: () => clock,
        );
        if (remainingBudgetWins) {
          clock = const Duration(milliseconds: 9995);
        }
        final pending = Completer<int>();
        var cleanupCompleted = false;
        Future<void> workload() async {
          try {
            await deadline.run(
              () => pending.future,
              timeout: remainingBudgetWins
                  ? const Duration(seconds: 10)
                  : const Duration(milliseconds: 5),
            );
          } finally {
            deadline.stop();
            // Teardown has its own bound and must not share the exhausted body.
            await Future<void>(() {
              cleanupCompleted = true;
            }).timeout(const Duration(seconds: 1));
          }
        }

        await expectLater(workload(), throwsA(isA<TimeoutException>()));
        expect(cleanupCompleted, isTrue);
        var lateActionStarted = false;
        await expectLater(
          deadline.run(() async {
            lateActionStarted = true;
          }, timeout: const Duration(seconds: 1)),
          throwsA(isA<TimeoutException>()),
        );
        expect(lateActionStarted, isFalse);
        pending.complete(1);
      },
      timeout: const Timeout(Duration(seconds: 2)),
    );
  }

  test(
    'stopping at teardown blocks late actions even with body budget left',
    () async {
      final deadline = JourneyBodyDeadline(budget: const Duration(seconds: 10));
      deadline.stop();
      var started = false;
      await expectLater(
        deadline.run(() async {
          started = true;
        }, timeout: const Duration(seconds: 1)),
        throwsA(isA<TimeoutException>()),
      );
      expect(started, isFalse);
    },
  );

  test(
    'connection deadline fails and closes a driver that arrives late',
    () async {
      final pending = Completer<int>();
      final closed = Completer<int>();
      await expectLater(
        connectJourneyDriver<int>(
          connect: () => pending.future,
          close: (driver) async {
            closed.complete(driver);
          },
          timeout: const Duration(milliseconds: 5),
        ),
        throwsA(isA<TimeoutException>()),
      );
      pending.complete(7);
      expect(await closed.future, 7);
    },
  );

  test('successful connection remains open for the request owner', () async {
    var closed = false;
    final connected = await connectJourneyDriver<int>(
      connect: () async => 7,
      close: (_) async {
        closed = true;
      },
    );
    expect(connected, 7);
    expect(closed, isFalse);
  });

  test('settled common journey preserves the existing performance report', () {
    final data = {
      'journey': commonReport(),
      'repeated_navigation': {'average_frame_build_time_millis': 3.25},
    };
    final result = parseJourneyResponse(envelope(data));
    expect(result.passed, isTrue);
    expect(result.frameworkAllTestsPassed, isTrue);
    expect(result.toJson()['all_tests_passed'], isTrue);
    expect(result.data, data);
  });

  for (final steam in [false, true]) {
    for (final activation in [false, true]) {
      test('settled provider journey steam=$steam activation=$activation', () {
        final result = parseJourneyResponse(
          envelope({
            'providers_journey': providerReport(
              steam: steam,
              activation: activation,
            ),
          }),
        );
        expect(result.passed, isTrue);
      });
    }
  }

  test(
    'truthful unavailable provider observations are valid test outcomes',
    () {
      final report = providerReport();
      for (final key in [
        'initial_catalogue',
        'recovery_catalogue',
        'repeat_catalogue',
      ]) {
        report[key] = {
          'quality': 'unavailable',
          'failure': 'network',
          'public_metadata_count': null,
          'cache_state': 'empty',
        };
      }
      report['steam_inspection'] = {
        'quality': null,
        'failure': 'unsupported',
        'observation_count': null,
        'paths_titles_and_ids': 'not included',
      };
      expect(checkProvider(report).passed, isTrue);
    },
  );

  for (final key in ['journey', 'providers_journey']) {
    test('$key requires current acceptance and no unfinished operations', () {
      for (final revision in [null, 1, 2.0, '2']) {
        final report = reportFor(key)..['acceptance_revision'] = revision;
        expect(parseJourneyResponse(envelope({key: report})).passed, isFalse);
      }
      for (final operation in ['pending_operation', 'failed_operation']) {
        for (final value in [null, 'send Workspace Escape']) {
          final report = reportFor(key)..[operation] = value;
          expect(parseJourneyResponse(envelope({key: report})).passed, isFalse);
        }
      }
    });

    test('$key requires every Workspace dismissal and focus observation', () {
      for (final missing in workspaceDismissal().keys) {
        final report = reportFor(key);
        (report['workspace_dismissal'] as Map).remove(missing);
        expect(
          parseJourneyResponse(envelope({key: report})).passed,
          isFalse,
          reason: missing,
        );
      }
      for (final side in ['before', 'after']) {
        for (final missing in (workspaceDismissal()[side] as Map).keys) {
          final report = reportFor(key);
          ((report['workspace_dismissal'] as Map)[side] as Map).remove(missing);
          expect(
            parseJourneyResponse(envelope({key: report})).passed,
            isFalse,
            reason: '$side.$missing',
          );
        }
        for (final change in <Map<String, Object?>>[
          {'workspace_present': null},
          {'workspace_current': 'true'},
          {'overlay_open': 1},
          {'menu_focus_restored': null},
          {'focused_route': 'private focus debug label'},
          {'lifecycle': 'unknown lifecycle string'},
          {'debug_label': 'unexpected diagnostic'},
        ]) {
          final report = reportFor(key);
          ((report['workspace_dismissal'] as Map)[side] as Map).addAll(change);
          expect(
            parseJourneyResponse(envelope({key: report})).passed,
            isFalse,
            reason: '$side ${change.keys}',
          );
        }
      }
    });

    test('$key cannot pass an ineffective Escape or misplaced focus', () {
      for (final dispatched in [null, false, 'true']) {
        final report = reportFor(key);
        (report['workspace_dismissal'] as Map)['escape_dispatched'] =
            dispatched;
        expect(parseJourneyResponse(envelope({key: report})).passed, isFalse);
      }
      for (final side in ['before', 'after']) {
        for (final observation in [
          'workspace_present',
          'workspace_current',
          'overlay_open',
          'menu_focus_restored',
        ]) {
          final report = reportFor(key);
          final focus = (report['workspace_dismissal'] as Map)[side] as Map;
          focus[observation] = !(focus[observation] as bool);
          expect(
            parseJourneyResponse(envelope({key: report})).passed,
            isFalse,
            reason: '$side.$observation',
          );
        }
        final report = reportFor(key);
        ((report['workspace_dismissal'] as Map)[side] as Map)['focused_route'] =
            'none';
        expect(parseJourneyResponse(envelope({key: report})).passed, isFalse);
      }
    });
  }

  test(
    'Support artwork clear requires every actual UI and native observation',
    () {
      for (final missing in supportArtworkClear().keys) {
        final report = providerReport();
        (report['artwork_support_clear'] as Map).remove(missing);
        expect(checkProvider(report).passed, isFalse, reason: missing);
      }
      for (final side in ['before', 'after']) {
        for (final missing in (supportArtworkClear()[side] as Map).keys) {
          final report = providerReport();
          ((report['artwork_support_clear'] as Map)[side] as Map).remove(
            missing,
          );
          expect(
            checkProvider(report).passed,
            isFalse,
            reason: '$side.$missing',
          );
        }
        for (final change in <Map<String, Object?>>[
          {'entries': 65},
          {'bytes': 64 * 1024 * 1024 + 1},
          {'preserved': 129},
          {'memory_entries': 9},
          {'memory_bytes': 16 * 1024 * 1024 + 1},
          {'active': 1},
          {'queued': 1},
          {'active': 0.0},
          {'queued': '0'},
        ]) {
          final report = providerReport();
          ((report['artwork_support_clear'] as Map)[side] as Map).addAll(
            change,
          );
          expect(
            checkProvider(report).passed,
            isFalse,
            reason: '$side ${change.keys}',
          );
        }
      }
    },
  );

  test('Support artwork clear cannot pass leftover data or unrelated state changes', () {
    for (final field in [
      'native_cache_available',
      'ui_confirmation_matched',
      'isolated_user_state_unchanged',
      'catalogue_unchanged',
      'library_unchanged',
    ]) {
      final report = providerReport();
      (report['artwork_support_clear'] as Map)[field] = false;
      expect(checkProvider(report).passed, isFalse, reason: field);
    }
    for (final change in <Map<String, Object?>>[
      {'reported_removed': 0},
      {'reported_preserved': 0},
      {'bound_image_widgets_before': -1},
      {'bound_image_widgets_before': 2003},
      {'bound_image_widgets_after': 1},
      {'bound_image_widgets_after': 0.0},
    ]) {
      final report = providerReport();
      (report['artwork_support_clear'] as Map).addAll(change);
      expect(checkProvider(report).passed, isFalse, reason: '${change.keys}');
    }
    for (final change in <Map<String, Object?>>[
      {'entries': 1},
      {'bytes': 1},
      {'preserved': 0},
      {'memory_entries': 1},
      {'memory_bytes': 1},
    ]) {
      final report = providerReport();
      ((report['artwork_support_clear'] as Map)['after'] as Map).addAll(change);
      expect(checkProvider(report).passed, isFalse, reason: '${change.keys}');
    }
  });

  test('Support may truthfully clear an already empty native cache', () {
    final report = providerReport();
    final clear = report['artwork_support_clear'] as Map;
    (clear['before'] as Map).addAll(<String, Object?>{
      'entries': 0,
      'bytes': 0,
      'memory_entries': 0,
      'memory_bytes': 0,
    });
    clear['reported_removed'] = 0;
    clear['bound_image_widgets_before'] = 0;
    expect(checkProvider(report).passed, isTrue);
  });

  for (final key in ['journey', 'providers_journey', 'artwork_journey']) {
    test(
      '$key rejects missing, partial, running, failed and unknown status',
      () {
        for (final status in [null, 'partial', 'running', 'failed', 'other']) {
          final report = reportFor(key);
          if (status == null) {
            report.remove('status');
          } else {
            report['status'] = status;
          }
          final result = parseJourneyResponse(envelope({key: report}));
          expect(result.failure, JourneyFailure.incompleteReport);
          expect(result.toJson()['all_tests_passed'], isFalse);
          expect(result.data[key], report);
        }
      },
    );

    test('$key requires explicit successful cleanup', () {
      for (final cleanup in [null, false, 'true']) {
        final report = reportFor(key);
        if (cleanup == null) {
          report.remove('cleanup_completed');
        } else {
          report['cleanup_completed'] = cleanup;
        }
        final result = parseJourneyResponse(envelope({key: report}));
        expect(result.failure, JourneyFailure.cleanupIncomplete);
        expect(result.passed, isFalse);
      }
    });
  }

  test('absent, unknown, ambiguous and malformed reports cannot pass', () {
    for (final data in <Map<String, Object?>>[
      {},
      {'unknown_journey': commonReport()},
      {'journey': commonReport(), 'providers_journey': providerReport()},
      {'journey': commonReport(), 'artwork_journey': artworkReport()},
      {
        'providers_journey': providerReport(),
        'artwork_journey': artworkReport(),
      },
      {'journey': null},
      {'providers_journey': []},
      {'artwork_journey': []},
    ]) {
      expect(parseJourneyResponse(envelope(data)).passed, isFalse);
    }
  });

  test(
    'complete artwork workload requires explicit native and live provenance',
    () {
      final report = artworkReport();
      final result = checkArtwork(report);
      expect(result.passed, isTrue);
      expect(result.frameworkAllTestsPassed, isTrue);
      expect(result.data['artwork_journey'], report);
      expect(result.toJson()['all_tests_passed'], isTrue);
    },
  );

  test(
    'artwork workload rejects every missing report or observation field',
    () {
      for (final key in artworkReport().keys) {
        expect(
          checkArtwork(artworkReport()..remove(key)).passed,
          isFalse,
          reason: 'Missing $key',
        );
      }
      for (final entry in artworkReport().entries) {
        if (entry.value is! Map) continue;
        for (final key in (entry.value as Map).keys) {
          final report = artworkReport();
          (report[entry.key] as Map).remove(key);
          expect(
            checkArtwork(report).passed,
            isFalse,
            reason: 'Missing ${entry.key}.$key',
          );
        }
      }
    },
  );

  test(
    'artwork workload requires all exact ordered phases and limitations',
    () {
      for (final field in ['completed_phases', 'limits']) {
        final expected = artworkReport()[field] as List;
        for (var i = 0; i < expected.length; i++) {
          final report = artworkReport();
          (report[field] as List).removeAt(i);
          expect(checkArtwork(report).failure, JourneyFailure.invalidReport);
        }
        for (final values in [
          expected.reversed.toList(),
          [...expected, expected.first],
          ['unknown', ...expected.skip(1)],
        ]) {
          expect(
            checkArtwork(artworkReport()..[field] = values).passed,
            isFalse,
          );
        }
      }
    },
  );

  test(
    'artwork rejects unavailable capabilities, pending work and broad claims',
    () {
      for (final change in <Map<String, Object?>>[
        {'source': 'unrecorded'},
        {'source': '1234567'},
        {'source': 'g' * 40},
        {'workload': 'mock-artwork'},
        {'native_cache_available': false},
        {'native_cache_available': 'true'},
        {'restart_scope': 'Operating-system restart passed.'},
        {'performance_scope': 'Faster than before.'},
        {'pending_operation': 'read'},
        {'failed_operation': 'read'},
      ]) {
        expect(
          checkArtwork(artworkReport()..addAll(change)).passed,
          isFalse,
          reason: change.toString(),
        );
      }
      final report = artworkReport();
      (report['catalogue'] as Map)['quality'] = 'stale';
      expect(checkArtwork(report).passed, isFalse);
    },
  );

  test(
    'artwork identities, timestamps, dimensions and timings stay bounded',
    () {
      for (final change in <Map<String, Object?>>[
        {'item_id': 'wrong/item'},
        {'revision': -1},
        {'key': 'a' * 63},
        {'sha256': 'z' * 64},
        {'path': '/art/../private.png'},
        {'note': ''},
        {'note': 'Gameplay proven'},
        {'width': 0},
        {'height': 8193},
        {'width': 8192, 'height': 8192},
        {'encoded_bytes': 0},
        {'encoded_bytes': 8 * 1024 * 1024 + 1},
        {'stored_at_utc': 'yesterday'},
        {'stored_at_utc': '2026-02-31T01:00:00.000Z'},
      ]) {
        final report = artworkReport();
        (report['image'] as Map).addAll(change);
        expect(checkArtwork(report).passed, isFalse, reason: change.toString());
      }
      for (final key in ['network', 'disk', 'memory', 'offline']) {
        for (final value in [null, 0, -1, 1.1, '10', 25000001]) {
          final report = artworkReport();
          (report['read_timings_us'] as Map)[key] = value;
          expect(checkArtwork(report).passed, isFalse);
        }
      }
    },
  );

  test(
    'artwork read provenance cannot contradict bytes, stale state or origin',
    () {
      for (final key in [
        'network_read',
        'disk_read',
        'memory_read',
        'offline',
      ]) {
        for (final change in <Map<String, Object?>>[
          {'sha256': 'b' * 64},
          {'origin': 'unknown'},
          {'stale': key == 'offline' ? false : true},
        ]) {
          final report = artworkReport();
          (report[key] as Map).addAll(change);
          expect(checkArtwork(report).passed, isFalse);
        }
      }
      for (final change in <Map<String, Object?>>[
        {'injected_failure': 'real network disconnected'},
        {'injected_clock_offset_ms': 0},
        {'visible_caption': 'Fresh artwork'},
        {'failed_transport_attempts': 0},
      ]) {
        final report = artworkReport();
        (report['offline'] as Map).addAll(change);
        expect(checkArtwork(report).passed, isFalse);
      }
      final report = artworkReport();
      (report['network_read'] as Map)['native_persisted'] = false;
      expect(checkArtwork(report).passed, isFalse);
    },
  );

  test(
    'artwork clear cannot pass with leftover bytes, refill or lost unknowns',
    () {
      for (final key in ['initial_cleared_health', 'cache_after_clear']) {
        for (final change in <Map<String, Object?>>[
          {'entries': 1},
          {'bytes': 1},
          {'preserved': 0},
        ]) {
          final report = artworkReport();
          (report[key] as Map).addAll(change);
          expect(checkArtwork(report).passed, isFalse);
        }
      }
      for (final key in ['initial_clear', 'clear']) {
        final report = artworkReport();
        (report[key] as Map)['removed'] = 2;
        expect(checkArtwork(report).passed, isFalse);
      }
      for (final key in (artworkReport()['clear_presentation'] as Map).keys) {
        final report = artworkReport();
        (report['clear_presentation'] as Map)[key] = 1;
        expect(checkArtwork(report).passed, isFalse);
      }
    },
  );

  test(
    'artwork presentation and cancellation require positive settled evidence',
    () {
      for (final entry in <String, Map<String, Object?>>{
        'presentation': {
          'store_note': 'wrong note',
          'detail_note': 'wrong note',
          'detail_back_completed': false,
          'aspect_ratio': '4:3',
        },
        'cancellation': {
          'failure': 'network',
          'repository_closed': false,
          'native_drained': false,
          'socket_abort': 'verified',
        },
      }.entries) {
        for (final change in entry.value.entries) {
          final report = artworkReport();
          (report[entry.key] as Map)[change.key] = change.value;
          expect(checkArtwork(report).passed, isFalse);
        }
      }
    },
  );

  test('framework failure wins over a completed report and spoofed flags', () {
    final report = commonReport();
    final result = parseJourneyResponse(
      envelope({'journey': report, 'all_tests_passed': true}, passed: false),
    );
    expect(result.failure, JourneyFailure.frameworkTestsFailed);
    expect(result.toJson()['all_tests_passed'], isFalse);
    expect(result.toJson()['framework_all_tests_passed'], isFalse);
    expect(result.data['journey'], report);
  });

  test(
    'observed outer-timeout false success retains incomplete phase evidence',
    () {
      final report = providerReport()
        ..['status'] = 'running'
        ..remove('cleanup_completed');
      (report['completed_phases']! as List).removeLast();
      final result = checkProvider(report);
      expect(result.frameworkAllTestsPassed, isTrue);
      expect(result.passed, isFalse);
      expect(result.failure, JourneyFailure.incompleteReport);
      expect(result.data['providers_journey'], report);
    },
  );

  test(
    'every claimed available provider phase must be present exactly once',
    () {
      final expected = providerReport()['completed_phases']! as List<String>;
      for (final phase in expected) {
        final report = providerReport();
        (report['completed_phases']! as List).remove(phase);
        expect(checkProvider(report).failure, JourneyFailure.invalidReport);
      }
      for (final extra in [expected.first, 'unknown phase']) {
        final report = providerReport();
        (report['completed_phases']! as List).add(extra);
        expect(checkProvider(report).passed, isFalse);
      }
      final reordered = providerReport();
      reordered['completed_phases'] = expected.reversed.toList();
      expect(checkProvider(reordered).passed, isFalse);
    },
  );

  test('conditional phases cannot contradict availability or forwarding', () {
    for (final key in [
      'steam_adapter_available',
      'native_activation_available',
    ]) {
      final report = providerReport()..[key] = false;
      expect(checkProvider(report).passed, isFalse);
      for (final value in [null, 'true', 1]) {
        final report = providerReport()..[key] = value;
        expect(checkProvider(report).passed, isFalse);
      }
      final missing = providerReport()..remove(key);
      expect(checkProvider(missing).passed, isFalse);
    }
    for (final code in [null, 1, '0', 0.0]) {
      final report = providerReport()..['activity_forward_exit_code'] = code;
      expect(checkProvider(report).passed, isFalse);
    }
  });

  test(
    'provider completion markers alone cannot substitute for observations',
    () {
      for (final key in [
        'source',
        'workload',
        'acceptance_revision',
        'completed_phases',
        'limits',
        'initial_catalogue',
        'recovery_catalogue',
        'repeat_catalogue',
        'cache_before_clear',
        'repeat_refresh_note',
        'steam_inspection',
        'workspace_dismissal',
        'artwork_support_clear',
      ]) {
        final report = providerReport()..remove(key);
        expect(checkProvider(report).passed, isFalse, reason: key);
      }
      final wrongWorkload = providerReport()..['workload'] = 'mock-provider';
      expect(checkProvider(wrongWorkload).passed, isFalse);
    },
  );

  test(
    'catalogue evidence requires truthful quality, counts and timestamps',
    () {
      for (final change in <Map<String, Object?>>[
        {'quality': 'unknown'},
        {'failure': 'network'},
        {'public_metadata_count': -1},
        {'public_metadata_count': 1.5},
        {'observed_at_utc': 'not a timestamp'},
        {'cache_state': 'unknown'},
      ]) {
        final report = providerReport();
        (report['initial_catalogue']! as Map).addAll(change);
        expect(checkProvider(report).passed, isFalse);
      }
      final missingFailure = providerReport();
      (missingFailure['initial_catalogue']! as Map).remove('failure');
      expect(checkProvider(missingFailure).passed, isFalse);
    },
  );

  test(
    'common journey requires the known fixture and all existing result fields',
    () {
      for (final key in commonReport().keys) {
        final report = commonReport()..remove(key);
        expect(
          parseJourneyResponse(envelope({'journey': report})).passed,
          isFalse,
          reason: key,
        );
      }
      final wrongFixture = commonReport()..['fixture'] = 'unknown-fixture';
      expect(
        parseJourneyResponse(envelope({'journey': wrongFixture})).passed,
        isFalse,
      );
    },
  );

  test('common journey requires exactly twelve valid navigation durations', () {
    for (final durations in <Object?>[
      null,
      List.filled(11, 1),
      List.filled(13, 1),
      List.filled(12, -1),
      List.filled(12, 1.0),
      List.filled(12, '1'),
    ]) {
      final report = commonReport()..['navigation_round_trip_us'] = durations;
      expect(
        parseJourneyResponse(envelope({'journey': report})).passed,
        isFalse,
      );
    }
  });

  test('common metrics and counters reject missing or invalid values', () {
    for (final key in [
      'bootstrap_to_settled_frame_us',
      'rss_before_bytes',
      'rss_after_bytes',
      'max_rss_bytes',
    ]) {
      for (final value in [null, -1, 1.0, '1']) {
        final report = commonReport()..[key] = value;
        expect(
          parseJourneyResponse(envelope({'journey': report})).passed,
          isFalse,
        );
      }
    }
    for (final key in ['process_counters_before', 'process_counters_after']) {
      final report = commonReport();
      (report[key]! as Map).remove('cpu_user_100ns');
      expect(
        parseJourneyResponse(envelope({'journey': report})).passed,
        isFalse,
      );
    }
  });

  test(
    'malformed and contradictory wire envelopes fail without raw errors',
    () {
      for (final raw in [
        'not JSON',
        'null',
        '[]',
        '{}',
        jsonEncode({'result': true, 'failureDetails': [], 'data': {}}),
        jsonEncode({'result': 'unknown', 'failureDetails': [], 'data': {}}),
        jsonEncode({
          'result': 'true',
          'failureDetails': ['failure'],
          'data': {},
        }),
        jsonEncode({'result': 'false', 'failureDetails': 'bad', 'data': {}}),
        jsonEncode({'result': 'true', 'failureDetails': [], 'data': []}),
      ]) {
        final result = parseJourneyResponse(raw);
        expect(result.failure, JourneyFailure.invalidResponse);
        expect(result.passed, isFalse);
      }
      final report = providerReport()..['status'] = 'running';
      final partial = parseJourneyResponse(
        jsonEncode({
          'result': 'unknown',
          'data': {'providers_journey': report},
        }),
      );
      expect(partial.data['providers_journey'], report);
      expect(partial.failure, JourneyFailure.invalidResponse);
    },
  );

  test('never-resolving request really times out, closes and emits a failure receipt', () async {
    var closed = false;
    var samples = 0;
    final result = await collectJourneyResult(
      request: () => Completer<String>().future,
      close: () async {
        closed = true;
      },
      allocation: () async => {'bytes_current': ++samples},
      requestTimeout: const Duration(milliseconds: 5),
    );
    expect(closed, isTrue);
    expect(result.failure, JourneyFailure.requestTimeout);
    expect(result.toJson()['all_tests_passed'], isFalse);
    expect(result.allocationBefore['bytes_current'], 1);
    expect(result.allocationAfter['bytes_current'], 2);
    expect(jsonEncode(result.toJson()), contains('requestTimeout'));
  });

  test('request errors and malformed responses always close without diagnostics leak', () async {
    const sensitive = 'ws://127.0.0.1:1234/private-token=/ws';
    for (final malformed in [false, true]) {
      var closed = false;
      final result = await collectJourneyResult(
        request: () async {
          if (malformed) return sensitive;
          throw StateError(sensitive);
        },
        close: () async {
          closed = true;
        },
      );
      expect(closed, isTrue);
      expect(result.passed, isFalse);
      expect(jsonEncode(result.toJson()), isNot(contains(sensitive)));
    }
  });

  test(
    'never-resolving close fails a completed journey within its own deadline',
    () async {
      final result = await collectJourneyResult(
        request: () async => envelope({'journey': commonReport()}),
        close: () => Completer<void>().future,
        closeTimeout: const Duration(milliseconds: 5),
      );
      expect(result.failure, isNull);
      expect(result.closeFailure, JourneyFailure.closeTimeout);
      expect(result.passed, isFalse);
      expect(result.data['journey'], commonReport());
    },
  );

  test('close failure retains the original request failure', () async {
    final result = await collectJourneyResult(
      request: () async => 'not JSON',
      close: () async {
        throw StateError('private transport details');
      },
    );
    expect(result.failure, JourneyFailure.invalidResponse);
    expect(result.closeFailure, JourneyFailure.closeFailed);
    expect(jsonEncode(result.toJson()), isNot(contains('private transport')));
  });

  test(
    'optional allocation failures and deadlines cannot fail a settled journey',
    () async {
      for (final neverCompletes in [false, true]) {
        final result = await collectJourneyResult(
          request: () async => envelope({'journey': commonReport()}),
          close: () async {},
          allocation: () {
            if (neverCompletes) return Completer<Map<String, Object?>>().future;
            throw StateError('private profiling details');
          },
          allocationTimeout: const Duration(milliseconds: 5),
        );
        expect(result.passed, isTrue);
        expect(result.allocationBefore['available'], isFalse);
        expect(result.allocationAfter['available'], isFalse);
        expect(
          jsonEncode(result.toJson()),
          isNot(contains('private profiling')),
        );
      }
    },
  );
}

String envelope(Map<String, Object?> data, {bool passed = true}) => jsonEncode({
  'result': passed.toString(),
  'failureDetails': <String>[],
  'data': data,
});

JourneyDriverResult checkProvider(Map<String, Object?> report) =>
    parseJourneyResponse(envelope({'providers_journey': report}));

JourneyDriverResult checkArtwork(Map<String, Object?> report) =>
    parseJourneyResponse(envelope({'artwork_journey': report}));

Map<String, Object?> reportFor(String key) => switch (key) {
  'journey' => commonReport(),
  'providers_journey' => providerReport(),
  'artwork_journey' => artworkReport(),
  _ => throw ArgumentError.value(key),
};

// Synthetic driver-validation fixture, not a live artwork acceptance receipt.
// Nested maps allow malformed nullable values so negative cases reach the
// evidence validator instead of failing at Dart's map mutation boundary.
Map<String, Object?> artworkReport() {
  const note = 'World concept artwork · Not gameplay';
  final digest = 'a' * 64;
  return {
    'source': '3e1c2b8888c78085453cf268cc3491aa69da34c6',
    'workload': 'windows-public-artwork-native-cache-v1',
    'status': 'completed',
    'cleanup_completed': true,
    'native_cache_available': true,
    'completed_phases': <String>[
      'live public catalogue reference',
      'native managed cache clear',
      'live artwork network fetch and native persistence',
      'repository restart reads native disk',
      'warm memory read',
      'Store and detail image presentation',
      'injected offline retention and visible label',
      'explicit cancellation and native drain',
      'explicit cache clear without automatic refill',
    ],
    'limits': <String>[
      'Public artwork is presentation metadata, not gameplay or ownership evidence.',
      'Offline failure and +2h age are injected; the physical network remains unchanged.',
      'Repository/channel re-creation does not prove process or operating-system restart.',
      'Cancellation exercises a public read request; socket abort is not instrumented.',
      'No physical power, screen reader, installer or image-golden acceptance.',
    ],
    'restart_scope': 'Repository and channel client; same Windows process.',
    'performance_scope':
        'Single-run operation timings; no optimization comparison.',
    'catalogue': <String, Object?>{
      'quality': 'complete',
      'public_metadata_count': 1,
      'observed_at_utc': '2026-10-11T01:00:00.000Z',
    },
    'image': <String, Object?>{
      'item_id': 'catalogue-title',
      'revision': 3,
      'key': digest,
      'path': '/art/test-concept.png',
      'note': note,
      'sha256': digest,
      'encoded_bytes': 4096,
      'width': 160,
      'height': 90,
      'stored_at_utc': '2026-10-11T01:00:00.000Z',
    },
    'read_timings_us': <String, Object?>{
      'network': 1000000,
      'disk': 20000,
      'memory': 1000,
      'offline': 22000,
    },
    'network_read': <String, Object?>{
      'origin': 'network',
      'sha256': digest,
      'stale': false,
      'native_persisted': true,
    },
    'disk_read': <String, Object?>{
      'origin': 'disk',
      'sha256': digest,
      'stale': false,
    },
    'memory_read': <String, Object?>{
      'origin': 'memory',
      'sha256': digest,
      'stale': false,
    },
    'presentation': <String, Object?>{
      'store_note': note,
      'detail_note': note,
      'detail_back_completed': true,
      'aspect_ratio': '16:9',
    },
    'offline': <String, Object?>{
      'injected_failure': 'network',
      'injected_clock_offset_ms': 7200000,
      'origin': 'disk',
      'stale': true,
      'sha256': digest,
      'visible_caption': '$note · Offline artwork may be out of date',
      'failed_transport_attempts': 2,
    },
    'cancellation': <String, Object?>{
      'failure': 'cancelled',
      'repository_closed': true,
      'native_drained': true,
      'socket_abort': 'not instrumented',
    },
    'initial_cache_health': <String, Object?>{
      'entries': 0,
      'bytes': 0,
      'preserved': 2,
    },
    'initial_clear': <String, Object?>{'removed': 0, 'preserved': 2},
    'initial_cleared_health': <String, Object?>{
      'entries': 0,
      'bytes': 0,
      'preserved': 2,
    },
    'cache_before_clear': <String, Object?>{
      'entries': 1,
      'bytes': 4216,
      'preserved': 2,
    },
    'clear': <String, Object?>{'removed': 1, 'preserved': 2},
    'cache_after_clear': <String, Object?>{
      'entries': 0,
      'bytes': 0,
      'preserved': 2,
    },
    'clear_presentation': <String, Object?>{
      'visible_images': 0,
      'memory_entries': 0,
      'memory_bytes': 0,
      'active': 0,
      'queued': 0,
      'automatic_refetches': 0,
    },
  };
}

Map<String, Object?> commonReport() => {
  'source': '3e1c2b8888c78085453cf268cc3491aa69da34c6',
  'fixture': 'bundled-catalogue-51-synthetic-activity-isolated-journal',
  'acceptance_revision': 2,
  'status': 'completed',
  'cleanup_completed': true,
  'workspace_dismissal': workspaceDismissal(),
  'bootstrap_to_settled_frame_us': 100000,
  'navigation_round_trip_us': List.generate(12, (i) => 200000 + i),
  'rss_before_bytes': 2000000,
  'rss_after_bytes': 3000000,
  'max_rss_bytes': 4000000,
  'process_counters_before': counters(),
  'process_counters_after': counters(),
  'profile_directory': r'C:\synthetic-journey-fixture',
  'physical_power_screen_reader_installer': 'not exercised by this test',
};

Map<String, int> counters() => {
  'cpu_kernel_100ns': 1,
  'cpu_user_100ns': 2,
  'read_operations': 3,
  'write_operations': 4,
  'read_bytes': 5,
  'write_bytes': 6,
  'other_operations': 7,
  'other_bytes': 8,
};

Map<String, Object?> providerReport({
  bool steam = true,
  bool activation = true,
}) => {
  'source': '3e1c2b8888c78085453cf268cc3491aa69da34c6',
  'workload': 'production-anonymous-catalogue-readonly-steam-isolated-journal',
  'acceptance_revision': 2,
  'status': 'completed',
  'cleanup_completed': true,
  'workspace_dismissal': workspaceDismissal(),
  'artwork_support_clear': supportArtworkClear(),
  'completed_phases': <String>[
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
  ],
  'steam_adapter_available': steam,
  'native_activation_available': activation,
  'initial_catalogue': catalogue(),
  'recovery_catalogue': catalogue(),
  'repeat_catalogue': catalogue(),
  'cache_before_clear': 'fresh',
  'repeat_refresh_note':
      'Fresh TTL may satisfy repeat refresh from validated cache.',
  'steam_inspection': steam
      ? {
          'quality': 'complete',
          'failure': null,
          'observation_count': 2,
          'paths_titles_and_ids': 'not included',
        }
      : {'state': 'adapter unavailable; not exercised'},
  if (activation) 'activity_forward_exit_code': 0,
  if (!activation)
    'activity_forwarding': 'native activation unavailable; not exercised',
  'limits': ['No installation or launch; synthetic regression fixture.'],
};

Map<String, Object?> workspaceDismissal() => {
  'escape_dispatched': true,
  'before': <String, Object?>{
    'workspace_present': true,
    'workspace_current': true,
    'overlay_open': true,
    'focused_route': 'workspace',
    'menu_focus_restored': false,
    'lifecycle': 'resumed',
  },
  'after': <String, Object?>{
    'workspace_present': false,
    'workspace_current': false,
    'overlay_open': false,
    'focused_route': 'other',
    'menu_focus_restored': true,
    'lifecycle': 'resumed',
  },
};

Map<String, Object?> supportArtworkClear() => {
  'native_cache_available': true,
  'before': <String, Object?>{
    'memory_entries': 1,
    'memory_bytes': 4096,
    'active': 0,
    'queued': 0,
    'entries': 1,
    'bytes': 4216,
    'preserved': 2,
  },
  'after': <String, Object?>{
    'memory_entries': 0,
    'memory_bytes': 0,
    'active': 0,
    'queued': 0,
    'entries': 0,
    'bytes': 0,
    'preserved': 2,
  },
  'ui_confirmation_matched': true,
  'reported_removed': 1,
  'reported_preserved': 2,
  'isolated_user_state_unchanged': true,
  'catalogue_unchanged': true,
  'library_unchanged': true,
  'bound_image_widgets_before': 1,
  'bound_image_widgets_after': 0,
};

Map<String, Object?> catalogue() => {
  'quality': 'complete',
  'failure': null,
  'public_metadata_count': 1,
  'observed_at_utc': '2026-10-10T15:51:00.000Z',
  'cache_state': 'fresh',
};
