import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/features/library/data/library_repository.dart';
import 'package:ryhze/features/library/library_controller.dart';
import 'package:ryhze/features/library/library_screen.dart';
import 'package:ryhze/features/library/model/library_entry.dart';

class _Repository implements LibraryRepository {
  _Repository(this.result);
  final LibraryLoadResult result;
  @override
  Future<LibraryLoadResult> load() async => result;
}

class _DeferredRepository implements LibraryRepository {
  final requests = <Completer<LibraryLoadResult>>[];
  @override
  Future<LibraryLoadResult> load() {
    final request = Completer<LibraryLoadResult>();
    requests.add(request);
    return request.future;
  }
}

LibraryEntry _entry(
  String id,
  String title, {
  LibraryInstallationState installation = LibraryInstallationState.unknown,
  LibraryOwnershipState ownership = LibraryOwnershipState.verifiedOwned,
}) => LibraryEntry(
  identity: LibraryIdentity(providerId: 'fixture', titleId: id),
  title: title,
  description: 'Fictional test record for $title.',
  ownership: LibraryOwnership(
    state: ownership,
    reason: 'Test ownership evidence.',
  ),
  installation: LibraryInstallation(
    state: installation,
    reason: 'Test installation evidence for $title.',
  ),
);

Future<void> _show(
  WidgetTester tester,
  LibraryController controller, {
  Size size = const Size(1280, 800),
  double scale = 1,
  bool reduced = false,
}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    MaterialApp(
      theme: RyhzeTokens.theme,
      shortcuts: <ShortcutActivator, Intent>{
        ...WidgetsApp.defaultShortcuts,
        const SingleActivator(LogicalKeyboardKey.gameButtonA):
            const ActivateIntent(),
        const SingleActivator(LogicalKeyboardKey.gameButtonB):
            const DismissIntent(),
      },
      home: MediaQuery(
        data: MediaQueryData(
          size: size,
          textScaler: TextScaler.linear(scale),
          disableAnimations: reduced,
        ),
        child: Scaffold(body: LibraryScreen(controller: controller)),
      ),
    ),
  );
  await tester.pump();
}

Future<LibraryController> _loaded(
  List<LibraryEntry> entries, {
  bool demo = false,
}) async {
  final controller = LibraryController(
    repository: _Repository(
      demo
          ? LibraryLoadResult.demo(entries)
          : LibraryLoadResult.verified(entries),
    ),
  );
  await controller.load();
  addTearDown(controller.dispose);
  return controller;
}

String _identity(String id) =>
    LibraryIdentity(providerId: 'fixture', titleId: id).toString();

Finder _entryFinder(String id) =>
    find.byKey(Key('library-card-${_identity(id)}'));

Future<void> _detailVisible(WidgetTester tester, Finder target) async {
  final scrollable = find.descendant(
    of: find.byKey(const Key('library-detail-scroll')),
    matching: find.byType(Scrollable),
  );
  await tester.scrollUntilVisible(
    target,
    300,
    scrollable: scrollable,
    maxScrolls: 20,
  );
  await tester.pumpAndSettle();
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

  testWidgets(
    'unavailable, loading, failure and verified empty remain distinct',
    (tester) async {
      final repository = _DeferredRepository();
      final controller = LibraryController(repository: repository);
      addTearDown(controller.dispose);
      await _show(tester, controller);
      final loading = controller.load();
      await tester.pump();
      expect(controller.status, LibraryStatus.loading);
      expect(find.text('Loading Library'), findsOneWidget);
      expect(find.textContaining('No owned'), findsNothing);
      repository.requests.single.complete(
        LibraryLoadResult.unavailable('Ownership source is not connected.'),
      );
      await loading;
      await tester.pumpAndSettle();
      expect(find.text('This capability is not available.'), findsOneWidget);
      expect(find.textContaining('No owned'), findsNothing);
      final failure = controller.load();
      repository.requests.last.complete(
        LibraryLoadResult.failure(
          'Test source failed without an ownership answer.',
        ),
      );
      await failure;
      await tester.pumpAndSettle();
      expect(
        find.text('The service is unavailable. Try again later.'),
        findsOneWidget,
      );
      expect(controller.status, LibraryStatus.error);
      final empty = controller.load();
      repository.requests.last.complete(LibraryLoadResult.verified(const []));
      await empty;
      await tester.pumpAndSettle();
      expect(controller.status, LibraryStatus.verifiedEmpty);
      expect(find.text('No owned games'), findsOneWidget);
      expect(
        find.text(
          'This source has successfully verified an empty owned Library.',
        ),
        findsOneWidget,
      );
      expect(find.text('This capability is not available.'), findsNothing);
      expect(
        find.text('The service is unavailable. Try again later.'),
        findsNothing,
      );
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'installation does not create ownership and unknown installation stays visible',
    (tester) async {
      final controller = await _loaded([
        _entry(
          'installed-owned',
          'Amber',
          installation: LibraryInstallationState.installed,
        ),
        _entry('unknown-owned', 'Birch'),
        _entry(
          'unsupported-owned',
          'Yarrow',
          installation: LibraryInstallationState.unsupported,
        ),
        _entry(
          'failed-owned',
          'Zinnia',
          installation: LibraryInstallationState.failed,
        ),
        _entry(
          'uninstalled-owned',
          'Cedar',
          installation: LibraryInstallationState.notInstalled,
        ),
        _entry(
          'installed-unowned',
          'Drift',
          installation: LibraryInstallationState.installed,
          ownership: LibraryOwnershipState.notOwned,
        ),
        _entry(
          'installed-unknown',
          'Elm',
          installation: LibraryInstallationState.installed,
          ownership: LibraryOwnershipState.unknown,
        ),
      ]);
      await _show(tester, controller);
      await tester.pumpAndSettle();
      expect(_entryFinder('installed-owned'), findsOneWidget);
      expect(_entryFinder('unknown-owned'), findsOneWidget);
      expect(_entryFinder('uninstalled-owned'), findsOneWidget);
      expect(find.text('Installation unsupported'), findsWidgets);
      expect(find.text('Installation check failed'), findsWidgets);
      expect(_entryFinder('installed-unowned'), findsNothing);
      expect(_entryFinder('installed-unknown'), findsNothing);
      await tester.tap(find.byKey(const Key('library-filter-installed')));
      await tester.pumpAndSettle();
      expect(_entryFinder('installed-owned'), findsOneWidget);
      expect(_entryFinder('uninstalled-owned'), findsNothing);
      expect(_entryFinder('unknown-owned'), findsNothing);
      expect(_entryFinder('unsupported-owned'), findsNothing);
      expect(_entryFinder('failed-owned'), findsNothing);
      await tester.tap(find.byKey(const Key('library-filter-allOwned')));
      await tester.pumpAndSettle();
      expect(_entryFinder('unknown-owned'), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'alphabetical entries, query and Library favourites filter owned records',
    (tester) async {
      final controller = await _loaded([
        _entry('z', 'Zinnia'),
        _entry('a', 'Amber'),
        _entry('b', 'Birch'),
      ]);
      await _show(tester, controller);
      await tester.pumpAndSettle();
      expect(find.textContaining('Title A–Z'), findsOneWidget);
      expect(
        tester.getTopLeft(_entryFinder('a')).dx,
        lessThan(tester.getTopLeft(_entryFinder('b')).dx),
      );
      controller.toggleFavourite(
        const LibraryIdentity(providerId: 'fixture', titleId: 'b'),
      );
      await tester.tap(find.byKey(const Key('library-filter-favourites')));
      await tester.pumpAndSettle();
      expect(_entryFinder('b'), findsOneWidget);
      expect(_entryFinder('a'), findsNothing);
      await tester.enterText(
        find.byKey(const Key('library-search')),
        'missing',
      );
      await tester.pumpAndSettle();
      expect(_entryFinder('b'), findsNothing);
      expect(controller.entries, hasLength(3));
      await tester.enterText(find.byKey(const Key('library-search')), 'BIR');
      await tester.pumpAndSettle();
      expect(_entryFinder('b'), findsOneWidget);
      await tester.tap(find.byKey(const Key('library-filter-allOwned')));
      await tester.enterText(find.byKey(const Key('library-search')), '');
      await tester.pumpAndSettle();
      expect(_entryFinder('a'), findsOneWidget);
      expect(_entryFinder('z'), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'details use selected identity and do not enable unsupported actions',
    (tester) async {
      final controller = await _loaded([
        _entry('a', 'Amber'),
        _entry('b', 'Birch'),
      ]);
      await _show(tester, controller);
      await tester.pumpAndSettle();
      for (final (id, title) in [('a', 'Amber'), ('b', 'Birch')]) {
        await tester.tap(_entryFinder(id));
        await tester.pumpAndSettle();
        final details = find.byKey(const Key('library-detail'));
        expect(
          find.descendant(of: details, matching: find.text(title)),
          findsOneWidget,
        );
        expect(find.text('Fictional test record for $title.'), findsOneWidget);
        await _detailVisible(
          tester,
          find.text('Test installation evidence for $title.'),
        );
        expect(
          find.text('Test installation evidence for $title.'),
          findsOneWidget,
        );
        await _detailVisible(tester, find.byKey(const Key('library-launch')));
        expect(
          tester
              .widget<RyhzeControl>(find.byKey(const Key('library-launch')))
              .onPressed,
          isNull,
        );
        expect(
          tester
              .widget<RyhzeControl>(find.byKey(const Key('library-install')))
              .onPressed,
          isNull,
        );
        tester
            .widget<ListView>(find.byKey(const Key('library-detail-scroll')))
            .controller!
            .jumpTo(0);
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('library-back')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('library-detail')), findsNothing);
      }
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'demonstration label persists through filtering and selected detail',
    (tester) async {
      final controller = await _loaded([
        _entry('a', 'Amber', ownership: LibraryOwnershipState.demonstration),
        _entry('b', 'Birch', ownership: LibraryOwnershipState.demonstration),
      ], demo: true);
      await _show(tester, controller);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('library-demo-indicator')), findsOneWidget);
      await tester.enterText(find.byKey(const Key('library-search')), 'amber');
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('library-demo-indicator')), findsOneWidget);
      await tester.tap(_entryFinder('a'));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('library-demo-indicator')), findsOneWidget);
      await _detailVisible(tester, find.byKey(const Key('library-launch')));
      expect(
        tester
            .widget<RyhzeControl>(find.byKey(const Key('library-launch')))
            .onPressed,
        isNull,
      );
      expect(
        tester
            .widget<RyhzeControl>(find.byKey(const Key('library-install')))
            .onPressed,
        isNull,
      );
      expect(tester.takeException(), isNull);
    },
  );

  for (final size in [
    const Size(1440, 1000),
    const Size(1280, 800),
    const Size(390, 844),
    const Size(320, 640),
  ]) {
    for (final scale in [1.0, 1.6, 2.0]) {
      testWidgets(
        'Library collection and details fit ${size.width} text=$scale',
        (tester) async {
          final controller = await _loaded([
            _entry(
              'a',
              'A deliberately long fictional title that must wrap accessibly',
            ),
            _entry('b', 'Birch'),
          ]);
          await _show(
            tester,
            controller,
            size: size,
            scale: scale,
            reduced: true,
          );
          await tester.pumpAndSettle();
          expect(tester.takeException(), isNull);
          for (final container in tester.widgetList<AnimatedContainer>(
            find.byType(AnimatedContainer),
          )) {
            expect(container.duration, Duration.zero);
          }
          final tile = find.byKey(
            Key(
              '${find.byKey(const Key('library-grid')).evaluate().isNotEmpty ? 'library-card' : 'library-row'}-${_identity('a')}',
            ),
          );
          final collection = find.byKey(
            Key(
              find.byKey(const Key('library-grid')).evaluate().isNotEmpty
                  ? 'library-grid'
                  : 'library-list',
            ),
          );
          await tester.scrollUntilVisible(
            tile,
            350,
            scrollable: find
                .descendant(of: collection, matching: find.byType(Scrollable))
                .first,
            maxScrolls: 20,
          );
          await tester.pumpAndSettle();
          await tester.tap(tile);
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('library-detail')), findsOneWidget);
          expect(tester.takeException(), isNull);
          final back = find.byKey(const Key('library-back'));
          await tester.ensureVisible(back);
          await tester.pumpAndSettle();
          expect(tester.getSize(back).height, greaterThanOrEqualTo(48));
          await tester.tap(back);
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('library-detail')), findsNothing);
          expect(tester.takeException(), isNull);
        },
      );
    }
  }

  testWidgets(
    'stale asynchronous results never replace current UI or notify disposed widgets',
    (tester) async {
      final repository = _DeferredRepository();
      final controller = LibraryController(repository: repository);
      final first = controller.load();
      await _show(tester, controller);
      final second = controller.load();
      repository.requests[1].complete(
        LibraryLoadResult.verified([_entry('new', 'New answer')]),
      );
      await second;
      await tester.pumpAndSettle();
      expect(_entryFinder('new'), findsOneWidget);
      repository.requests[0].complete(
        LibraryLoadResult.verified([_entry('old', 'Stale answer')]),
      );
      await first;
      await tester.pumpAndSettle();
      expect(_entryFinder('new'), findsOneWidget);
      expect(_entryFinder('old'), findsNothing);
      final late = controller.load();
      await tester.pump();
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
      repository.requests.last.complete(
        LibraryLoadResult.failure('Late disposal result'),
      );
      await late;
      await tester.pump();
      expect(tester.takeException(), isNull);
    },
  );
}
