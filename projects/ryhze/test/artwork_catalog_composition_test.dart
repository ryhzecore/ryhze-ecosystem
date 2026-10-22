import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/store/artwork_cache_panel.dart';
import 'package:ryhze/features/store/artwork_maintenance.dart';
import 'package:ryhze/providers/public_artwork_repository.dart';
import 'package:ryhze/providers/public_catalog_cache.dart';
import 'package:ryhze/providers/public_catalog_codec.dart';
import 'package:ryhze/providers/ryhze_catalog_repository.dart';

import 'ryhze_catalog_repository_test.dart' as fixtures;
import 'public_artwork_repository_test.dart' as artwork;

class Maintenance implements ArtworkMaintenance {
  int clears = 0;
  @override
  Future<PublicArtworkHealth> health() async => const PublicArtworkHealth(
    memoryEntries: 1,
    memoryBytes: 20,
    active: 0,
    queued: 0,
    disk: PublicArtworkCacheHealth(entries: 1, bytes: 140, preserved: 2),
  );
  @override
  Future<PublicArtworkClearResult> clear() async {
    clears++;
    return const PublicArtworkClearResult(removed: 1, preserved: 2);
  }
}

void main() {
  test(
    'catalogue restart retains exact public image identity and label',
    () async {
      final storage = MemoryTransactionStore();
      final entry = fixtures.entry('one')
        ..addAll({
          'image': '/art/one.png',
          'imageNote': 'Concept art · Not gameplay',
          'unknown': 'must not persist',
        });
      final cache = PublicCatalogCache(storage);
      final now = DateTime.utc(2026, 10, 11);
      final remote = RyhzeCatalogRepository(
        cache: cache,
        clock: () => now,
        transport: fixtures.Transport([
          (_) => fixtures.encode([entry]),
        ]),
      );
      final first = await remote.search(
        EntertainmentCategory.games,
        '',
        CancellationToken(),
      );
      expect(first.quality, ReadQuality.complete);
      final image = first.values.single.artwork!;
      expect(image.itemId, 'one');
      expect(image.revision, 3);
      expect(image.note, 'Concept art · Not gameplay');
      expect(storage.read().toString(), isNot(contains('must not persist')));
      final restarted = RyhzeCatalogRepository(
        cache: cache,
        clock: () => now,
        transport: fixtures.Transport([]),
      );
      final result = await restarted.search(
        EntertainmentCategory.games,
        '',
        CancellationToken(),
      );
      expect(result.values.single.artwork, image);
      expect(
        result.values.single.availability,
        PublicCatalogCodec.metadataAvailability,
      );
      await storage.close();
    },
  );
  test('missing artwork retains fallback while incomplete/foreign reference fails validation', () {
    expect(
      PublicCatalogCodec.parse([fixtures.entry('one')]).single.artwork,
      isNull,
    );
    for (final fields in [
      {'image': '/art/one.png'},
      {'imageNote': 'Concept'},
      {'image': 'https://unapproved.invalid/a.png', 'imageNote': 'Concept'},
    ]) {
      expect(
        () => PublicCatalogCodec.parse([fixtures.entry('one')..addAll(fields)]),
        throwsA(isA<AppFailure>()),
      );
    }
  });
  test('composition owns lifecycle invalidation and drain without user-data effects', () async {
    final cache = artwork.Cache();
    final images = PublicArtworkRepository(
      transport: artwork.Transport(),
      cache: cache,
      validateDecoded: (_) async {},
    );
    final storage = MemoryTransactionStore();
    final services = AppServices(storage: storage, artwork: images);
    expect(services.workspace.artwork, same(images));
    final before = storage.read().toString();
    services.interruptLifecycle();
    expect(images.changes.value, 1);
    await images.clear();
    expect(storage.read().toString(), before);
    await services.close();
    expect(cache.drains, greaterThanOrEqualTo(2));
  });
  testWidgets(
    'separate artwork support cleanup remains usable at true 2x narrow width',
    (tester) async {
      final maintenance = Maintenance();
      await tester.binding.setSurfaceSize(const Size(360, 740));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      await tester.pumpWidget(
        MaterialApp(
          home: MediaQuery(
            data: const MediaQueryData(textScaler: TextScaler.linear(2)),
            child: Scaffold(
              body: SingleChildScrollView(
                child: ArtworkCachePanel(maintenance: maintenance),
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(
        find.textContaining('2 unrecognized files preserved'),
        findsOneWidget,
      );
      await tester.ensureVisible(find.text('Clear artwork cache'));
      await tester.tap(find.text('Clear artwork cache'));
      await tester.pumpAndSettle();
      expect(maintenance.clears, 1);
      expect(
        find.textContaining('1 saved images removed; 2 unrecognized'),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
    },
  );
}
