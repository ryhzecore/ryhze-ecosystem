import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/store/catalog_repository.dart';

class _Catalogue implements CatalogRepository, CatalogLookupRepository {
  static const title = EntertainmentTitle(
    id: 'test-title',
    title: 'A catalogue title',
    description: 'Synthetic metadata for the browsing regression.',
    tags: ['Adventure'],
  );
  List<EntertainmentTitle> items = [title];
  bool offline = false;
  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) async => RepositoryRead(
    offline ? [] : items,
    quality: offline ? ReadQuality.unavailable : ReadQuality.complete,
    observedAt: DateTime.utc(2026, 10, 10),
    failure: offline ? const AppFailure(FailureKind.network) : null,
  );
  @override
  Future<EntertainmentTitle?> lookup(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  ) async => items.where((t) => t.id == id).firstOrNull;
}

void main() {
  testWidgets(
    'Store opens exact operational metadata and saves only its identity',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 1000);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final source = _Catalogue();
      final services = AppServices(
        storage: MemoryTransactionStore(),
        catalogRepository: source,
      );
      await tester.pumpWidget(RyhzeApp(services: services));
      await tester.pumpAndSettle();
      expect(find.text('A catalogue title'), findsWidgets);
      expect(find.textContaining('Larcenous'), findsNothing);
      await tester.tap(find.text('Favourites').first);
      await tester.pumpAndSettle();
      expect(
        services.collections.read(services.session.scope).single.id,
        'test-title',
      );
      expect(services.library.entries, isEmpty);
      await tester.tap(find.byKey(const Key('hero-explore')));
      await tester.pumpAndSettle();
      expect(find.text(_Catalogue.title.description), findsWidgets);
      expect(find.text('Play'), findsNothing);
      await tester.tap(find.byKey(const Key('detail-back')));
      await tester.pumpAndSettle();
      source.offline = true;
      await services.discovery.load(EntertainmentCategory.games);
      await tester.pumpAndSettle();
      expect(find.textContaining('Saved catalogue'), findsOneWidget);
      expect(find.text('A catalogue title'), findsWidgets);
      source.offline = false;
      source.items = [];
      await services.discovery.load(EntertainmentCategory.games);
      await tester.pumpAndSettle();
      expect(find.text('No games available.'), findsOneWidget);
      expect(find.byKey(const Key('hero-explore')), findsNothing);
      expect(
        services.collections.read(services.session.scope).single.id,
        'test-title',
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await services.close();
    },
  );
}
