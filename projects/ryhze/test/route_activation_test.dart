import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/app/navigation_controller.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/title_detail.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/features/store/catalog_repository.dart';

class DeferredCatalog extends LocalCatalogRepository {
  final requests = <Completer<EntertainmentTitle?>>[];
  final tokens = <CancellationToken>[];
  @override
  Future<EntertainmentTitle?> lookup(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  ) {
    tokens.add(cancellation);
    final result = Completer<EntertainmentTitle?>();
    requests.add(result);
    return result.future;
  }
}

void main() {
  test(
    'Activity is a navigation-only workspace surface with no payload',
    () async {
      const route = AppRoute(AppDestination.activity);
      expect(route.toUri().toString(), 'ryhze://app/activity');
      expect(
        AppRoute.parse(route.toUri())?.destination,
        AppDestination.activity,
      );
      for (final value in [
        'ryhze://app/activity?',
        'ryhze://app/activity#',
        'ryhze://app/activity?item=x',
        'ryhze://app/activity?provider=x',
        'ryhze://app/activity?execute=1',
        'ryhze://app/activity/other',
      ]) {
        expect(AppRoute.parse(Uri.parse(value)), isNull);
      }
      final services = AppServices.memory();
      services.navigation.go(const AppRoute(AppDestination.filmsStore));
      services.navigation.go(route);
      expect(services.navigation.destination, AppDestination.filmsStore);
      await services.routes.activate(route);
      expect(services.routes.value.hasPendingPresentation, isTrue);
      expect(services.routes.value.activity, isTrue);
      expect(services.routes.takeActivity(), isTrue);
      expect(services.routes.takeActivity(), isFalse);
      expect(services.navigation.destination, AppDestination.filmsStore);
      expect(services.jobs.read(services.session.scope), isEmpty);
      await services.close();
    },
  );
  test('malformed and oversized URI data fails closed without exceptions', () {
    for (final uri in [
      Uri.parse('ryhze://app/games/store?item=%FF'),
      Uri(
        scheme: 'ryhze',
        host: 'app',
        path: '/games/store',
        queryParameters: {'item': 'x' * 5000},
      ),
    ]) {
      expect(AppRoute.parse(uri), isNull);
    }
  });
  test(
    'routes reject ambiguous Library identity and command-bearing destinations',
    () {
      expect(
        AppRoute.parse(Uri.parse('ryhze://app/games/library?item=x')),
        isNull,
      );
      expect(
        AppRoute.parse(Uri.parse('ryhze://app/games/store?item=x&provider=P')),
        isNull,
      );
      final route = AppRoute.parse(
        Uri.parse('ryhze://app/games/library?item=X&provider=P'),
      )!;
      expect(route.itemId, 'X');
      expect(route.providerId, 'P');
    },
  );
  test(
    'scope and navigation changes cancel pending exact item resolution',
    () async {
      final catalog = DeferredCatalog();
      final services = AppServices(
        storage: MemoryTransactionStore(),
        catalogRepository: catalog,
      );
      final pending = services.routes.activate(
        AppRoute(AppDestination.gamesStore, itemId: Catalog.larcenous.id),
      );
      services.navigation.selectCategory(EntertainmentCategory.films);
      expect(catalog.tokens.last.isCancelled, isTrue);
      catalog.requests.last.complete(Catalog.larcenous);
      await pending;
      expect(services.routes.value.title, isNull);
      final second = services.routes.activate(
        AppRoute(AppDestination.gamesStore, itemId: Catalog.larcenous.id),
      );
      services.session.replace(const AccountScope.verified('test', 'other'));
      catalog.requests.last.complete(Catalog.larcenous);
      await second;
      expect(services.routes.value.title, isNull);
      await services.close();
    },
  );
  testWidgets(
    'validated activation opens exact Store detail once; unknown item fails safely',
    (tester) async {
      final services = AppServices.memory();
      await tester.pumpWidget(RyhzeApp(services: services));
      await tester.pumpAndSettle();
      await services.routes.activateUri(
        Uri.parse('ryhze://app/games/store?item=${Catalog.larcenous.id}'),
      );
      await tester.pumpAndSettle();
      expect(find.byType(TitleDetail), findsOneWidget);
      await services.routes.activateUri(
        Uri.parse('ryhze://app/games/store?item=${Catalog.larcenous.id}'),
      );
      await tester.pumpAndSettle();
      expect(find.byType(TitleDetail), findsOneWidget);
      Navigator.of(tester.element(find.byType(TitleDetail))).pop();
      await tester.pumpAndSettle();
      await services.routes.activateUri(
        Uri.parse('ryhze://app/games/store?item=missing'),
      );
      await tester.pumpAndSettle();
      expect(find.byType(TitleDetail), findsNothing);
      expect(
        find.text(const AppFailure(FailureKind.unsupported).message),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await services.close();
    },
  );
}
