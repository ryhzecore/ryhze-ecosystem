import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../app/navigation_controller.dart';
import '../../app/app_services.dart';
import '../store/store_controller.dart';
import '../store/catalog_browse_controller.dart';
import '../../domain/contracts.dart';
import '../workspace/workspace_panel.dart';
import '../../design/controls.dart';
import '../../design/artwork.dart';
import '../../design/modal_frame.dart';
import '../../design/tokens.dart';
import '../library/library_controller.dart';
import '../library/library_screen.dart';
import 'model/catalog.dart';
import 'widgets/entertainment_header.dart';
import 'widgets/apple_touch_navigation.dart';
import 'widgets/store_sections.dart';
import 'widgets/about_panel.dart';
import 'widgets/search_panel.dart';
import 'widgets/title_detail.dart';

class EntertainmentScreen extends StatefulWidget {
  const EntertainmentScreen({super.key, required this.services});
  final AppServices services;
  @override
  State<EntertainmentScreen> createState() => _EntertainmentScreenState();
}

class _EntertainmentScreenState extends State<EntertainmentScreen> {
  NavigationController get navigation => widget.services.navigation;
  StoreController get store => widget.services.store;
  OverlayCoordinator get overlays => widget.services.overlays;
  EntertainmentCategory get category => navigation.category;
  bool get libraryDestination => navigation.libraryDestination;
  CatalogBrowseController get discovery => widget.services.discovery;
  EntertainmentTitle? get featured =>
      discovery.state(category).titles.firstOrNull;
  ValueNotifier<bool> get favourite =>
      store.favourite(featured?.id ?? Catalog.larcenous.id);
  final scroll = ScrollController();
  final heroLayout = GlobalKey();
  final searchFocus = FocusNode(debugLabel: 'Search trigger');
  final menuFocus = FocusNode(debugLabel: 'Menu trigger');
  final librarySearchFocus = FocusNode(debugLabel: 'Library search');
  final shellFocus = FocusNode(debugLabel: 'Ryhze navigation');
  LibraryController get library => widget.services.library;

  @override
  void initState() {
    super.initState();
    navigation.addListener(navigationChanged);
    store.addListener(storeChanged);
    discovery.addListener(discoveryChanged);
    if (!discovery.usesBundledCatalogue) unawaited(discovery.load(category));
    widget.services.routes.addListener(routeChanged);
    overlays.addListener(overlayChanged);
    // Cold activation may resolve before the first widget is mounted.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) routeChanged();
    });
  }

  @override
  void dispose() {
    widget.services.routes.removeListener(routeChanged);
    overlays.removeListener(overlayChanged);
    navigation.removeListener(navigationChanged);
    store.removeListener(storeChanged);
    discovery.removeListener(discoveryChanged);
    scroll.dispose();
    searchFocus.dispose();
    menuFocus.dispose();
    librarySearchFocus.dispose();
    shellFocus.dispose();
    super.dispose();
  }

  Future<void>? titlePresentation;

  void overlayChanged() {
    if (!overlays.isOpen &&
        widget.services.routes.value.hasPendingPresentation) {
      routeChanged();
    }
  }

  void routeChanged() {
    if (!mounted || overlays.isOpen) return;
    final routes = widget.services.routes;
    if (routes.takeActivity()) {
      unawaited(openWorkspace(activity: true));
      return;
    }
    final quality = routes.value.quality;
    final title = routes.takeTitle();
    if (title != null) titlePresentation = showTitle(title, quality: quality);
    final failure = routes.value.failure;
    if (failure != null) {
      ScaffoldMessenger.of(context)
          .showSnackBar(SnackBar(content: Text(failure.message)));
    }
  }

  void storeChanged() {
    final failure = store.failure;
    if (mounted && failure != null) {
      ScaffoldMessenger.of(context)
          .showSnackBar(SnackBar(content: Text(failure.message)));
    }
  }

  void navigationChanged() {
    if (!mounted) return;
    setState(() {});
    if (navigation.destination != AppDestination.gamesLibrary &&
        discovery.state(category).read == null) {
      unawaited(discovery.load(category));
    }
  }

  void discoveryChanged() {
    if (mounted) setState(() {});
  }

  void selectCategory(EntertainmentCategory value) {
    if (category == value) return;
    if (scroll.hasClients) navigation.rememberScroll(scroll.offset);
    navigation.selectCategory(value);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && scroll.hasClients) {
        scroll.jumpTo(
          navigation.scrollOffset.clamp(0, scroll.position.maxScrollExtent),
        );
      }
    });
  }

  void selectDestination(AppDestination destination) {
    if (overlays.isOpen) return;
    shellFocus.requestFocus();
    if (navigation.destination == destination) return;
    if (scroll.hasClients &&
        navigation.destination != AppDestination.gamesLibrary) {
      navigation.rememberScroll(scroll.offset);
    }
    navigation.go(AppRoute(destination));
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || navigation.destination != destination) return;
      if (scroll.hasClients && destination != AppDestination.gamesLibrary) {
        scroll.jumpTo(
          navigation.scrollOffset.clamp(0, scroll.position.maxScrollExtent),
        );
      }
    });
  }

  Future<T?> openFrame<T>(Widget child) => overlays.run(
    () => Navigator.of(context).push<T>(
      RyhzeDialogRoute<T>(
        child: child,
        enter: RyhzeTokens.motion(context, RyhzeTokens.frameIn),
        exit: RyhzeTokens.motion(context, RyhzeTokens.frameOut),
      ),
    ),
  );

  void toggleFavourite() {
    final title = featured;
    if (title != null) unawaited(store.toggle(title.id, title: title.title));
  }

  Future<void> openTitle([EntertainmentTitle? selected]) async {
    final title = selected ?? featured;
    if (title == null) return;
    titlePresentation = null;
    await widget.services.routes.activate(
      AppRoute(
        category == EntertainmentCategory.films
            ? AppDestination.filmsStore
            : AppDestination.gamesStore,
        itemId: title.id,
      ),
    );
    await titlePresentation;
  }

  Future<void> showTitle(
    EntertainmentTitle title, {
    ReadQuality quality = ReadQuality.complete,
  }) async => openFrame<void>(
    TitleDetail(
      title: title,
      category: category,
      metadataNotice: switch (quality) {
        ReadQuality.stale => 'Saved catalogue information. Online information could not be refreshed.',
        ReadQuality.partial =>
          'Bundled catalogue information. Online information is unavailable.',
        _ => null,
      },
      favourite: store.favourite(title.id),
      onFavourite: () => store.toggle(title.id, title: title.title),
      persistent: widget.services.storage.durable,
    ),
  );
  Future<void> openSearch() async {
    if (overlays.isOpen) return;
    if (category == EntertainmentCategory.games && libraryDestination) {
      if (library.selectedEntry == null) {
        librarySearchFocus.requestFocus();
        return;
      }
      library.back();
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted &&
            category == EntertainmentCategory.games &&
            libraryDestination) {
          librarySearchFocus.requestFocus();
        }
      });
      return;
    }
    final selected = await openFrame<EntertainmentTitle>(
      SearchPanel(
        repository: widget.services.catalog,
        initialQuery: navigation.query,
        category: category,
        onQueryChanged: navigation.setQuery,
      ),
    );
    if (!mounted) return;
    if (selected != null) {
      if (category == EntertainmentCategory.games) {
        navigation.selectLibrary(false);
      }
      await openTitle(selected);
    }
    if (mounted) searchFocus.requestFocus();
  }

  Future<void> openWorkspace({
    bool settings = false,
    bool activity = false,
  }) async {
    if (overlays.isOpen) return;
    final services = widget.services;
    final route = await openFrame<AppRoute>(
      WorkspacePanel(
        controller: services.workspace,
        providers: services.providers,
        jobs: services.jobs.read(services.session.scope),
        diagnostics: services.diagnostics,
        durable: services.storage.durable,
        integrations: services.integrations,
        initialSettings: settings,
        initialActivity: activity,
        overlays: overlays,
      ),
    );
    if (mounted && route != null) await services.routes.activate(route);
    if (mounted && !overlays.isOpen) menuFocus.requestFocus();
  }

  Future<void> chooseMenuItem(String choice) async {
    switch (choice) {
      case 'games':
        selectCategory(EntertainmentCategory.games);
      case 'films':
        selectCategory(EntertainmentCategory.films);
      case 'workspace':
        await openWorkspace();
      case 'about':
        await openFrame<void>(const AboutPanel());
    }
    if (mounted && !overlays.isOpen) menuFocus.requestFocus();
  }

  @override
  Widget build(BuildContext context) => CallbackShortcuts(
    bindings: <ShortcutActivator, VoidCallback>{
      const SingleActivator(LogicalKeyboardKey.comma, control: true):
          openWorkspace,
      const SingleActivator(LogicalKeyboardKey.keyK, control: true): openSearch,
      const SingleActivator(LogicalKeyboardKey.keyK, meta: true): openSearch,
      if (Theme.of(context).platform == TargetPlatform.macOS ||
          Theme.of(context).platform == TargetPlatform.iOS) ...{
        const SingleActivator(LogicalKeyboardKey.digit1, meta: true): () =>
            selectDestination(AppDestination.gamesStore),
        const SingleActivator(LogicalKeyboardKey.digit2, meta: true): () =>
            selectDestination(AppDestination.gamesLibrary),
        const SingleActivator(LogicalKeyboardKey.digit3, meta: true): () =>
            selectDestination(AppDestination.filmsStore),
        const SingleActivator(LogicalKeyboardKey.comma, meta: true): () =>
            openWorkspace(settings: true),
      },
    },
    child: Focus(
      focusNode: shellFocus,
      autofocus: true,
      child: Scaffold(
        body: RyhzeAmbience(
          child: SafeArea(
            child: FocusTraversalGroup(
              policy: ReadingOrderTraversalPolicy(),
              child: LayoutBuilder(
                builder: (context, constraints) {
                  final width = constraints.maxWidth;
                  final browse = discovery.state(category);
                  final touchNavigation =
                      Theme.of(context).platform == TargetPlatform.iOS &&
                      width < 700 &&
                      constraints.maxHeight >= 400;
                  final gutter = StoreLayout.gutter(width);
                  final cardWidth = StoreLayout.cardWidth(width);
                  final cardGap = StoreLayout.cardGap(width);
                  final columns = StoreLayout.columns(width);
                  final showLibrary =
                      category == EntertainmentCategory.games &&
                      libraryDestination;
                  final minimumChrome =
                      showLibrary &&
                          MediaQuery.textScalerOf(context).scale(16) > 20.8
                      ? 48.0
                      : 80.0;
                  return Column(
                    children: [
                      ConstrainedBox(
                        constraints: BoxConstraints(
                          maxHeight: constraints.maxHeight < minimumChrome
                              ? constraints.maxHeight
                              : (constraints.maxHeight - 96).clamp(
                                  minimumChrome,
                                  constraints.maxHeight,
                                ),
                        ),
                        child: SingleChildScrollView(
                          key: const Key('app-chrome-scroll'),
                          child: SizedBox(
                            width: width,
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                EntertainmentHeader(
                                  overlayChanges: overlays,
                                  width: width,
                                  gutter: gutter,
                                  category: category,
                                  onCategory: selectCategory,
                                  onSearch: openSearch,
                                  searchLabel: showLibrary
                                      ? 'Search Library'
                                      : 'Search Ryhze',
                                  onMenuChoice: chooseMenuItem,
                                  searchFocus: searchFocus,
                                  menuFocus: menuFocus,
                                  showChooser: !touchNavigation,
                                  libraryDestination: libraryDestination,
                                  onDestination:
                                      !touchNavigation &&
                                          category ==
                                              EntertainmentCategory.games
                                      ? (library) => selectDestination(
                                          library
                                              ? AppDestination.gamesLibrary
                                              : AppDestination.gamesStore,
                                        )
                                      : null,
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                      Expanded(
                        child: IndexedStack(
                          index: showLibrary ? 1 : 0,
                          children: [
                            TickerMode(
                              enabled: !showLibrary,
                              child: ExcludeFocus(
                                excluding: showLibrary,
                                child: Scrollbar(
                                  controller: scroll,
                                  child: CustomScrollView(
                                    controller: scroll,
                                    key: const Key('catalog-scroll'),
                                    slivers: [
                                      if (featured != null)
                                        SliverToBoxAdapter(
                                          child: AnimatedSwitcher(
                                            key: heroLayout,
                                            duration: RyhzeTokens.motion(
                                              context,
                                              RyhzeTokens.frameIn,
                                            ),
                                            switchInCurve: RyhzeTokens.ease,
                                            switchOutCurve: RyhzeTokens.ease,
                                            child: StoreHero(
                                              key: ValueKey(category),
                                              width: width,
                                              gutter: gutter,
                                              category: category,
                                              title:
                                                  featured ?? Catalog.larcenous,
                                              favourite: favourite,
                                              onFavourite: toggleFavourite,
                                              onExplore: openTitle,
                                            ),
                                          ),
                                        ),
                                      if (!discovery.usesBundledCatalogue)
                                        SliverPadding(
                                          padding: EdgeInsets.fromLTRB(
                                            gutter,
                                            20,
                                            gutter,
                                            0,
                                          ),
                                          sliver: SliverToBoxAdapter(
                                            child: Semantics(
                                              liveRegion: true,
                                              child: Wrap(
                                                spacing: 16,
                                                runSpacing: 12,
                                                crossAxisAlignment:
                                                    WrapCrossAlignment.center,
                                                children: [
                                                  Text(
                                                    browse.loading
                                                        ? 'Refreshing catalogue…'
                                                        : browse
                                                                  .read
                                                                  ?.quality ==
                                                              ReadQuality.stale
                                                        ? 'Saved catalogue · Could not refresh. Availability may have changed.'
                                                        : browse
                                                                  .read
                                                                  ?.quality ==
                                                              ReadQuality
                                                                  .partial
                                                        ? 'Bundled catalogue · Online information is unavailable.'
                                                        : browse.failure != null
                                                        ? browse
                                                              .failure!
                                                              .message
                                                        : browse.read == null
                                                        ? 'No public catalogue metadata loaded.'
                                                        : 'Public catalogue · Ownership and downloads are separate.',
                                                  ),
                                                  RyhzeControl(
                                                    key: const Key(
                                                      'catalog-refresh',
                                                    ),
                                                    label: 'Refresh catalogue',
                                                    onPressed: browse.loading
                                                        ? null
                                                        : () => discovery.load(
                                                            category,
                                                          ),
                                                    child: const Text(
                                                      'Refresh',
                                                    ),
                                                  ),
                                                ],
                                              ),
                                            ),
                                          ),
                                        ),
                                      SliverPadding(
                                        padding: EdgeInsets.fromLTRB(
                                          gutter,
                                          width <= 760 ? 42 : 64,
                                          gutter,
                                          32,
                                        ),
                                        sliver: SliverToBoxAdapter(
                                          child: CatalogHeading(
                                            category: category,
                                            compact: width <= 700,
                                            width: width,
                                            count: browse.titles.length,
                                          ),
                                        ),
                                      ),
                                      SliverPadding(
                                        padding: EdgeInsets.symmetric(
                                          horizontal: gutter,
                                        ),
                                        sliver: SliverList.builder(
                                          itemCount: browse.titles.isEmpty
                                              ? (browse.loading ? 0 : 1)
                                              : (browse.titles.length / columns)
                                                    .ceil(),
                                          itemBuilder: (context, index) {
                                            if (browse.titles.isEmpty) {
                                              return Text(
                                                browse.failure == null
                                                    ? browse.read == null
                                                          ? 'Refresh to load the catalogue.'
                                                          : category ==
                                                                EntertainmentCategory
                                                                    .games
                                                          ? 'No games available.'
                                                          : 'No films available yet.'
                                                    : 'The catalogue is unavailable. You can retry without losing your saved collection.',
                                              );
                                            }
                                            return Padding(
                                              padding: EdgeInsets.only(
                                                bottom: cardGap,
                                              ),
                                              child: Row(
                                                crossAxisAlignment:
                                                    CrossAxisAlignment.start,
                                                children: [
                                                  for (
                                                    var column = 0;
                                                    column < columns &&
                                                        index * columns +
                                                                column <
                                                            browse
                                                                .titles
                                                                .length;
                                                    column++
                                                  ) ...[
                                                    if (column > 0)
                                                      SizedBox(width: cardGap),
                                                    SizedBox(
                                                      key: ValueKey(
                                                        browse
                                                            .titles[index *
                                                                    columns +
                                                                column]
                                                            .id,
                                                      ),
                                                      width: cardWidth,
                                                      child: GameTile(
                                                        category: category,
                                                        title:
                                                            browse
                                                                .titles[index *
                                                                    columns +
                                                                column],
                                                        onOpen: () => openTitle(
                                                          browse.titles[index *
                                                                  columns +
                                                              column],
                                                        ),
                                                      ),
                                                    ),
                                                  ],
                                                ],
                                              ),
                                            );
                                          },
                                        ),
                                      ),
                                      SliverPadding(
                                        padding: EdgeInsets.fromLTRB(
                                          gutter,
                                          32,
                                          gutter,
                                          48,
                                        ),
                                        sliver: SliverToBoxAdapter(
                                          child: Column(
                                            crossAxisAlignment:
                                                CrossAxisAlignment.start,
                                            children: [
                                              const Text(
                                                'PRE-ALPHA showcase. Games and Films are still being built.',
                                                style: TextStyle(
                                                  color: RyhzeTokens.subtle,
                                                  fontSize: 11,
                                                  height: 1.7,
                                                ),
                                              ),
                                              ValueListenableBuilder<bool>(
                                                valueListenable: favourite,
                                                builder: (context, saved, _) =>
                                                    saved
                                                    ? Padding(
                                                        padding:
                                                            EdgeInsets.only(
                                                              top: 8,
                                                            ),
                                                        child: Text(
                                                          widget
                                                                  .services
                                                                  .storage
                                                                  .durable
                                                              ? 'Saved collection · Does not establish ownership.'
                                                              : 'Favourites are temporary and reset when you close the app.',
                                                          style: TextStyle(
                                                            color: RyhzeTokens
                                                                .subtle,
                                                            fontSize: 11,
                                                            height: 1.7,
                                                          ),
                                                        ),
                                                      )
                                                    : const SizedBox.shrink(),
                                              ),
                                            ],
                                          ),
                                        ),
                                      ),
                                    ],
                                  ),
                                ),
                              ),
                            ),
                            TickerMode(
                              enabled: showLibrary,
                              child: ExcludeFocus(
                                excluding: !showLibrary,
                                child: LibraryScreen(
                                  controller: library,
                                  searchFocusNode: librarySearchFocus,
                                  active: showLibrary,
                                ),
                              ),
                            ),
                          ],
                        ),
                      ),
                      if (touchNavigation)
                        AppleTouchNavigation(
                          selected: navigation.destination,
                          onSelected: selectDestination,
                        ),
                    ],
                  );
                },
              ),
            ),
          ),
        ),
      ),
    ),
  );
}
