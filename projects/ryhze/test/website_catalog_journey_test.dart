import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/app/navigation_controller.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/store_sections.dart';
import 'package:ryhze/features/entertainment/widgets/title_detail.dart';
import 'package:ryhze/features/store/catalog_repository.dart';
import 'package:ryhze/providers/public_catalog_transport.dart';
import 'package:ryhze/providers/ryhze_catalog_repository.dart';

class _RetiredTransport implements PublicCatalogTransport {
  int calls = 0;
  @override
  Future<Uint8List> fetch(CancellationToken cancellation) async {
    cancellation.check();
    calls++;
    throw const AppFailure(FailureKind.unsupported);
  }
}

class _MetadataCatalog extends LocalCatalogRepository
    implements CatalogMetadataLookupRepository {
  _MetadataCatalog(this.quality, this.titles);
  final ReadQuality quality;
  final List<EntertainmentTitle> titles;
  @override
  Future<RepositoryRead<EntertainmentTitle>> lookupRead(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  ) async => RepositoryRead(
    titles,
    quality: quality,
    observedAt: DateTime.utc(2026, 10, 11),
  );
}

class _CountingBundledCatalog extends LocalCatalogRepository {
  final calls = <EntertainmentCategory, int>{};

  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) {
    calls.update(category, (value) => value + 1, ifAbsent: () => 1);
    return super.search(category, query, cancellation);
  }
}

class _FailingCatalog implements CatalogRepository {
  final calls = <EntertainmentCategory, int>{};

  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    calls.update(category, (value) => value + 1, ifAbsent: () => 1);
    throw const AppFailure(FailureKind.network);
  }
}

/// Synthetic native-channel delivery; OS protocol registration is separate.
Future<void> _deliverRoute(WidgetTester tester, String location) async {
  await tester.binding.defaultBinaryMessenger.handlePlatformMessage(
    SystemChannels.navigation.name,
    SystemChannels.navigation.codec.encodeMethodCall(
      MethodCall('pushRouteInformation', {'location': location}),
    ),
    null,
  );
}

void main() {
  testWidgets(
    'retired catalogue still supports labelled game and film preview journeys',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1280, 900);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetPhysicalSize);
      final transport = _RetiredTransport();
      final services = AppServices(
        storage: MemoryTransactionStore(),
        catalogRepository: RyhzeCatalogRepository(transport: transport),
      );
      await tester.pumpWidget(RyhzeApp(services: services));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('hero-explore')));
      await tester.pumpAndSettle();
      expect(find.byType(TitleDetail), findsOneWidget);
      expect(
        find.text(
          'Bundled catalogue information. Online information is unavailable.',
        ),
        findsOneWidget,
      );
      expect(find.text(Catalog.larcenous.title), findsWidgets);
      await tester.tap(find.byKey(const Key('detail-back')));
      await tester.pumpAndSettle();
      final gameCalls = transport.calls;
      await tester.tap(find.byKey(const Key('category-films')));
      await tester.pumpAndSettle();
      expect(services.navigation.destination, AppDestination.filmsStore);
      await tester.tap(find.byKey(const Key('hero-favourite')));
      await tester.pumpAndSettle();
      expect(services.store.favourite(Catalog.tommyRiley.id).value, isTrue);
      expect(services.store.favourite(Catalog.larcenous.id).value, isFalse);
      for (var repeat = 0; repeat < 2; repeat++) {
        await tester.tap(find.byKey(const Key('hero-explore')));
        await tester.pumpAndSettle();
        final detail = tester.widget<TitleDetail>(find.byType(TitleDetail));
        expect(detail.title.id, Catalog.tommyRiley.id);
        expect(detail.category, EntertainmentCategory.films);
        expect(find.byKey(const Key('detail-metadata-notice')), findsOneWidget);
        expect(find.text(Catalog.tommyRiley.availability), findsOneWidget);
        expect(find.text(Catalog.availability), findsNothing);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
      }
      await tester.tap(find.byKey(const Key('search-trigger')));
      await tester.pumpAndSettle();
      await tester.enterText(find.byKey(const Key('search-input')), 'Tommy');
      await tester.pumpAndSettle();
      expect(find.text('Film · In development'), findsOneWidget);
      expect(find.text('Game · In development'), findsNothing);
      await tester.tap(find.text(Catalog.tommyRiley.title).last);
      await tester.pumpAndSettle();
      expect(
        tester.widget<TitleDetail>(find.byType(TitleDetail)).title.id,
        Catalog.tommyRiley.id,
      );
      expect(services.navigation.destination, AppDestination.filmsStore);
      expect(transport.calls, gameCalls);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'Search trigger');
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await services.close();
    },
  );

  for (final entry in ['store route', 'item route', 'shortcut']) {
    testWidgets(
      'first Films entry through $entry loads the catalogue and retains it after detail Back',
      (tester) async {
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = const Size(1280, 1000);
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.view.resetPhysicalSize);
        final fallback = _CountingBundledCatalog();
        final services = AppServices(
          storage: MemoryTransactionStore(),
          catalogRepository: RyhzeCatalogRepository(
            transport: _RetiredTransport(),
            fallback: fallback,
          ),
        );
        await tester.pumpWidget(RyhzeApp(services: services));
        await tester.pumpAndSettle();
        expect(
          services.discovery.state(EntertainmentCategory.films).read,
          isNull,
        );
        expect(fallback.calls[EntertainmentCategory.films], isNull);

        if (entry == 'shortcut') {
          await tester.sendKeyDownEvent(LogicalKeyboardKey.metaLeft);
          await tester.sendKeyEvent(LogicalKeyboardKey.digit3);
          await tester.sendKeyUpEvent(LogicalKeyboardKey.metaLeft);
        } else {
          await _deliverRoute(
            tester,
            entry == 'item route'
                ? 'ryhze://app/films/store?item=${Catalog.tommyRiley.id}'
                : 'ryhze://app/films/store',
          );
        }
        await tester.pumpAndSettle();
        expect(services.navigation.destination, AppDestination.filmsStore);
        expect(services.discovery.state(EntertainmentCategory.films).titles, [
          Catalog.tommyRiley,
        ]);
        if (entry != 'item route') {
          await tester.tap(find.byKey(const Key('hero-explore')));
          await tester.pumpAndSettle();
        }
        expect(
          tester.widget<TitleDetail>(find.byType(TitleDetail)).title,
          Catalog.tommyRiley,
        );
        await tester.tap(find.byKey(const Key('detail-back')));
        await tester.pumpAndSettle();
        expect(find.byType(TitleDetail), findsNothing);
        expect(
          tester.widget<StoreHero>(find.byType(StoreHero)).title,
          Catalog.tommyRiley,
        );
        expect(find.text('Refresh to load the catalogue.'), findsNothing);
        final tileTitle = find.byKey(
          ValueKey('game-tile-title-${Catalog.tommyRiley.id}'),
        );
        expect(tileTitle, findsOneWidget);
        await tester.ensureVisible(tileTitle);
        await tester.pumpAndSettle();
        expect(tileTitle.hitTestable(), findsOneWidget);

        final filmReads = fallback.calls[EntertainmentCategory.films];
        await _deliverRoute(tester, 'ryhze://app/games/store');
        await tester.pumpAndSettle();
        await _deliverRoute(tester, 'ryhze://app/films/store');
        await tester.pumpAndSettle();
        expect(fallback.calls[EntertainmentCategory.films], filmReads);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
        await services.close();
      },
      variant: TargetPlatformVariant({
        entry == 'shortcut' ? TargetPlatform.macOS : TargetPlatform.windows,
      }),
    );
  }

  testWidgets('failed first Films load waits for an explicit retry', (
    tester,
  ) async {
    final catalog = _FailingCatalog();
    final services = AppServices(
      storage: MemoryTransactionStore(),
      catalogRepository: catalog,
    );
    await tester.pumpWidget(RyhzeApp(services: services));
    await tester.pumpAndSettle();
    await _deliverRoute(tester, 'ryhze://app/films/store');
    await tester.pumpAndSettle();
    expect(catalog.calls[EntertainmentCategory.films], 1);
    expect(
      services.discovery.state(EntertainmentCategory.films).loading,
      isFalse,
    );
    expect(
      services.discovery.state(EntertainmentCategory.films).failure,
      isNotNull,
    );
    await tester.tap(find.byKey(const Key('catalog-refresh')));
    await tester.pumpAndSettle();
    expect(catalog.calls[EntertainmentCategory.films], 2);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox());
    await services.close();
  });

  for (final quality in [ReadQuality.partial, ReadQuality.stale]) {
    test(
      'metadata route retains $quality and never establishes ownership',
      () async {
        final services = AppServices(
          storage: MemoryTransactionStore(),
          catalogRepository: _MetadataCatalog(quality, [Catalog.tommyRiley]),
        );
        await services.routes.activate(
          AppRoute(AppDestination.filmsStore, itemId: Catalog.tommyRiley.id),
        );
        expect(services.routes.value.title, Catalog.tommyRiley);
        expect(services.routes.value.quality, quality);
        expect(services.library.entries, isEmpty);
        expect(services.routes.takeTitle(), Catalog.tommyRiley);
        expect(services.routes.takeTitle(), isNull);
        await services.close();
      },
    );
  }
  for (final titles in [
    <EntertainmentTitle>[],
    [Catalog.larcenous],
    [Catalog.tommyRiley, Catalog.tommyRiley],
  ]) {
    test(
      'metadata route rejects absent, wrong or ambiguous exact identity ${titles.length}',
      () async {
        final services = AppServices(
          storage: MemoryTransactionStore(),
          catalogRepository: _MetadataCatalog(ReadQuality.partial, titles),
        );
        await services.routes.activate(
          AppRoute(AppDestination.filmsStore, itemId: Catalog.tommyRiley.id),
        );
        expect(services.routes.value.title, isNull);
        expect(services.routes.value.failure, isNotNull);
        await services.close();
      },
    );
  }
  test(
    'unavailable metadata cannot open even if provider returns an item',
    () async {
      final services = AppServices(
        storage: MemoryTransactionStore(),
        catalogRepository: _MetadataCatalog(ReadQuality.unavailable, [
          Catalog.tommyRiley,
        ]),
      );
      await services.routes.activate(
        AppRoute(AppDestination.filmsStore, itemId: Catalog.tommyRiley.id),
      );
      expect(services.routes.value.title, isNull);
      expect(services.routes.value.failure, isNotNull);
      await services.close();
    },
  );
}
