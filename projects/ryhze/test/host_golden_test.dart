import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/widgets.dart';

import 'support/golden_diagnostics.dart';

import 'support/host_goldens.dart';
import 'support/journal_fixture.dart';

class _RecordingComparator extends GoldenFileComparator {
  bool result = true;
  Object? error;
  Uri? compared;
  Uint8List? received;
  int comparisons = 0;
  int updates = 0;

  @override
  Future<bool> compare(Uint8List imageBytes, Uri golden) async {
    comparisons++;
    compared = golden;
    received = imageBytes;
    if (error case final problem?) throw problem;
    return result;
  }

  @override
  Future<void> update(Uri golden, Uint8List imageBytes) async {
    updates++;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('optional geometry diagnostics', () {
    for (final matched in [true, false]) {
      testWidgets('blocked output retains strict result $matched', (
        tester,
      ) async {
        final directory = Directory.systemTemp.createTempSync(
          'ryhze-diagnostics-',
        );
        addTearDown(() => directory.deleteSync(recursive: true));
        final blocked = File('${directory.path}/blocked');
        await tester.runAsync(() => blocked.writeAsString('preserved'));
        final previous = goldenFileComparator;
        final delegate = _RecordingComparator()..result = matched;
        goldenFileComparator = delegate;
        addTearDown(() => goldenFileComparator = previous);
        await tester.pumpWidget(
          const RepaintBoundary(
            key: Key('diagnostic-target'),
            child: SizedBox(width: 32, height: 32),
          ),
        );
        final comparison = expectGoldenWithDiagnostics(
          tester,
          find.byKey(const Key('diagnostic-target')),
          'goldens/diagnostic.png',
          environment: {
            'RYHZE_GOLDEN_DIAGNOSTICS': '1',
            'RYHZE_GOLDEN_DIAGNOSTICS_ROOT': blocked.path,
          },
        );
        if (matched) {
          await comparison;
        } else {
          await expectLater(comparison, throwsA(isA<TestFailure>()));
        }
        expect(delegate.comparisons, 1);
        expect(delegate.compared, Uri.parse('goldens/diagnostic.png'));
        expect(delegate.updates, 0);
        expect(
          tester.takeException(),
          isNull,
          reason: 'Optional diagnostic I/O must not enter binding errors.',
        );
        expect(
          await tester.runAsync(() => blocked.readAsString()),
          'preserved',
        );
      });
    }
    testWidgets('diagnostics do not consume unrelated framework errors', (
      tester,
    ) async {
      final directory = Directory.systemTemp.createTempSync(
        'ryhze-diagnostics-pending-',
      );
      addTearDown(() => directory.deleteSync(recursive: true));
      final blocked = File('${directory.path}/blocked');
      await tester.runAsync(() => blocked.writeAsString('preserved'));
      final previous = goldenFileComparator;
      final delegate = _RecordingComparator();
      goldenFileComparator = delegate;
      addTearDown(() => goldenFileComparator = previous);
      await tester.pumpWidget(
        const RepaintBoundary(
          key: Key('diagnostic-target'),
          child: SizedBox(width: 32, height: 32),
        ),
      );
      final unrelated = StateError('synthetic unrelated framework failure');
      FlutterError.reportError(FlutterErrorDetails(exception: unrelated));
      await expectGoldenWithDiagnostics(
        tester,
        find.byKey(const Key('diagnostic-target')),
        'goldens/diagnostic.png',
        environment: {
          'RYHZE_GOLDEN_DIAGNOSTICS': '1',
          'RYHZE_GOLDEN_DIAGNOSTICS_ROOT': blocked.path,
        },
      );
      expect(delegate.comparisons, 1);
      expect(tester.takeException(), same(unrelated));
      expect(tester.takeException(), isNull);
    });
  });

  group('actual rendering host policy', () {
    test('uses the OS host independently of a simulated target platform', () {
      final previous = debugDefaultTargetPlatformOverride;
      debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
      try {
        final policy = GoldenHostPolicy.fromEnvironment(environment: {});
        expect(policy.host, Platform.operatingSystem);
        expect(policy.captureCandidates, isFalse);
      } finally {
        debugDefaultTargetPlatformOverride = previous;
      }
    });

    test('candidate capture requires every explicit host CI opt-in', () {
      const approved = {
        'CI': 'true',
        'RYHZE_GOLDEN_DIAGNOSTICS': '1',
        'RYHZE_GOLDEN_CANDIDATES': '1',
      };
      for (final host in ['macos', 'windows']) {
        final policy = GoldenHostPolicy.fromEnvironment(
          host: host,
          environment: approved,
        );
        expect(policy.host, host);
        expect(policy.captureCandidates, isTrue);
        for (final entry in const {
          'CI': ['false', '1', ''],
          'RYHZE_GOLDEN_DIAGNOSTICS': ['0', ''],
        }.entries) {
          for (final value in entry.value) {
            expect(
              () => GoldenHostPolicy.fromEnvironment(
                host: host,
                environment: {...approved, entry.key: value},
              ),
              throwsA(isA<TestFailure>()),
              reason: '$host ${entry.key}=$value must not enable capture.',
            );
          }
        }
        for (final value in ['0', '', 'true']) {
          expect(
            GoldenHostPolicy.fromEnvironment(
              host: host,
              environment: {...approved, 'RYHZE_GOLDEN_CANDIDATES': value},
            ).captureCandidates,
            isFalse,
          );
        }
        for (final key in approved.keys) {
          final missing = Map<String, String>.of(approved)..remove(key);
          if (key == 'RYHZE_GOLDEN_CANDIDATES') {
            expect(
              GoldenHostPolicy.fromEnvironment(
                host: host,
                environment: missing,
              ).captureCandidates,
              isFalse,
            );
          } else {
            expect(
              () => GoldenHostPolicy.fromEnvironment(
                host: host,
                environment: missing,
              ),
              throwsA(isA<TestFailure>()),
            );
          }
        }
      }
      for (final host in ['linux', 'android']) {
        expect(
          () => GoldenHostPolicy.fromEnvironment(
            host: host,
            environment: approved,
          ),
          throwsA(isA<TestFailure>()),
        );
      }
    });
  });

  group('strict host reference routing', () {
    final pixels = Uint8List.fromList([137, 80, 78, 71, 13, 10, 26, 10]);

    test(
      'each host preserves its strict reference key and exact delegate result',
      () async {
        for (final host in ['linux', 'windows', 'macos']) {
          final delegate = _RecordingComparator()..result = false;
          final comparator = HostGoldenComparator(
            delegate: delegate,
            policy: GoldenHostPolicy(host: host, captureCandidates: false),
          );
          final key = Uri.parse('goldens/library/selection-large-text.png');
          final expected = host == 'linux'
              ? key
              : Uri.parse(
                  'goldens/hosts/$host/library/selection-large-text.png',
                );
          expect(comparator.referenceFor(key), expected);
          expect(await comparator.compare(pixels, key), isFalse);
          expect(delegate.compared, expected);
          expect(delegate.received, same(pixels));
          expect(delegate.comparisons, 1);
          expect(delegate.updates, 0);
        }
      },
    );

    for (final host in ['macos', 'windows']) {
      test('$host isolates references without falling back to Linux', () async {
        final directory = await createJournalFixture('ryhze-host-reference-');
        addTearDown(() => directory.delete(recursive: true));
        final linux = File('${directory.path}/goldens/shared.png');
        await linux.parent.create(recursive: true);
        await linux.writeAsBytes(pixels);
        final comparator = HostGoldenComparator(
          delegate: LocalFileComparator(
            directory.uri.resolve('synthetic_host_test.dart'),
          ),
          policy: GoldenHostPolicy(host: host, captureCandidates: false),
        );
        final key = Uri.parse('goldens/shared.png');
        expect(
          comparator.referenceFor(key),
          Uri.parse('goldens/hosts/$host/shared.png'),
        );
        await expectLater(
          comparator.compare(pixels, key),
          throwsA(
            isA<TestFailure>().having(
              (error) => error.message,
              'missing host reference',
              contains('goldens/hosts/$host/shared.png'),
            ),
          ),
        );
        expect(await linux.readAsBytes(), pixels);
        expect(
          await File('${directory.path}/goldens/hosts/$host/shared.png')
              .exists(),
          isFalse,
        );
      });
    }

    test('unsafe golden paths never reach the delegate', () async {
      final delegate = _RecordingComparator();
      final comparator = HostGoldenComparator(
        delegate: delegate,
        policy: GoldenHostPolicy(host: 'macos', captureCandidates: false),
      );
      for (final path in [
        '../goldens/escape.png',
        'goldens/../escape.png',
        'goldens/%2e%2e/escape.png',
        'goldens/nested%2fescape.png',
        'goldens/nested%5cescape.png',
        '/goldens/absolute.png',
        'file:///goldens/absolute.png',
        'https://example.invalid/goldens/remote.png',
        'goldens/reference.png?replace=true',
        'goldens/reference.png#fragment',
      ]) {
        await expectLater(
          () => comparator.compare(pixels, Uri.parse(path)),
          throwsA(isA<TestFailure>()),
          reason: path,
        );
      }
      expect(delegate.comparisons, 0);
      expect(delegate.updates, 0);
    });

    test(
      'updating references is prohibited for every host and capture mode',
      () async {
        for (final policy in [
          GoldenHostPolicy(host: 'linux', captureCandidates: false),
          GoldenHostPolicy(host: 'windows', captureCandidates: false),
          GoldenHostPolicy(host: 'windows', captureCandidates: true),
          GoldenHostPolicy(host: 'macos', captureCandidates: false),
          GoldenHostPolicy(host: 'macos', captureCandidates: true),
        ]) {
          final delegate = _RecordingComparator();
          final comparator = HostGoldenComparator(
            delegate: delegate,
            policy: policy,
          );
          await expectLater(
            () => comparator.update(Uri.parse('goldens/reference.png'), pixels),
            throwsA(isA<TestFailure>()),
          );
          expect(delegate.updates, 0);
          expect(delegate.comparisons, 0);
        }
      },
    );

    for (final host in ['macos', 'windows']) {
      final referencePrefix = 'goldens/hosts/$host';
      test(
        '$host only candidate mode converts expected pixel comparison errors',
        () async {
          final directory = await createJournalFixture('ryhze-host-errors-');
          addTearDown(() => directory.delete(recursive: true));
          for (final capture in [false, true]) {
            final delegate = _RecordingComparator()
              ..error = FlutterError(
                'Golden "$referencePrefix/mismatch.png": Pixel test failed, '
                '1.0% diff detected.',
              );
            final comparator = HostGoldenComparator(
              delegate: delegate,
              policy: GoldenHostPolicy(host: host, captureCandidates: capture),
              candidateRoot: directory,
            );
            await expectLater(
              comparator.compare(pixels, Uri.parse('goldens/mismatch.png')),
              throwsA(capture ? isA<TestFailure>() : isA<FlutterError>()),
            );
            final unexpected = FlutterError(
              'Synthetic unrelated framework failure',
            );
            delegate.error = unexpected;
            await expectLater(
              comparator.compare(pixels, Uri.parse('goldens/unrelated.png')),
              throwsA(
                capture
                    ? isA<TestFailure>().having(
                        (error) => error.message,
                        'unexpected framework failure',
                        'Golden diagnostic operation failed unexpectedly (FlutterError).',
                      )
                    : same(unexpected),
              ),
            );
            delegate.error = StateError(
              'Synthetic unexpected comparator failure',
            );
            await expectLater(
              comparator.compare(pixels, Uri.parse('goldens/unexpected.png')),
              throwsA(
                capture
                    ? isA<TestFailure>().having(
                        (error) => error.message,
                        'unexpected state failure',
                        'Golden diagnostic operation failed unexpectedly (StateError).',
                      )
                    : isA<StateError>(),
              ),
            );
          }
        },
      );

      test('$host candidate files preserve bytes and provenance', () async {
        final directory = await createJournalFixture('ryhze-host-candidate-');
        addTearDown(() => directory.delete(recursive: true));
        final delegate = _RecordingComparator()..result = false;
        final comparator = HostGoldenComparator(
          delegate: delegate,
          policy: GoldenHostPolicy(host: host, captureCandidates: true),
          candidateRoot: directory,
        );
        expect(
          await comparator.compare(pixels, Uri.parse('goldens/apple/view.png')),
          isFalse,
        );
        final candidate = File('${directory.path}/$host/apple/view.png');
        expect(await candidate.readAsBytes(), pixels);
        final metadata = jsonDecode(
          await File('${candidate.path}.json').readAsString(),
        ) as Map<String, dynamic>;
        expect(metadata['referenceHost'], host);
        expect(metadata['rendererHost'], host);
        expect(
          metadata['status'],
          'unreviewed candidate; not an approved reference',
        );
        expect(metadata['canonicalGoldenKey'], 'goldens/apple/view.png');
        expect(
          metadata['strictReferenceKey'],
          '$referencePrefix/apple/view.png',
        );
        expect(metadata['capturedOnHost'], Platform.operatingSystem);
        expect(delegate.compared, Uri.parse('$referencePrefix/apple/view.png'));
        expect(delegate.received, same(pixels));
        expect(delegate.updates, 0);
      });

      test('$host candidate storage rejects links', () async {
        final directory = await createJournalFixture('ryhze-host-links-');
        addTearDown(() => directory.delete(recursive: true));
        final outside = await Directory('${directory.path}/outside').create();
        final delegate = _RecordingComparator();
        final candidateRoot = await Directory('${directory.path}/candidates')
            .create();
        final hostDirectory = await Directory('${candidateRoot.path}/$host')
            .create();
        final linkedParent = Link('${hostDirectory.path}/linked');
        await linkedParent.create(outside.path);
        final comparator = HostGoldenComparator(
          delegate: delegate,
          policy: GoldenHostPolicy(host: host, captureCandidates: true),
          candidateRoot: candidateRoot,
        );
        await expectLater(
          comparator.compare(pixels, Uri.parse('goldens/linked/escape.png')),
          throwsA(isA<TestFailure>()),
        );
        final untouched = File('${outside.path}/untouched.png');
        await untouched.writeAsString('preserved');
        await Link('${hostDirectory.path}/existing.png').create(untouched.path);
        await expectLater(
          comparator.compare(pixels, Uri.parse('goldens/existing.png')),
          throwsA(isA<TestFailure>()),
        );
        final existingPixels = File('${hostDirectory.path}/metadata.png');
        await existingPixels.writeAsBytes(pixels);
        await Link('${existingPixels.path}.json').create(untouched.path);
        await expectLater(
          comparator.compare(
            Uint8List.fromList([1, 2, 3]),
            Uri.parse('goldens/metadata.png'),
          ),
          throwsA(isA<TestFailure>()),
        );
        expect(
          await existingPixels.readAsBytes(),
          pixels,
          reason: 'Reject linked metadata before overwriting candidate bytes.',
        );
        expect(await untouched.readAsString(), 'preserved');
        expect(await File('${outside.path}/escape.png').exists(), isFalse);
        expect(delegate.comparisons, 0);
        expect(delegate.updates, 0);
      });
    }
  });

  test('candidate mode completes both captures but fails the real test runner', () async {
    for (final host in ['macos', 'windows']) {
      final referencePrefix = 'goldens/hosts/$host';
      final directory = await createJournalFixture('ryhze-host-subprocess-');
      addTearDown(() => directory.delete(recursive: true));
      // Flutter's test engine does not implement Isolate.resolvePackageUri.
      // Use the same resolved package configuration as the current test run.
      final packageConfig = File('.dart_tool/package_config.json').absolute;
      final configuration = jsonDecode(
        await packageConfig.readAsString(),
      ) as Map<String, dynamic>;
      final flutterPackage = (configuration['packages'] as List<dynamic>)
          .cast<Map<String, dynamic>>()
          .singleWhere((package) => package['name'] == 'flutter');
      final flutterRoot = packageConfig.uri.resolve(
        '${(flutterPackage['rootUri'] as String).replaceFirst(RegExp(r'/$'), '')}/',
      );
      final executable = File.fromUri(
        flutterRoot.resolve(
          Platform.isWindows ? '../../bin/flutter.bat' : '../../bin/flutter',
        ),
      );
      final process = await Process.start(
        executable.path,
        [
          '--suppress-analytics',
          '--no-version-check',
          'test',
          '--no-pub',
          '--concurrency=1',
          '--reporter',
          'expanded',
          'test/support/fixtures/host_golden_failure_fixture.dart',
        ],
        workingDirectory: Directory.current.path,
        runInShell: Platform.isWindows,
        environment: {
          'RYHZE_GOLDEN_TEST_ROOT': directory.path,
          'RYHZE_GOLDEN_TEST_HOST': host,
          'RYHZE_GOLDEN_CANDIDATES': '0',
          'RYHZE_GOLDEN_DIAGNOSTICS': '0',
          'CI': 'true',
          'BOT': 'true',
          'FLUTTER_SUPPRESS_ANALYTICS': 'true',
        },
      );
      final streams = Future.wait([
        process.stdout.transform(utf8.decoder).join(),
        process.stderr.transform(utf8.decoder).join(),
      ]);
      late int exitCode;
      late List<String> captured;
      try {
        exitCode = await process.exitCode.timeout(const Duration(seconds: 60));
      } finally {
        // A timed-out future does not terminate its process. Always reap this
        // child, and drain both pipes concurrently so full stderr cannot stall it.
        process.kill(ProcessSignal.sigkill);
        await process.exitCode.timeout(const Duration(seconds: 10));
        captured = await streams.timeout(const Duration(seconds: 10));
      }
      final output = captured.join('\n');
      expect(exitCode, isNot(0), reason: output);
      final afterCaptures = File('${directory.path}/after-captures.txt');
      expect(
        await afterCaptures.exists(),
        isTrue,
        reason: '$host child did not reach both captures.\n$output',
      );
      expect(
        await afterCaptures.readAsString(),
        'Both synthetic missing-reference and pixel-mismatch comparisons completed.',
        reason: output,
      );
      for (final name in ['first', 'second']) {
        final candidate = File('${directory.path}/$host/synthetic/$name.png');
        expect(await candidate.exists(), isTrue, reason: output);
        expect(await candidate.length(), greaterThan(8));
        expect(await File('${candidate.path}.json').exists(), isTrue);
      }
      final reference = File(
        '${directory.path}/references/$referencePrefix/synthetic/second.png',
      );
      expect(
        await reference.readAsBytes(),
        await File('${directory.path}/original-reference.png').readAsBytes(),
        reason: 'Capturing a mismatch must leave even a temporary reference intact.',
      );
      expect(output, contains('non-existent file'));
      expect(
        output,
        contains(
          'Golden "$referencePrefix/synthetic/second.png": Pixel test failed,',
        ),
      );
      expect(output, contains('Strict host golden comparisons failed (2).'));
      expect(output, contains('this test remains failed.'));
      expect(
        output,
        contains('synthetic missing Finder fails before continuation'),
      );
      expect(
        await File('${directory.path}/unexpected-continued.txt').exists(),
        isFalse,
        reason: 'An unrelated Finder failure must stop its test immediately.',
      );
      expect(
        output,
        contains(
          'Golden diagnostic operation failed unexpectedly (FileSystemException).',
        ),
      );
      expect(
        await File('${directory.path}/io-continued.txt').exists(),
        isFalse,
        reason: 'An unexpected I/O failure must stop its test immediately.',
      );
      expect(
        'Strict host golden comparisons failed'.allMatches(output),
        hasLength(1),
        reason:
            'Only the two expected comparisons may produce deferred failure.',
      );
    }
  }, timeout: const Timeout(Duration(seconds: 180)));
}
