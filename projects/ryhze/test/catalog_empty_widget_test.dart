import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/providers/public_catalog_cache.dart';
import 'package:ryhze/providers/ryhze_catalog_repository.dart';

import 'ryhze_catalog_repository_test.dart' show Transport, entry, encode;

void main() {
  testWidgets(
    'loading, cleared and complete-empty remote Store invent no hero',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 1000);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final response = Completer<Uint8List>();
      final store = MemoryTransactionStore();
      final repository = RyhzeCatalogRepository(
        cache: PublicCatalogCache(store),
        transport: Transport([(_) => response.future, (_) => encode([])]),
      );
      final services = AppServices(
        storage: store,
        catalogRepository: repository,
      );
      try {
        await tester.pumpWidget(RyhzeApp(services: services));
        await tester.pump();
        expect(find.textContaining('Larcenous'), findsNothing);
        expect(find.byKey(const Key('hero-explore')), findsNothing);
        expect(find.byKey(const Key('hero-favourite')), findsNothing);
        response.complete(encode([entry('live-fixture')]));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('hero-explore')), findsOneWidget);
        await services.clearPublicCatalog();
        await tester.pumpAndSettle();
        expect(find.textContaining('Synthetic live-fixture'), findsNothing);
        expect(find.textContaining('Larcenous'), findsNothing);
        expect(find.byKey(const Key('hero-explore')), findsNothing);
        expect(find.byKey(const Key('hero-favourite')), findsNothing);
        expect(
          find.text('No public catalogue metadata loaded.'),
          findsOneWidget,
        );
        expect(find.text('Refresh to load the catalogue.'), findsOneWidget);
        await services.discovery.load(EntertainmentCategory.games);
        await tester.pumpAndSettle();
        expect(find.text('No games available.'), findsOneWidget);
        expect(find.byKey(const Key('hero-explore')), findsNothing);
        expect(find.byKey(const Key('hero-favourite')), findsNothing);
        expect(find.textContaining('Larcenous'), findsNothing);
        expect(tester.takeException(), isNull);
      } finally {
        if (!response.isCompleted) response.complete(encode([]));
        await tester.pumpWidget(const SizedBox());
        await services.close();
      }
    },
  );
}
