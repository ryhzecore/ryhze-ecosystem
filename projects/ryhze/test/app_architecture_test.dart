import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app/navigation_controller.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/title_detail.dart';
import 'package:ryhze/features/store/store_controller.dart';

void main() {
  test('navigation restores category queries and scroll independently', () {
    final nav = NavigationController();
    nav.setQuery('game query');
    nav.rememberScroll(120);
    nav.selectLibrary(true);
    nav.selectCategory(EntertainmentCategory.films);
    nav.setQuery('film query');
    nav.selectCategory(EntertainmentCategory.games);
    expect(nav.destination, AppDestination.gamesLibrary);
    expect(nav.query, 'game query');
    nav.selectLibrary(false);
    expect(nav.scrollOffset, 120);
    expect(nav.back(), isTrue);
    nav.resetScope();
    expect(nav.query, isEmpty);
    expect(nav.back(), isFalse);
    nav.dispose();
  });
  test('deep links accept only bounded navigational routes', () {
    expect(
      AppRoute.parse(Uri.parse('ryhze://app/games/store?item=abc'))?.itemId,
      'abc',
    );
    for (final uri in [
      'https://app/games/store',
      'ryhze://evil/games/store',
      'ryhze://app/games/store?command=install',
      'ryhze://app/games/store?item=a&item=b',
      'ryhze://app/games/store?item=..%2Fsecret',
      'ryhze://app:80/games/store',
    ]) {
      expect(AppRoute.parse(Uri.parse(uri)), isNull, reason: uri);
    }
  });
  test('overlay releases failures and blocks repeated activation', () async {
    final overlays = OverlayCoordinator();
    await expectLater(
      overlays.run(() async => throw StateError('route')),
      throwsStateError,
    );
    expect(await overlays.run(() async => 1), 1);
    overlays.dispose();
    expect(await overlays.run(() async => 2), isNull);
  });
  testWidgets('detail and favourites follow exact selected identity', (
    tester,
  ) async {
    final store = StoreController();
    const selected = EntertainmentTitle(
      id: 'Other',
      title: 'Other title',
      description: 'Other description',
      tags: ['Puzzle'],
    );
    store.toggle('Other');
    expect(store.favourite('other').value, isFalse);
    expect(store.favourite(Catalog.larcenous.id).value, isFalse);
    await tester.pumpWidget(
      MaterialApp(
        home: TitleDetail(
          title: selected,
          favourite: store.favourite(selected.id),
          onFavourite: () => store.toggle(selected.id),
        ),
      ),
    );
    expect(find.text('Other title'), findsOneWidget);
    expect(find.text('Other description'), findsOneWidget);
    expect(find.text(Catalog.larcenous.title), findsNothing);
    await tester.pumpWidget(const SizedBox());
    store.dispose();
  });
  test('Films search cannot display Games results', () {
    expect(Catalog.search('', category: EntertainmentCategory.films), [
      Catalog.tommyRiley,
    ]);
    expect(
      Catalog.search('Larcenous', category: EntertainmentCategory.films),
      isEmpty,
    );
  });
}
