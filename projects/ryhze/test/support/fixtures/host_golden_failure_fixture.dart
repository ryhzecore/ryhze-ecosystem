import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

import '../host_goldens.dart';

class _UnexpectedIoComparator extends GoldenFileComparator {
  @override
  Future<bool> compare(Uint8List imageBytes, Uri golden) async {
    throw const FileSystemException(
      'Synthetic unexpected comparison I/O failure',
    );
  }

  @override
  Future<void> update(Uri golden, Uint8List imageBytes) async {
    throw TestFailure(
      'This synthetic comparator must never update references.',
    );
  }
}

/// An intentionally failing subprocess fixture, never native platform validation.
/// It simulates host routing to prove candidate capture cannot turn failure green.
void main() {
  final root = Platform.environment['RYHZE_GOLDEN_TEST_ROOT'];
  if (root == null) {
    throw StateError(
      'The synthetic golden fixture requires its temporary root.',
    );
  }
  final host = Platform.environment['RYHZE_GOLDEN_TEST_HOST'];
  if (host == null || (host != 'macos' && host != 'windows')) {
    throw StateError('The synthetic golden fixture requires a supported host.');
  }
  final referencePrefix = 'goldens/hosts/$host';
  final originalComparator = goldenFileComparator;
  setUpAll(() async {
    // The only seeded reference is disposable test data outside the repository.
    // Its dimensions intentionally disagree with the second rendered widget.
    final recorder = ui.PictureRecorder();
    ui.Canvas(recorder).drawColor(const Color(0xff000000), ui.BlendMode.src);
    final picture = recorder.endRecording();
    final image = await picture.toImage(1, 1);
    final encoded = await image.toByteData(format: ui.ImageByteFormat.png);
    final bytes = encoded!.buffer.asUint8List();
    image.dispose();
    picture.dispose();
    final reference = File(
      '$root/references/$referencePrefix/synthetic/second.png',
    );
    await reference.parent.create(recursive: true);
    await reference.writeAsBytes(bytes, flush: true);
    await File('$root/original-reference.png').writeAsBytes(bytes, flush: true);
    goldenFileComparator = LocalFileComparator(
      File('$root/references/synthetic_host_test.dart').uri,
    );
  });
  tearDownAll(() {
    goldenFileComparator = originalComparator;
  });
  installHostGoldens(
    policy: GoldenHostPolicy(host: host, captureCandidates: true),
    candidateRoot: Directory(root),
  );
  testWidgets(
    'synthetic missing and mismatched $host candidates remain failures',
    (tester) async {
      for (final entry in const {
        'first': Color(0xff1741c7),
        'second': Color(0xff317956),
      }.entries) {
        await tester.pumpWidget(
          Center(
            child: RepaintBoundary(
              key: const Key('synthetic-candidate'),
              child: SizedBox(
                width: 32,
                height: 32,
                child: ColoredBox(color: entry.value),
              ),
            ),
          ),
        );
        await compareHostGolden(
          tester,
          find.byKey(const Key('synthetic-candidate')),
          'goldens/synthetic/${entry.key}.png',
        );
      }
      await tester.runAsync(() async {
        await File('$root/after-captures.txt').writeAsString(
          'Both synthetic missing-reference and pixel-mismatch comparisons completed.',
          flush: true,
        );
      });
    },
  );
  testWidgets('synthetic missing Finder fails before continuation', (
    tester,
  ) async {
    await tester.pumpWidget(const SizedBox());
    await compareHostGolden(
      tester,
      find.byKey(const Key('deliberately-absent')),
      'goldens/synthetic/unexpected.png',
    );
    await tester.runAsync(() async {
      await File('$root/unexpected-continued.txt').writeAsString(
        'An unexpected Finder failure was incorrectly deferred.',
        flush: true,
      );
    });
  });
  testWidgets('synthetic I/O failure stops before continuation', (
    tester,
  ) async {
    final previous = goldenFileComparator;
    goldenFileComparator = HostGoldenComparator(
      delegate: _UnexpectedIoComparator(),
      policy: GoldenHostPolicy(host: host, captureCandidates: true),
      candidateRoot: Directory(root),
    );
    addTearDown(() {
      goldenFileComparator = previous;
    });
    await tester.pumpWidget(
      const Center(
        child: RepaintBoundary(
          key: Key('synthetic-io'),
          child: SizedBox(width: 32, height: 32),
        ),
      ),
    );
    await compareHostGolden(
      tester,
      find.byKey(const Key('synthetic-io')),
      'goldens/synthetic/io.png',
    );
    await tester.runAsync(() async {
      await File('$root/io-continued.txt').writeAsString(
        'An unexpected I/O failure was incorrectly deferred.',
        flush: true,
      );
    });
  });
}
