import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/features/library/data/library_repository.dart';
import 'package:ryhze/features/library/library_controller.dart';
import 'package:ryhze/features/library/library_screen.dart';
import 'package:ryhze/features/library/model/library_entry.dart';
import 'package:ryhze/features/library/widgets/library_entry_tile.dart';

class _LargeDemoRepository implements LibraryRepository {
  @override
  Future<LibraryLoadResult> load() async => LibraryLoadResult.demo(
    List.generate(2000, (index) {
      final id = index.toString().padLeft(4, '0');
      return LibraryEntry(
        identity: LibraryIdentity(providerId: 'large-test-demo', titleId: id),
        title: 'Fictional test game $id',
        ownership: const LibraryOwnership(
          state: LibraryOwnershipState.demonstration,
          reason: 'Fictional measurement fixture, never ownership evidence.',
        ),
        installation: const LibraryInstallation(
          state: LibraryInstallationState.unknown,
          reason: 'Simulated unknown installation.',
        ),
      );
    }),
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    for (final name in ['Inter', 'SpaceGrotesk']) {
      final loader = FontLoader(name)
        ..addFont(rootBundle.load('assets/fonts/$name.ttf'));
      await loader.load();
    }
  });

  for (final (size, scale) in [
    (const Size(1280, 800), 1.0),
    (const Size(390, 844), 1.6),
  ]) {
    testWidgets(
      'bounded 2000-entry demo fixture records mounting, rebuilds, scrolling and image cache ${size.width}',
      (tester) async {
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = size;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final cache = PaintingBinding.instance.imageCache;
        final oldMaximumSize = cache.maximumSize;
        final oldMaximumBytes = cache.maximumSizeBytes;
        cache.clear();
        cache.clearLiveImages();
        cache.maximumSize = 64;
        cache.maximumSizeBytes = 32 * 1024 * 1024;
        addTearDown(() {
          cache.clear();
          cache.clearLiveImages();
          cache.maximumSize = oldMaximumSize;
          cache.maximumSizeBytes = oldMaximumBytes;
        });
        final builds = <String, int>{};
        // This SDK's callback builtOnce flag is updated only when verbose
        // rebuild printing is enabled. Track weak Element identities ourselves.
        final seenBuild = Expando<bool>('Library tile build observation');
        var phase = 'mount';
        final previousBuildCallback = debugOnRebuildDirtyWidget;
        debugOnRebuildDirtyWidget = (element, builtOnce) {
          if (element.widget is LibraryEntryTile) {
            final tile = element.widget as LibraryEntryTile;
            final seen = seenBuild[element] ?? false;
            seenBuild[element] = true;
            final name =
                '$phase.${tile.card ? 'grid' : 'list'}.${seen ? 'rebuild' : 'initial'}';
            builds[name] = (builds[name] ?? 0) + 1;
          }
          previousBuildCallback?.call(element, builtOnce);
        };
        addTearDown(() => debugOnRebuildDirtyWidget = previousBuildCallback);
        final controller = LibraryController(
          repository: _LargeDemoRepository(),
        );
        addTearDown(controller.dispose);
        await controller.load();
        final mountTimer = Stopwatch()..start();
        await tester.pumpWidget(
          MaterialApp(
            theme: RyhzeTokens.theme,
            home: MediaQuery(
              data: MediaQueryData(
                size: size,
                textScaler: TextScaler.linear(scale),
                disableAnimations: true,
              ),
              child: Scaffold(body: LibraryScreen(controller: controller)),
            ),
          ),
        );
        await tester.pumpAndSettle();
        // Fake frame time alone does not complete native image decoding. Await
        // only artwork providers actually mounted in this layout before reading
        // cache sizes; compact text rows do not introduce image providers.
        final artwork = tester
            .widgetList<Image>(
              find.descendant(
                of: find.byType(LibraryScreen),
                matching: find.byType(Image),
              ),
            )
            .map((image) => image.image)
            .toSet();
        for (final provider in artwork) {
          await tester.runAsync(
            () => precacheImage(
              provider,
              tester.element(find.byType(LibraryScreen)),
            ),
          );
        }
        await tester.pumpAndSettle();
        mountTimer.stop();
        int mounted(bool card) => tester
            .widgetList<LibraryEntryTile>(find.byType(LibraryEntryTile))
            .where((tile) => tile.card == card)
            .length;
        final initialGrid = mounted(true);
        final initialList = mounted(false);
        // Structural lazy-building criterion, not a time/performance budget.
        expect(initialGrid + initialList, greaterThan(0));
        expect(initialGrid, lessThan(2000));
        expect(initialList, lessThan(2000));
        expect(find.byKey(const Key('library-demo-indicator')), findsOneWidget);
        final collection = find.byKey(
          Key(size.width > 700 ? 'library-grid' : 'library-list'),
        );
        final scrollable = find
            .descendant(of: collection, matching: find.byType(Scrollable))
            .first;
        final position = tester.state<ScrollableState>(scrollable).position;
        final scrollSamples = <Map<String, Object>>[];
        var maximumGrid = initialGrid;
        var maximumList = initialList;
        phase = 'scroll';
        for (var step = 1; step <= 8; step++) {
          final timer = Stopwatch()..start();
          position.jumpTo(position.maxScrollExtent * step / 8);
          await tester.pumpAndSettle();
          timer.stop();
          final grid = mounted(true);
          final list = mounted(false);
          maximumGrid = grid > maximumGrid ? grid : maximumGrid;
          maximumList = list > maximumList ? list : maximumList;
          scrollSamples.add({
            'step': step,
            'offset': position.pixels,
            'test_wall_us': timer.elapsedMicroseconds,
            'mounted_grid': grid,
            'mounted_list': list,
            'image_cache_entries': cache.currentSize,
            'image_cache_bytes': cache.currentSizeBytes,
            'live_image_count': cache.liveImageCount,
            'pending_image_count': cache.pendingImageCount,
          });
          expect(grid, lessThan(2000));
          expect(list, lessThan(2000));
          expect(tester.takeException(), isNull);
        }
        expect(position.pixels, greaterThan(0));
        final lastId = const LibraryIdentity(
          providerId: 'large-test-demo',
          titleId: '1999',
        ).toString();
        expect(
          find.byKey(
            Key('${size.width > 700 ? 'library-card' : 'library-row'}-$lastId'),
          ),
          findsOneWidget,
        );
        phase = 'favourite';
        controller.toggleFavourite(controller.entries.last.identity);
        await tester.pumpAndSettle();
        phase = 'query';
        controller.setQuery('1999');
        await tester.pumpAndSettle();
        expect(controller.visibleEntries, hasLength(1));
        expect(mounted(true), lessThanOrEqualTo(1));
        expect(mounted(false), lessThanOrEqualTo(1));
        expect(cache.currentSize, lessThanOrEqualTo(cache.maximumSize));
        expect(
          cache.currentSizeBytes,
          lessThanOrEqualTo(cache.maximumSizeBytes),
        );
        expect(find.byKey(const Key('library-demo-indicator')), findsOneWidget);
        // Captured by the test runner as raw evidence. Flutter test wall times are
        // synthetic host observations, not release-frame or Steam Deck results.
        debugPrint(
          'LIBRARY_FIXTURE_MEASUREMENT ${jsonEncode({'schema': 2, 'entries': 2000, 'fixture': 'fictional_demo', 'width': size.width, 'height': size.height, 'text_scale': scale, 'reduced_motion': true, 'mount_test_wall_us': mountTimer.elapsedMicroseconds, 'initial_mounted_grid': initialGrid, 'initial_mounted_list': initialList, 'maximum_mounted_grid': maximumGrid, 'maximum_mounted_list': maximumList, 'tile_build_callbacks': builds, 'build_classification': 'weak Element identity distinguishes first build from rebuild; SDK builtOnce flag ignored', 'mounted_artwork_providers': artwork.length, 'image_decode_wait': 'awaited mounted provider precache before first sample', 'scroll_samples': scrollSamples, 'final_image_cache_entries': cache.currentSize, 'final_image_cache_bytes': cache.currentSizeBytes, 'final_pending_image_count': cache.pendingImageCount, 'artwork': 'shared bundled branded placeholder; no remote game artwork', 'limitations': 'Debug widget test with fake frame clock; elapsed wall time includes test harness. No FPS, production performance or physical device claim.'})}',
        );
        expect(tester.takeException(), isNull);
      },
    );
  }
}
