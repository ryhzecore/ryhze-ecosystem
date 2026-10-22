import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/design/header_menu.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/features/entertainment/entertainment_screen.dart';
import 'package:ryhze/features/library/data/library_repository.dart';
import 'package:ryhze/features/library/library_controller.dart';
import 'package:ryhze/features/library/library_screen.dart';
import 'package:ryhze/features/library/model/library_entry.dart';

class _Repository implements LibraryRepository {
  _Repository({int count = 3, bool varied = false})
    : entries = List.generate(count, (index) {
        final number = index.toString().padLeft(4, '0');
        return LibraryEntry(
          identity: LibraryIdentity(providerId: 'test', titleId: number),
          title:
              'Fictional game $number${varied && index % 3 == 0 ? ' with a deliberately long wrapped name across multiple compact lines' : ''}',
          ownership: const LibraryOwnership(
            state: LibraryOwnershipState.verifiedOwned,
            reason: 'Injected test ownership evidence.',
          ),
          installation: const LibraryInstallation(
            state: LibraryInstallationState.unknown,
            reason: 'No test installation source.',
          ),
        );
      });
  final List<LibraryEntry> entries;
  @override
  Future<LibraryLoadResult> load() async => LibraryLoadResult.verified(entries);
}

String _id(int index) => LibraryIdentity(
  providerId: 'test',
  titleId: index.toString().padLeft(4, '0'),
).toString();

Future<void> _show(
  WidgetTester tester,
  LibraryRepository repository, {
  Size size = const Size(1280, 800),
  double scale = 1,
  bool reduced = false,
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
        disableAnimations: reduced,
      ),
      child: RyhzeApp(libraryRepository: repository),
    ),
  );
  await tester.pumpAndSettle();
}

Future<void> _destination(WidgetTester tester, String destination) async {
  await tester.tap(find.byKey(Key('destination-$destination')));
  await tester.pumpAndSettle();
}

LibraryController _controller(WidgetTester tester) => tester
    .widget<LibraryScreen>(find.byType(LibraryScreen, skipOffstage: false))
    .controller;

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
    'Library game exposes a working assistive technology tap action',
    (tester) async {
      final semantics = tester.ensureSemantics();
      try {
        await _show(tester, _Repository());
        await _destination(tester, 'library');
        final target = find.byKey(Key('library-card-${_id(1)}'));
        final node = tester.getSemantics(target);
        expect(
          node.getSemanticsData().hasAction(ui.SemanticsAction.tap),
          isTrue,
        );
        tester.binding.performSemanticsAction(
          ui.SemanticsActionEvent(
            viewId: tester.view.viewId,
            nodeId: node.id,
            type: ui.SemanticsAction.tap,
          ),
        );
        await tester.pumpAndSettle();
        expect(
          _controller(tester).selectedId,
          const LibraryIdentity(providerId: 'test', titleId: '0001'),
        );
        expect(find.byKey(const Key('library-detail')), findsOneWidget);
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    },
  );

  for (final (name, before, beforeScale, after, afterScale) in [
    (
      'desktop to compact',
      const Size(1280, 800),
      1.0,
      const Size(390, 844),
      1.0,
    ),
    (
      'compact to desktop',
      const Size(390, 844),
      1.0,
      const Size(1280, 800),
      1.0,
    ),
    (
      'desktop to enlarged text',
      const Size(1280, 800),
      1.0,
      const Size(1280, 800),
      1.6,
    ),
  ]) {
    testWidgets(
      'Back restores non-first originating game after $name while details are open',
      (tester) async {
        final repository = _Repository(count: 100);
        await _show(tester, repository, size: before, scale: beforeScale);
        await _destination(tester, 'library');
        final beforeGrid = before.width >= 820 && beforeScale <= 1.3;
        final collection = find.byKey(
          Key(beforeGrid ? 'library-grid' : 'library-list'),
        );
        final target = find.byKey(
          Key('${beforeGrid ? 'library-card' : 'library-row'}-${_id(24)}'),
        );
        await tester.scrollUntilVisible(
          target,
          500,
          scrollable: find
              .descendant(of: collection, matching: find.byType(Scrollable))
              .first,
          maxScrolls: 30,
        );
        await tester.pumpAndSettle();
        await tester.tap(target);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('library-detail')), findsOneWidget);
        await _show(tester, repository, size: after, scale: afterScale);
        expect(find.byKey(const Key('library-detail')), findsOneWidget);
        await tester.sendKeyEvent(LogicalKeyboardKey.gameButtonB);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('library-detail')), findsNothing);
        final afterGrid = after.width >= 820 && afterScale <= 1.3;
        final restored = find.byKey(
          Key('${afterGrid ? 'library-card' : 'library-row'}-${_id(24)}'),
        );
        expect(restored.hitTestable(), findsOneWidget);
        expect(
          FocusManager.instance.primaryFocus?.debugLabel,
          'Library ${_id(24)}',
        );
        final afterCollection = find.byKey(
          Key(afterGrid ? 'library-grid' : 'library-list'),
        );
        final restoredScroll = tester
            .state<ScrollableState>(
              find
                  .descendant(
                    of: afterCollection,
                    matching: find.byType(Scrollable),
                  )
                  .first,
            )
            .position;
        expect(restoredScroll.pixels, greaterThan(0));
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'Back restores distant mixed-height identity after enlarged compact reflow',
    (tester) async {
      final repository = _Repository(count: 2000, varied: true);
      await _show(tester, repository);
      await _destination(tester, 'library');
      final target = find.byKey(Key('library-card-${_id(1500)}'));
      final collection = find.byKey(const Key('library-grid'));
      await tester.scrollUntilVisible(
        target,
        650,
        scrollable: find
            .descendant(of: collection, matching: find.byType(Scrollable))
            .first,
        maxScrolls: 400,
      );
      await tester.pumpAndSettle();
      await tester.tap(target);
      await tester.pumpAndSettle();
      expect(
        _controller(tester).selectedId,
        const LibraryIdentity(providerId: 'test', titleId: '1500'),
      );
      await _show(tester, repository, size: const Size(390, 844), scale: 1.6);
      expect(find.byKey(const Key('library-detail')), findsOneWidget);
      await tester.sendKeyEvent(LogicalKeyboardKey.gameButtonB);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('library-detail')), findsNothing);
      expect(
        find.byKey(Key('library-row-${_id(1500)}')).hitTestable(),
        findsOneWidget,
      );
      expect(
        FocusManager.instance.primaryFocus?.debugLabel,
        'Library ${_id(1500)}',
      );
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Store favourite cannot grant Library ownership or set Library favourites',
    (tester) async {
      final repository = _Repository();
      await _show(tester, repository);
      await tester.ensureVisible(find.text('Favourites').first);
      await tester.pumpAndSettle();
      await tester.tap(find.text('Favourites').first);
      await tester.pumpAndSettle();
      expect(find.text('Favourited'), findsOneWidget);
      await _destination(tester, 'library');
      final controller = _controller(tester);
      expect(controller.entries, hasLength(3));
      expect(
        controller.entries.every(
          (entry) => !controller.isFavourite(entry.identity),
        ),
        isTrue,
      );
      expect(find.text('Larcenous Driftscape'), findsNothing);
      await tester.tap(find.byKey(const Key('library-filter-favourites')));
      await tester.pumpAndSettle();
      expect(controller.visibleEntries, isEmpty);
      controller.toggleFavourite(repository.entries.first.identity);
      await tester.pumpAndSettle();
      expect(find.byKey(Key('library-card-${_id(0)}')), findsOneWidget);
      await _destination(tester, 'store');
      expect(find.text('Favourited'), findsOneWidget);
      await tester.ensureVisible(find.text('Favourited').first);
      await tester.pumpAndSettle();
      await tester.tap(find.text('Favourited').first);
      await tester.pumpAndSettle();
      await _destination(tester, 'library');
      expect(controller.isFavourite(repository.entries.first.identity), isTrue);
      expect(controller.entries, hasLength(3));
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Games destinations preserve Store state; Library query and selection also survive Films',
    (tester) async {
      await _show(tester, _Repository(count: 80));
      final storeScroll = tester
          .widget<CustomScrollView>(find.byKey(const Key('catalog-scroll')))
          .controller!;
      storeScroll.jumpTo(storeScroll.position.maxScrollExtent / 2);
      await tester.pumpAndSettle();
      final storeOffset = storeScroll.offset;
      await tester.tap(find.byKey(const Key('search-trigger')));
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byKey(const Key('search-input')),
        'larcenous',
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      await _destination(tester, 'library');
      await tester.enterText(find.byKey(const Key('library-search')), '002');
      await tester.pumpAndSettle();
      final controller = _controller(tester);
      await tester.tap(find.byKey(Key('library-card-${_id(2)}')));
      await tester.pumpAndSettle();
      expect(
        controller.selectedId,
        LibraryIdentity(providerId: 'test', titleId: '0002'),
      );
      await _destination(tester, 'store');
      expect(storeScroll.offset, closeTo(storeOffset, .01));
      await _destination(tester, 'library');
      expect(find.byKey(const Key('library-detail')), findsOneWidget);
      expect(controller.query, '002');
      await tester.tap(find.byKey(const Key('category-films')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('destination-library')), findsNothing);
      expect(
        find.descendant(
          of: find.byKey(const Key('hero-surface')),
          matching: find.textContaining(RegExp(r'Tommy\s+Riley')),
        ),
        findsOneWidget,
      );
      expect(find.byType(LibraryScreen), findsNothing);
      await tester.tap(find.byKey(const Key('category-games')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('library-detail')), findsOneWidget);
      expect(controller.query, '002');
      await _destination(tester, 'store');
      // Films keeps its existing catalogue scroll-reset behavior.
      await tester.tap(find.byKey(const Key('search-trigger')));
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('search-input')))
            .controller!
            .text,
        'larcenous',
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      await _destination(tester, 'library');
      expect(find.byKey(const Key('library-detail')), findsOneWidget);
      expect(controller.query, '002');
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'header Search and Control K focus Library search without opening Store results',
    (tester) async {
      await _show(tester, _Repository());
      await _destination(tester, 'library');
      await tester.tap(find.byKey(const Key('search-trigger')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('search-input')), findsNothing);
      final field = tester.widget<TextField>(
        find.byKey(const Key('library-search')),
      );
      expect(field.focusNode!.hasFocus, isTrue);
      await tester.enterText(find.byKey(const Key('library-search')), '0001');
      await tester.tap(find.byKey(const Key('destination-library')));
      await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
      await tester.sendKeyEvent(LogicalKeyboardKey.keyK);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
      await tester.pumpAndSettle();
      expect(field.focusNode!.hasFocus, isTrue);
      expect(find.byKey(const Key('search-input')), findsNothing);
      expect(_controller(tester).query, '0001');
      expect(tester.takeException(), isNull);
    },
  );

  for (final (size, keyPrefix) in [
    (const Size(1280, 800), 'library-card'),
    (const Size(390, 844), 'library-row'),
  ]) {
    for (final dismissal in ['back', 'escape', 'controller-b']) {
      testWidgets(
        'Library $dismissal restores originating focus and scroll at ${size.width}',
        (tester) async {
          await _show(tester, _Repository(count: 100), size: size);
          await _destination(tester, 'library');
          final collection = find.byKey(
            Key(size.width > 700 ? 'library-grid' : 'library-list'),
          );
          final scrollable = find
              .descendant(of: collection, matching: find.byType(Scrollable))
              .first;
          final target = find.byKey(Key('$keyPrefix-${_id(24)}'));
          await tester.scrollUntilVisible(
            target,
            500,
            scrollable: scrollable,
            maxScrolls: 30,
          );
          await tester.pumpAndSettle();
          final position = tester.state<ScrollableState>(scrollable).position;
          final offset = position.pixels;
          expect(offset, greaterThan(0));
          await tester.tap(target);
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('library-detail')), findsOneWidget);
          if (dismissal == 'back') {
            await tester.tap(find.byKey(const Key('library-back')));
          } else {
            await tester.sendKeyEvent(
              dismissal == 'escape'
                  ? LogicalKeyboardKey.escape
                  : LogicalKeyboardKey.gameButtonB,
            );
          }
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('library-detail')), findsNothing);
          expect(
            FocusManager.instance.primaryFocus?.debugLabel,
            'Library ${_id(24)}',
          );
          expect(position.pixels, closeTo(offset, .01));
          expect(target.hitTestable(), findsOneWidget);
          expect(tester.takeException(), isNull);
        },
      );
    }
  }

  testWidgets(
    'Library keyboard Enter, arrows and synthetic controller A select title-specific details',
    (tester) async {
      await _show(tester, _Repository());
      await _destination(tester, 'library');
      await tester.tap(find.byKey(Key('library-card-${_id(0)}')));
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      await tester.pumpAndSettle();
      expect(
        FocusManager.instance.primaryFocus?.debugLabel,
        'Library ${_id(1)}',
      );
      final focusedTile = find.byKey(Key('library-card-${_id(1)}'));
      final decoration =
          tester
                  .widget<AnimatedContainer>(
                    find
                        .descendant(
                          of: focusedTile,
                          matching: find.byType(AnimatedContainer),
                        )
                        .first,
                  )
                  .decoration!
              as BoxDecoration;
      expect((decoration.border! as Border).top.color, RyhzeTokens.accent);
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await tester.pumpAndSettle();
      expect(
        _controller(tester).selectedId,
        LibraryIdentity(providerId: 'test', titleId: '0001'),
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.gameButtonB);
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.gameButtonA);
      await tester.pumpAndSettle();
      expect(
        _controller(tester).selectedId,
        LibraryIdentity(providerId: 'test', titleId: '0001'),
      );
      expect(tester.takeException(), isNull);
    },
  );

  for (final reduced in [false, true]) {
    testWidgets(
      'Library keeps menu lifecycle and state through resize, text scale and detail reflow, reduced=$reduced',
      (tester) async {
        final repository = _Repository();
        await _show(tester, repository, reduced: reduced);
        await _destination(tester, 'library');
        await tester.enterText(find.byKey(const Key('library-search')), '0001');
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(Key('library-card-${_id(1)}')));
        await tester.pumpAndSettle();
        final screenState = tester.state(find.byType(EntertainmentScreen));
        final menuFocus = tester
            .widget<HeaderMenu>(find.byType(HeaderMenu))
            .focusNode;
        final controller = _controller(tester);
        for (final (size, scale) in [
          (const Size(320, 640), 2.0),
          (const Size(1280, 800), 1.6),
          (const Size(390, 844), 1.0),
        ]) {
          await tester.tap(find.byKey(const Key('menu-trigger')));
          await tester.pumpAndSettle();
          expect(find.byType(MenuItemButton), findsNWidgets(4));
          await _show(
            tester,
            repository,
            size: size,
            scale: scale,
            reduced: reduced,
          );
          expect(
            tester.state(find.byType(EntertainmentScreen)),
            same(screenState),
          );
          expect(
            tester.widget<HeaderMenu>(find.byType(HeaderMenu)).focusNode,
            same(menuFocus),
          );
          expect(_controller(tester), same(controller));
          expect(controller.query, '0001');
          expect(
            controller.selectedId,
            LibraryIdentity(providerId: 'test', titleId: '0001'),
          );
          if (find.byType(MenuItemButton).evaluate().isNotEmpty) {
            await tester.sendKeyEvent(LogicalKeyboardKey.escape);
            await tester.pumpAndSettle();
          }
          expect(find.byType(MenuItemButton), findsNothing);
          await tester.tap(find.byKey(const Key('menu-trigger')));
          await tester.pumpAndSettle();
          await tester.sendKeyEvent(LogicalKeyboardKey.escape);
          await tester.pumpAndSettle();
          expect(FocusManager.instance.primaryFocus, same(menuFocus));
          expect(find.byKey(const Key('library-detail')), findsOneWidget);
          expect(tester.takeException(), isNull);
        }
      },
    );
  }
}
