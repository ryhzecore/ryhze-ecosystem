import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/domain/app_route.dart';
import 'package:ryhze/features/entertainment/entertainment_screen.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/title_detail.dart';
import 'package:ryhze/features/library/data/library_preferences.dart';
import 'package:ryhze/features/library/data/library_repository.dart';
import 'package:ryhze/features/library/library_controller.dart';
import 'package:ryhze/features/library/library_screen.dart';
import 'package:ryhze/features/library/model/library_entry.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/features/store/catalog_repository.dart';
import 'package:ryhze/features/workspace/workspace_panel.dart';

const _identity = LibraryIdentity(providerId: 'fixture', titleId: 'One');

class _DeferredCatalog extends LocalCatalogRepository {
  final requests = <Completer<EntertainmentTitle?>>[];
  final tokens = <CancellationToken>[];

  @override
  Future<EntertainmentTitle?> lookup(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  ) {
    tokens.add(cancellation);
    final request = Completer<EntertainmentTitle?>();
    requests.add(request);
    return request.future;
  }
}

class _Repository implements LibraryRepository {
  const _Repository();
  @override
  Future<LibraryLoadResult> load() async => LibraryLoadResult.verified([
    LibraryEntry(
      identity: _identity,
      title: 'Synthetic test title',
      ownership: const LibraryOwnership(
        state: LibraryOwnershipState.verifiedOwned,
        reason: 'Injected fixture; no native or provider verification.',
      ),
      installation: const LibraryInstallation(
        state: LibraryInstallationState.unknown,
        reason: 'No native installation checked.',
      ),
    ),
  ]);
}

LibraryController _controller(WidgetTester tester) => tester
    .widget<LibraryScreen>(find.byType(LibraryScreen, skipOffstage: false))
    .controller;

Future<void> _show(
  WidgetTester tester, {
  Size size = const Size(1280, 800),
  double scale = 1,
  EdgeInsets padding = EdgeInsets.zero,
  EdgeInsets insets = EdgeInsets.zero,
  AppServices? services,
}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    MediaQuery(
      data: MediaQueryData(
        size: size,
        textScaler: TextScaler.linear(scale),
        padding: padding,
        viewPadding: padding,
        viewInsets: insets,
        disableAnimations: true,
      ),
      child: RyhzeApp(
        libraryRepository: const _Repository(),
        services: services,
      ),
    ),
  );
  await tester.pumpAndSettle();
}

Future<void> _command(WidgetTester tester, LogicalKeyboardKey key) async {
  await tester.sendKeyDownEvent(LogicalKeyboardKey.metaLeft);
  await tester.sendKeyEvent(key);
  await tester.sendKeyUpEvent(LogicalKeyboardKey.metaLeft);
  await tester.pumpAndSettle();
}

/// Synthetic Flutter channel delivery, not OS protocol registration evidence.
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
    'Apple destination changes cancel delivered item routes; exact Library routes use shared identity',
    (tester) async {
      final catalog = _DeferredCatalog();
      final services = AppServices(
        storage: MemoryTransactionStore(),
        libraryRepository: const _Repository(),
        catalogRepository: catalog,
      );
      await _show(tester, services: services);
      final pending = _deliverRoute(
        tester,
        'ryhze://app/games/store?item=${Catalog.larcenous.id}',
      );
      await tester.pump();
      expect(catalog.requests, hasLength(1));
      await _command(tester, LogicalKeyboardKey.digit3);
      expect(catalog.tokens.single.isCancelled, isTrue);
      catalog.requests.single.complete(Catalog.larcenous);
      await pending;
      await tester.pumpAndSettle();
      expect(services.navigation.destination, AppDestination.filmsStore);
      expect(find.byType(TitleDetail), findsNothing);
      await _deliverRoute(
        tester,
        'ryhze://app/games/library?item=One&provider=fixture',
      );
      await tester.pumpAndSettle();
      expect(services.navigation.destination, AppDestination.gamesLibrary);
      expect(services.library.selectedId, _identity);
      expect(find.byKey(const Key('library-detail')), findsOneWidget);
      expect(services.jobs.read(services.session.scope), isEmpty);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await services.close();
    },
    variant: TargetPlatformVariant({TargetPlatform.iOS, TargetPlatform.macOS}),
  );

  testWidgets(
    'Apple Command destinations release hidden search and retain feature state',
    (tester) async {
      await _show(tester);
      await _command(tester, LogicalKeyboardKey.digit2);
      expect(find.byKey(const Key('library-screen')), findsOneWidget);
      await _command(tester, LogicalKeyboardKey.keyK);
      final field = tester.widget<TextField>(
        find.byKey(const Key('library-search')),
      );
      expect(field.focusNode!.hasFocus, isTrue);
      await tester.enterText(
        find.byKey(const Key('library-search')),
        'Synthetic',
      );
      await _command(tester, LogicalKeyboardKey.digit3);
      expect(find.byType(LibraryScreen), findsNothing);
      expect(field.focusNode!.hasFocus, isFalse);
      await _command(tester, LogicalKeyboardKey.digit1);
      expect(
        tester
            .widget<RyhzeControl>(find.byKey(const Key('destination-store')))
            .selected,
        isTrue,
      );
      await _command(tester, LogicalKeyboardKey.digit2);
      expect(_controller(tester).query, 'Synthetic');
      expect(field.focusNode!.hasFocus, isFalse);
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant({TargetPlatform.macOS, TargetPlatform.iOS}),
  );

  testWidgets(
    'Apple modal retains navigation ownership during Command shortcuts',
    (tester) async {
      await _show(tester);
      await _command(tester, LogicalKeyboardKey.keyK);
      expect(find.byKey(const Key('search-input')), findsOneWidget);
      await _command(tester, LogicalKeyboardKey.digit2);
      expect(find.byKey(const Key('search-input')), findsOneWidget);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(find.byType(LibraryScreen), findsNothing);
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'Search trigger');
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.macOS),
  );

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'iOS touch navigation respects safe area and enlarged text $scale',
      (tester) async {
        await _show(
          tester,
          size: const Size(320, 640),
          scale: scale,
          padding: const EdgeInsets.only(top: 47, bottom: 34),
        );
        final bar = find.byKey(const Key('apple-touch-navigation'));
        expect(tester.getRect(bar).bottom, lessThanOrEqualTo(606));
        expect(
          tester.getRect(find.byKey(const Key('header-brand'))).top,
          greaterThanOrEqualTo(47),
        );
        for (final name in ['store', 'library', 'films']) {
          final destination = find.byKey(Key('destination-$name'));
          expect(tester.getSize(destination).height, greaterThanOrEqualTo(48));
          expect(destination.hitTestable(), findsOneWidget);
          await tester.tap(destination);
          await tester.pumpAndSettle();
          expect(tester.widget<RyhzeControl>(destination).selected, isTrue);
          expect(tester.takeException(), isNull);
        }
        await tester.tap(find.byKey(const Key('destination-library')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('library-list')), findsOneWidget);
        expect(find.byKey(const Key('library-grid')), findsNothing);
        final scrollable = find
            .descendant(
              of: find.byKey(const Key('library-list')),
              matching: find.byType(Scrollable),
            )
            .first;
        await tester.scrollUntilVisible(
          find.text('Your Library'),
          -200,
          scrollable: scrollable,
        );
        // Switch to the real default unavailable source, then exercise content
        // below the compact viewport; an empty or Demo view is not ownership.
        await _controller(tester)
            .setRepository(const UnavailableLibraryRepository());
        await tester.pumpAndSettle();
        await tester.scrollUntilVisible(
          find.text('Library unavailable'),
          200,
          scrollable: scrollable,
        );
        await tester.pumpAndSettle();
        expect(find.text('Library unavailable').hitTestable(), findsOneWidget);
        final demo = find.byKey(const Key('library-demo-toggle'));
        await tester.ensureVisible(demo);
        await tester.pumpAndSettle();
        await tester.tap(demo);
        await tester.pumpAndSettle();
        expect(_controller(tester).isDemo, isTrue);
        expect(find.byKey(const Key('library-demo-indicator')), findsOneWidget);
        await tester.ensureVisible(demo);
        await tester.pumpAndSettle();
        expect(
          find.text('Leave Demo Library').hitTestable(),
          findsOneWidget,
          reason:
              'toggle=${tester.getRect(demo)} '
              'label=${tester.getRect(find.text('Leave Demo Library'))} '
              'viewport=${tester.getRect(find.byKey(const Key('library-list')))}',
        );
        await tester.tap(demo);
        await tester.pumpAndSettle();
        expect(_controller(tester).isDemo, isFalse);
        await tester.scrollUntilVisible(
          find.text('Library unavailable'),
          200,
          scrollable: scrollable,
        );
        await tester.pumpAndSettle();
        expect(find.text('Library unavailable').hitTestable(), findsOneWidget);
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.iOS),
    );
  }

  for (final platform in [TargetPlatform.iOS, TargetPlatform.macOS]) {
    testWidgets('Apple back restores exact Library origin on $platform', (
      tester,
    ) async {
      await _show(
        tester,
        size: platform == TargetPlatform.iOS
            ? const Size(390, 844)
            : const Size(1280, 800),
      );
      await _command(tester, LogicalKeyboardKey.digit2);
      final entry = find.byKey(
        Key(
          'library-${platform == TargetPlatform.iOS ? 'row' : 'card'}-$_identity',
        ),
      );
      await tester.ensureVisible(entry);
      await tester.tap(entry);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('library-detail')), findsOneWidget);
      if (platform == TargetPlatform.iOS) {
        await tester.binding.handlePopRoute();
        await tester.pumpAndSettle();
      } else {
        await _command(tester, LogicalKeyboardKey.bracketLeft);
      }
      expect(find.byKey(const Key('library-detail')), findsNothing);
      expect(
        FocusManager.instance.primaryFocus?.debugLabel,
        'Library $_identity',
      );
      expect(entry.hitTestable(), findsOneWidget);
      expect(tester.takeException(), isNull);
    }, variant: TargetPlatformVariant.only(platform));
  }

  testWidgets(
    'shared session replacement clears Apple views and preserves scoped intent',
    (tester) async {
      final services = AppServices.memory(
        libraryRepository: const _Repository(),
      );
      await _show(tester, services: services);
      final previous = _controller(tester);
      final previousScreen = tester.state(find.byType(EntertainmentScreen));
      await previous.toggleFavourite(_identity);
      await services.store.toggle(Catalog.larcenous.id);
      expect(previous.isFavourite(_identity), isTrue);
      await _command(tester, LogicalKeyboardKey.digit2);
      await tester.enterText(
        find.byKey(const Key('library-search')),
        'private query',
      );
      services.session.replace(
        const AccountScope.verified('fixture', 'account-b'),
      );
      await tester.pumpAndSettle();
      final current = _controller(tester);
      expect(previousScreen.mounted, isFalse);
      expect(current, same(previous));
      expect(current.query, isEmpty);
      expect(current.selectedId, isNull);
      expect(current.isFavourite(_identity), isFalse);
      expect(current.entries, isEmpty);
      expect(current.status, LibraryStatus.unavailable);
      expect(
        LibraryPreferences(services.storage, const AccountScope.local()).read(),
        {_identity},
      );
      expect(
        tester
            .widget<RyhzeControl>(find.byKey(const Key('hero-favourite')))
            .selected,
        isFalse,
      );
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.macOS),
  );

  testWidgets('macOS Command comma opens shared settings and returns focus', (
    tester,
  ) async {
    await _show(tester);
    await _command(tester, LogicalKeyboardKey.comma);
    expect(find.byType(WorkspacePanel), findsOneWidget);
    expect(find.text('Reduce motion'), findsOneWidget);
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pumpAndSettle();
    expect(find.byType(WorkspacePanel), findsNothing);
    expect(FocusManager.instance.primaryFocus?.debugLabel, 'Menu trigger');
    expect(tester.takeException(), isNull);
  }, variant: TargetPlatformVariant.only(TargetPlatform.macOS));
}
