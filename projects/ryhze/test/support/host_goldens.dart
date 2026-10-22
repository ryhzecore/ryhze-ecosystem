import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';

/// The renderer host determines references; TargetPlatformVariant does not.
/// Constructor injection is for explicit synthetic policy/regression fixtures.
class GoldenHostPolicy {
  const GoldenHostPolicy({required this.host, required this.captureCandidates});

  factory GoldenHostPolicy.fromEnvironment({
    String? host,
    Map<String, String>? environment,
  }) {
    final actualHost = host ?? Platform.operatingSystem;
    final env = environment ?? Platform.environment;
    final capture = env['RYHZE_GOLDEN_CANDIDATES'] == '1';
    if (capture &&
        (!{'macos', 'windows'}.contains(actualHost) ||
            env['CI'] != 'true' ||
            env['RYHZE_GOLDEN_DIAGNOSTICS'] != '1')) {
      throw TestFailure(
        'Golden candidate capture requires macOS or Windows, CI=true, '
        'RYHZE_GOLDEN_DIAGNOSTICS=1 and RYHZE_GOLDEN_CANDIDATES=1.',
      );
    }
    if (!{'linux', 'windows', 'macos'}.contains(actualHost)) {
      throw TestFailure('No golden reference policy for host: $actualHost');
    }
    return GoldenHostPolicy(host: actualHost, captureCandidates: capture);
  }

  final String host;
  final bool captureCandidates;
}

/// Strict references are never written. Captures are quarantined observations,
/// not approval or replacement reference material.
class HostGoldenComparator extends GoldenFileComparator {
  HostGoldenComparator({
    required this.delegate,
    required this.policy,
    Directory? candidateRoot,
  }) : candidateRoot =
           candidateRoot ??
           Directory(
             Platform.environment['RYHZE_GOLDEN_CANDIDATE_ROOT'] ??
                 (policy.host == 'windows'
                     ? 'build/windows-visual-validation/golden-candidates'
                     : 'build/apple-unsigned-validation/golden-candidates'),
           );

  final GoldenFileComparator delegate;
  final GoldenHostPolicy policy;
  final Directory candidateRoot;
  int _expectedFailures = 0;

  Uri referenceFor(Uri canonical) {
    final suffix = _suffix(canonical);
    return switch (policy.host) {
      'linux' => canonical,
      'macos' || 'windows' => Uri(path: 'goldens/hosts/${policy.host}/$suffix'),
      _ => throw TestFailure(
        'No golden reference policy for host: ${policy.host}',
      ),
    };
  }

  @override
  Uri getTestUri(Uri key, int? version) => delegate.getTestUri(key, version);

  @override
  Future<bool> compare(Uint8List imageBytes, Uri golden) async {
    try {
      return await _compare(imageBytes, golden);
    } on TestFailure {
      rethrow;
    } catch (error, stack) {
      if (!policy.captureCandidates) rethrow;
      // Flutter's matcher sends other errors to the binding's pending-error
      // channel. An unmarked TestFailure makes this await reject immediately;
      // it cannot enter the deferred expected-comparison failure ledger.
      Error.throwWithStackTrace(
        TestFailure(
          'Golden diagnostic operation failed unexpectedly (${error.runtimeType}).',
        ),
        stack,
      );
    }
  }

  Future<bool> _compare(Uint8List imageBytes, Uri golden) async {
    final reference = referenceFor(golden);
    if (policy.captureCandidates) {
      if (!{'macos', 'windows'}.contains(policy.host)) {
        throw TestFailure(
          'Only macOS or Windows candidates can be quarantined.',
        );
      }
      await _capture(imageBytes, golden, reference);
    }
    try {
      // The delegate retains its exact pixel comparison and missing-file rules.
      final matched = await delegate.compare(imageBytes, reference);
      if (!matched) _expectedFailures++;
      return matched;
    } on TestFailure catch (failure) {
      if (failure.message ==
          'Could not be compared against non-existent file: "$reference"') {
        _expectedFailures++;
      }
      rethrow;
    } on FlutterError catch (error) {
      final message = error.toString();
      if (!message.startsWith('Golden "$reference": Pixel test failed,')) {
        rethrow;
      }
      _expectedFailures++;
      if (!policy.captureCandidates) rethrow;
      // LocalFileComparator emits FlutterError for pixel differences. Convert
      // that specific comparison failure into the matcher's TestFailure path,
      // avoiding runAsync's pending-error channel so a later capture can run.
      throw TestFailure(message);
    }
  }

  @override
  Future<void> update(Uri golden, Uint8List imageBytes) async {
    throw TestFailure(
      'Golden master updates are disabled. Candidates require the explicit '
      'strict CI diagnostic mode; they are never promoted automatically.',
    );
  }

  Future<void> _capture(Uint8List bytes, Uri canonical, Uri reference) async {
    final suffix = _suffix(canonical);
    final file = File('${candidateRoot.path}/${policy.host}/$suffix');
    final metadata = File('${file.path}.json');
    await _prepareOutput(file);
    await _prepareOutput(metadata);
    await file.writeAsBytes(bytes, flush: true);
    final provenance = {
      'schema': 1,
      'status': 'unreviewed candidate; not an approved reference',
      'canonicalGoldenKey': canonical.path,
      'strictReferenceKey': reference.path,
      'referenceHost': policy.host,
      'rendererHost': policy.host,
      'capturedOnHost': Platform.operatingSystem,
      'dartVersion': Platform.version,
      'testDescription': _activeFailures?.description,
      'pngBytes': bytes.length,
      'sourceEvidence':
          'See the verification lane source-commit.txt, '
          'source-status.txt and archive hash manifest.',
    };
    await metadata.writeAsString(
      '${const JsonEncoder.withIndent('  ').convert(provenance)}\n',
      flush: true,
    );
  }

  Future<void> _prepareOutput(File file) async {
    var parent = file.absolute.parent;
    while (true) {
      final type = await FileSystemEntity.type(parent.path, followLinks: false);
      if (type != FileSystemEntityType.directory &&
          type != FileSystemEntityType.notFound) {
        throw TestFailure(
          'Candidate output must not follow a linked directory.',
        );
      }
      if (parent.parent.path == parent.path) break;
      parent = parent.parent;
    }
    final type = await FileSystemEntity.type(file.path, followLinks: false);
    if (type != FileSystemEntityType.file &&
        type != FileSystemEntityType.notFound) {
      throw TestFailure('Candidate output must be a regular file.');
    }
    await file.parent.create(recursive: true);
  }
}

String _suffix(Uri key) {
  final parts = key.path.split('/');
  if (key.hasScheme ||
      key.hasAuthority ||
      key.hasQuery ||
      key.hasFragment ||
      parts.length < 2 ||
      parts.first != 'goldens' ||
      parts[1] == 'hosts' ||
      !parts.last.endsWith('.png') ||
      parts.any(
        (part) =>
            part == '.' ||
            part == '..' ||
            !RegExp(r'^[A-Za-z0-9._-]+$').hasMatch(part),
      )) {
    throw TestFailure('Unsafe or noncanonical golden reference key.');
  }
  return parts.skip(1).join('/');
}

_GoldenFailures? _activeFailures;

/// Wrap existing comparators after their setup so their Linux comparisons remain
/// authoritative. A mandatory per-test teardown retains every deferred failure.
void installHostGoldens({GoldenHostPolicy? policy, Directory? candidateRoot}) {
  GoldenFileComparator? previous;
  late GoldenHostPolicy selected;
  setUpAll(() {
    _rejectUpdates();
    previous = goldenFileComparator;
    selected = policy ?? GoldenHostPolicy.fromEnvironment();
    goldenFileComparator = HostGoldenComparator(
      delegate: previous!,
      policy: selected,
      candidateRoot: candidateRoot,
    );
  });
  setUp(() {
    _rejectUpdates();
    final failures = selected.captureCandidates ? _GoldenFailures() : null;
    _activeFailures = failures;
    if (failures != null) {
      addTearDown(() {
        _activeFailures = null;
        failures.verify();
      });
    }
  });
  tearDownAll(() {
    if (previous != null) goldenFileComparator = previous!;
    _activeFailures = null;
  });
}

/// Only expected golden assertion failures may be deferred in explicit capture
/// mode. Finder/layout/assertion errors elsewhere retain their original behavior.
Future<void> compareHostGolden(
  WidgetTester tester,
  Finder target,
  String goldenPath,
) async {
  _rejectUpdates();
  final failures = _activeFailures;
  final comparator = goldenFileComparator;
  final previousFailures = comparator is HostGoldenComparator
      ? comparator._expectedFailures
      : null;
  if (failures != null) failures.description = tester.testDescription;
  try {
    await expectLater(target, matchesGoldenFile(goldenPath));
  } on TestFailure catch (failure) {
    if (failures == null ||
        comparator is! HostGoldenComparator ||
        !identical(comparator, goldenFileComparator) ||
        comparator._expectedFailures == previousFailures) {
      rethrow;
    }
    failures.messages.add('$goldenPath\n${failure.message}');
  }
}

void _rejectUpdates() {
  if (autoUpdateGoldenFiles) {
    throw TestFailure(
      '--update-goldens is prohibited for approved references.',
    );
  }
}

class _GoldenFailures {
  String? description;
  final messages = <String>[];
  void verify() {
    if (messages.isNotEmpty) {
      throw TestFailure(
        'Strict host golden comparisons failed (${messages.length}). '
        'Quarantined candidates are unapproved; this test remains failed.\n'
        '${messages.join('\n\n')}',
      );
    }
  }
}
