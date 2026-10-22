import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../design/controls.dart';
import '../../design/tokens.dart';
import 'library_controller.dart';
import 'model/library_entry.dart';
import 'widgets/library_detail.dart';
import 'widgets/library_entry_tile.dart';

/// Library presentation owns focus and scroll only. Its injected controller owns
/// the session's data, filtering and selection; root navigation owns its lifetime.
class LibraryScreen extends StatefulWidget {
  const LibraryScreen({
    super.key,
    required this.controller,
    this.searchFocusNode,
    this.active = true,
  });

  final LibraryController controller;
  final FocusNode? searchFocusNode;
  final bool active;

  @override
  State<LibraryScreen> createState() => _LibraryScreenState();
}

class _LibraryScreenState extends State<LibraryScreen> {
  final _railScroll = ScrollController();
  final _gridScroll = ScrollController();
  final _listScroll = ScrollController();
  final _detailScroll = ScrollController();
  final _localSearchFocus = FocusNode(debugLabel: 'Library search');
  final _backFocus = FocusNode(debugLabel: 'Library Back');
  final _scope = FocusScopeNode(debugLabel: 'Library');
  final _collectionHeader = GlobalKey();
  final Map<String, FocusNode> _entryFocus = {};
  late final TextEditingController _search;
  FocusNode? _origin;
  LibraryIdentity? _originIdentity;
  LibraryIdentity? _previousSelection;
  int _focusRequest = 0;
  bool _twoAreas = false;
  double _collectionWidth = 0;

  FocusNode get _searchFocus => widget.searchFocusNode ?? _localSearchFocus;

  @override
  void initState() {
    super.initState();
    _search = TextEditingController(text: widget.controller.query);
    _previousSelection = widget.controller.selectedId;
    widget.controller.addListener(_changed);
  }

  @override
  void didUpdateWidget(covariant LibraryScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.controller != widget.controller) {
      _focusRequest++;
      oldWidget.controller.removeListener(_changed);
      widget.controller.addListener(_changed);
      _previousSelection = widget.controller.selectedId;
      _origin = null;
      _originIdentity = null;
      _synchronizeQuery();
    }
    if (!widget.active) {
      _focusRequest++;
      if (_scope.hasFocus) _scope.unfocus();
    }
  }

  void _synchronizeQuery() {
    if (_search.text == widget.controller.query) return;
    _search.value = TextEditingValue(
      text: widget.controller.query,
      selection: TextSelection.collapsed(
        offset: widget.controller.query.length,
      ),
    );
  }

  void _changed() {
    _synchronizeQuery();
    final selection = widget.controller.selectedId;
    if (_previousSelection != selection) {
      _previousSelection = selection;
      if (selection != null) {
        final request = ++_focusRequest;
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (!mounted || !widget.active || request != _focusRequest) return;
          if (widget.controller.selectedId != selection) return;
          if (_detailScroll.hasClients) _detailScroll.jumpTo(0);
          if (_backFocus.context != null) _backFocus.requestFocus();
        });
      }
    }
    setState(() {});
  }

  void _select(LibraryEntry entry, FocusNode origin) {
    _origin = origin;
    _originIdentity = entry.identity;
    widget.controller.select(entry.identity);
  }

  void _back() {
    widget.controller.back();
    final request = ++_focusRequest;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !widget.active || request != _focusRequest) return;
      if (widget.controller.selectedId != null) return;
      _restoreOrigin(request);
    });
  }

  void _restoreOrigin(
    int request, {
    double? previousOffset,
    double? previousExtent,
  }) {
    if (!mounted ||
        !widget.active ||
        request != _focusRequest ||
        widget.controller.selectedId != null) {
      return;
    }
    final identity = _originIdentity;
    final collectionKey = identity == null
        ? null
        : 'library-${_twoAreas ? 'card' : 'row'}-${libraryEntryKey(identity)}';
    final original = _origin;
    final target =
        original != null && original.context != null && original.canRequestFocus
        ? original
        : _entryFocus[collectionKey];
    if (target != null && target.context != null && target.canRequestFocus) {
      target.requestFocus();
      Scrollable.ensureVisible(
        target.context!,
        duration: Duration.zero,
        alignmentPolicy: ScrollPositionAlignmentPolicy.keepVisibleAtStart,
      );
      return;
    }
    final entries = widget.controller.visibleEntries;
    final index = entries.indexWhere((entry) => entry.identity == identity);
    final scroll = _twoAreas ? _gridScroll : _listScroll;
    final header = _collectionHeader.currentContext?.findRenderObject();
    if (index < 0 ||
        !scroll.hasClients ||
        header is! RenderBox ||
        !header.hasSize) {
      if (_searchFocus.context != null) _searchFocus.requestFocus();
      return;
    }
    var offset = header.size.height + 48;
    if (_twoAreas) {
      final columns = ((_collectionWidth - 32) / 248).floor().clamp(1, 4);
      final scale = MediaQuery.textScalerOf(context).scale(16) / 16;
      offset += (index ~/ columns) * (310 * scale + 16);
    } else {
      for (var row = 0; row < index; row++) {
        offset +=
            libraryRowExtent(context, entries[row], _collectionWidth - 32) + 12;
      }
    }
    final extent = scroll.position.maxScrollExtent;
    offset = offset.clamp(0.0, extent);
    // Lazy-list estimates can change once a distant row is laid out. Retry only
    // while layout makes progress, never by waiting for a timing threshold.
    if (offset == previousOffset && extent == previousExtent) {
      if (_searchFocus.context != null) _searchFocus.requestFocus();
      return;
    }
    scroll.jumpTo(offset);
    WidgetsBinding.instance.addPostFrameCallback(
      (_) => _restoreOrigin(
        request,
        previousOffset: offset,
        previousExtent: extent,
      ),
    );
    WidgetsBinding.instance.scheduleFrame();
  }

  @override
  void dispose() {
    widget.controller.removeListener(_changed);
    _focusRequest++;
    _railScroll.dispose();
    _gridScroll.dispose();
    _listScroll.dispose();
    _detailScroll.dispose();
    _search.dispose();
    _localSearchFocus.dispose();
    _backFocus.dispose();
    _scope.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => PopScope<Object?>(
    canPop: !widget.active || widget.controller.selectedEntry == null,
    onPopInvokedWithResult: (didPop, result) {
      if (!didPop && widget.active && widget.controller.selectedEntry != null) {
        _back();
      }
    },
    child: _focusScope(context),
  );

  Widget _focusScope(BuildContext context) => FocusScope(
    node: _scope,
    canRequestFocus: widget.active,
    descendantsAreFocusable: widget.active,
    descendantsAreTraversable: widget.active,
    skipTraversal: !widget.active,
    child: Shortcuts(
      shortcuts: {
        const SingleActivator(LogicalKeyboardKey.escape): const DismissIntent(),
        if (Theme.of(context).platform == TargetPlatform.macOS ||
            Theme.of(context).platform == TargetPlatform.iOS)
          const SingleActivator(LogicalKeyboardKey.bracketLeft, meta: true):
              const DismissIntent(),
      },
      child: Actions(
        actions: widget.controller.selectedEntry == null
            ? const <Type, Action<Intent>>{}
            : <Type, Action<Intent>>{
                DismissIntent: CallbackAction<DismissIntent>(
                  onInvoke: (_) {
                    _back();
                    return null;
                  },
                ),
              },
        child: LayoutBuilder(
          builder: (context, constraints) {
            final textScale = MediaQuery.textScalerOf(context).scale(16) / 16;
            final twoAreas =
                constraints.maxWidth >= 820 &&
                textScale <= 1.3 &&
                constraints.maxHeight >= 360;
            var shortViewport = constraints.maxHeight < 180;
            if (!shortViewport &&
                widget.controller.isDemo &&
                (Theme.of(context).platform == TargetPlatform.iOS ||
                    Theme.of(context).platform == TargetPlatform.macOS)) {
              // At large text sizes the full Demo banner can consume the
              // compact collection's usable height. Measure its actual text,
              // preserving the shorter truthful label and scrollable actions.
              final banner =
                  TextPainter(
                    text: TextSpan(
                      text: 'Demo Library · Fictional games and simulated statuses',
                      style: DefaultTextStyle.of(context).style
                          .merge(const TextStyle(fontSize: 12, height: 1.4)),
                    ),
                    textDirection: Directionality.of(context),
                    textScaler: MediaQuery.textScalerOf(context),
                  )..layout(
                    maxWidth: (constraints.maxWidth - (twoAreas ? 48 : 24) - 32)
                        .clamp(0.0, double.infinity),
                  );
              shortViewport = constraints.maxHeight - banner.height - 60 < 180;
              banner.dispose();
            }
            if (twoAreas != _twoAreas &&
                widget.active &&
                widget.controller.selectedEntry != null) {
              final request = ++_focusRequest;
              WidgetsBinding.instance.addPostFrameCallback((_) {
                if (!mounted ||
                    !widget.active ||
                    request != _focusRequest ||
                    widget.controller.selectedEntry == null) {
                  return;
                }
                if (_backFocus.context != null) _backFocus.requestFocus();
              });
            }
            _twoAreas = twoAreas;
            _collectionWidth = constraints.maxWidth - (twoAreas ? 328 : 24);
            return Padding(
              key: const Key('library-screen'),
              padding: EdgeInsets.fromLTRB(
                twoAreas ? 24 : 12,
                shortViewport ? 0 : 8,
                twoAreas ? 24 : 12,
                shortViewport ? 0 : 16,
              ),
              child: Column(
                children: [
                  if (widget.controller.isDemo) ...[
                    _demoBanner(compact: shortViewport),
                    if (!shortViewport) const SizedBox(height: 12),
                  ],
                  Expanded(child: twoAreas ? _desktop() : _compact()),
                ],
              ),
            );
          },
        ),
      ),
    ),
  );

  Widget _demoBanner({bool compact = false}) => Container(
    key: const Key('library-demo-indicator'),
    width: double.infinity,
    padding: EdgeInsets.symmetric(horizontal: 16, vertical: compact ? 4 : 12),
    decoration: BoxDecoration(
      color: RyhzeTokens.raised,
      borderRadius: BorderRadius.circular(RyhzeTokens.itemRadius),
      border: Border.all(color: RyhzeTokens.border),
    ),
    child: Text(
      compact
          ? 'Demo Library'
          : 'Demo Library · Fictional games and simulated statuses',
      semanticsLabel: 'Demo Library. Fictional games and simulated statuses.',
      style: const TextStyle(
        color: RyhzeTokens.accent,
        fontSize: 12,
        height: 1.4,
      ),
    ),
  );

  Widget _frame(Widget child) => DecoratedBox(
    decoration: BoxDecoration(
      color: RyhzeTokens.surface,
      borderRadius: BorderRadius.circular(RyhzeTokens.radius),
      border: Border.all(color: RyhzeTokens.border),
    ),
    child: ClipRRect(
      borderRadius: BorderRadius.circular(RyhzeTokens.radius),
      child: child,
    ),
  );

  Widget _desktop() {
    final entries = widget.controller.visibleEntries;
    return Row(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SizedBox(
          width: 264,
          child: _frame(
            Column(
              children: [
                Padding(
                  padding: const EdgeInsets.all(16),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      const Eyebrow('Library'),
                      const SizedBox(height: 12),
                      _searchField(),
                      const SizedBox(height: 12),
                      _filters(vertical: true),
                    ],
                  ),
                ),
                const Divider(height: 1),
                Expanded(
                  child: Scrollbar(
                    controller: _railScroll,
                    child: ListView.separated(
                      key: const Key('library-rail'),
                      controller: _railScroll,
                      padding: const EdgeInsets.all(12),
                      itemCount: entries.length,
                      separatorBuilder: (_, _) => const SizedBox(height: 8),
                      itemBuilder: (_, index) => _tile(entries[index]),
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
        const SizedBox(width: 16),
        Expanded(child: _frame(_content(compact: false))),
      ],
    );
  }

  Widget _compact() => _frame(_content(compact: true));

  Widget _content({required bool compact}) {
    final selected = widget.controller.selectedEntry;
    return Stack(
      fit: StackFit.expand,
      children: [
        Offstage(
          offstage: selected != null,
          child: ExcludeFocus(
            excluding: selected != null,
            child: _collection(compact: compact),
          ),
        ),
        if (selected != null)
          LibraryDetail(
            durableFavourites: widget.controller.durableFavourites,
            failureMessage: widget.controller.preferenceFailure?.message,
            verificationNotice: widget.controller.isStale
                ? widget.controller.statusMessage
                : null,
            key: const Key('library-detail'),
            entry: selected,
            favourite: widget.controller.isFavourite(selected.identity),
            onFavourite: () =>
                widget.controller.toggleFavourite(selected.identity),
            onBack: _back,
            backFocusNode: _backFocus,
            scrollController: _detailScroll,
          ),
      ],
    );
  }

  Widget _collection({required bool compact}) {
    final entries = widget.controller.visibleEntries;
    final scroll = compact ? _listScroll : _gridScroll;
    final hasResult =
        widget.controller.status == LibraryStatus.loaded ||
        widget.controller.status == LibraryStatus.demo;
    return Scrollbar(
      controller: scroll,
      child: CustomScrollView(
        key: Key(compact ? 'library-list' : 'library-grid'),
        controller: scroll,
        slivers: [
          SliverPadding(
            padding: EdgeInsets.symmetric(
              horizontal: _collectionWidth < 600 ? 16 : 24,
              vertical: 24,
            ),
            sliver: SliverToBoxAdapter(
              child: Column(
                key: _collectionHeader,
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Text(
                    widget.controller.isDemo ? 'Demo Library' : 'Your Library',
                    style: heading(30),
                  ),
                  const SizedBox(height: 12),
                  if (hasResult)
                    Text(
                      '${entries.length} ${widget.controller.isDemo ? 'demo' : 'owned'} '
                      '${entries.length == 1 ? 'game' : 'games'} · Title A–Z',
                      style: const TextStyle(
                        color: RyhzeTokens.subtle,
                        fontSize: 12,
                        height: 1.5,
                      ),
                    ),
                  if (hasResult &&
                      !widget.controller.isDemo &&
                      widget.controller.statusMessage.isNotEmpty) ...[
                    const SizedBox(height: 12),
                    Text(
                      widget.controller.statusMessage,
                      style: const TextStyle(
                        color: RyhzeTokens.accent,
                        fontSize: 13,
                        height: 1.5,
                      ),
                    ),
                  ],
                  if (compact) ...[
                    const SizedBox(height: 16),
                    _searchField(),
                    const SizedBox(height: 12),
                    _filters(vertical: false),
                  ],
                  const SizedBox(height: 16),
                  RyhzeControl(
                    key: const Key('library-demo-toggle'),
                    label: widget.controller.isDemo
                        ? 'Leave Demo Library'
                        : 'Explore Demo Library',
                    onPressed: widget.controller.isDemo
                        ? widget.controller.leaveDemo
                        : widget.controller.enterDemo,
                    child: Text(
                      widget.controller.isDemo
                          ? 'Leave Demo Library'
                          : 'Explore Demo Library',
                    ),
                  ),
                  if (hasResult &&
                      widget.controller.filter == LibraryFilter.installed) ...[
                    const SizedBox(height: 12),
                    const Text(
                      'Only confirmed installed games are shown. '
                      'Unknown installation statuses remain in All owned games.',
                      style: TextStyle(
                        color: RyhzeTokens.subtle,
                        fontSize: 12,
                        height: 1.5,
                      ),
                    ),
                  ],
                  if (!hasResult || entries.isEmpty) ...[
                    const SizedBox(height: 24),
                    _status(hasResult),
                  ],
                ],
              ),
            ),
          ),
          if (hasResult && entries.isNotEmpty)
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 24),
              sliver: compact
                  ? SliverList.builder(
                      itemCount: entries.length,
                      itemBuilder: (_, index) => Padding(
                        padding: const EdgeInsets.only(bottom: 12),
                        child: _tile(entries[index]),
                      ),
                    )
                  : SliverLayoutBuilder(
                      builder: (context, constraints) {
                        final columns = (constraints.crossAxisExtent / 248)
                            .floor()
                            .clamp(1, 4);
                        final scale =
                            MediaQuery.textScalerOf(context).scale(16) / 16;
                        return SliverGrid.builder(
                          itemCount: entries.length,
                          gridDelegate:
                              SliverGridDelegateWithFixedCrossAxisCount(
                                crossAxisCount: columns,
                                crossAxisSpacing: 16,
                                mainAxisSpacing: 16,
                                mainAxisExtent: 310 * scale,
                              ),
                          itemBuilder: (_, index) =>
                              _tile(entries[index], card: true),
                        );
                      },
                    ),
            ),
        ],
      ),
    );
  }

  Widget _tile(LibraryEntry entry, {bool card = false}) => LibraryEntryTile(
    key: Key(
      'library-${card ? 'card' : 'row'}-${libraryEntryKey(entry.identity)}',
    ),
    entry: entry,
    card: card,
    favourite: widget.controller.isFavourite(entry.identity),
    selected: widget.controller.selectedId == entry.identity,
    onSelected: (focus) => _select(entry, focus),
    onFocusNode: (node, attached) {
      final key =
          'library-${card ? 'card' : 'row'}-${libraryEntryKey(entry.identity)}';
      if (attached) {
        _entryFocus[key] = node;
      } else if (identical(_entryFocus[key], node)) {
        _entryFocus.remove(key);
      }
    },
  );

  Widget _searchField() => TextField(
    key: const Key('library-search'),
    controller: _search,
    focusNode: _searchFocus,
    onChanged: widget.controller.setQuery,
    style: const TextStyle(fontSize: 14, color: RyhzeTokens.text),
    decoration: InputDecoration(
      labelText: 'Search Library',
      labelStyle: const TextStyle(color: RyhzeTokens.muted),
      prefixIcon: const Icon(
        Icons.search_rounded,
        color: RyhzeTokens.subtle,
        size: 20,
      ),
      contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 16),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(24),
        borderSide: const BorderSide(color: RyhzeTokens.border),
      ),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(24),
        borderSide: const BorderSide(color: RyhzeTokens.border),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(24),
        borderSide: const BorderSide(color: RyhzeTokens.accent, width: 2),
      ),
    ),
  );

  Widget _filters({required bool vertical}) {
    final filters = LibraryFilter.values.map((filter) {
      final label = switch (filter) {
        LibraryFilter.allOwned =>
          widget.controller.isDemo ? 'All demo games' : 'All owned games',
        LibraryFilter.favourites => 'Favourites',
        LibraryFilter.installed => 'Installed',
      };
      return RyhzeControl(
        key: Key('library-filter-${filter.name}'),
        label: label,
        selected: widget.controller.filter == filter,
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
        onPressed: () => widget.controller.setFilter(filter),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(
              widget.controller.filter == filter
                  ? Icons.check_circle_outline
                  : Icons.circle_outlined,
              size: 18,
              color: widget.controller.filter == filter
                  ? RyhzeTokens.accent
                  : RyhzeTokens.subtle,
            ),
            const SizedBox(width: 8),
            Flexible(child: Text(label)),
          ],
        ),
      );
    }).toList();
    if (vertical) {
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          for (var index = 0; index < filters.length; index++) ...[
            if (index != 0) const SizedBox(height: 8),
            filters[index],
          ],
        ],
      );
    }
    return Wrap(spacing: 8, runSpacing: 8, children: filters);
  }

  Widget _status(bool hasResult) {
    final controller = widget.controller;
    final title = hasResult
        ? 'No matching games'
        : switch (controller.status) {
            LibraryStatus.unavailable => 'Library unavailable',
            LibraryStatus.loading => 'Loading Library',
            LibraryStatus.error => 'Library could not be loaded',
            LibraryStatus.verifiedEmpty => 'No owned games',
            LibraryStatus.loaded || LibraryStatus.demo => 'No matching games',
          };
    final message = hasResult
        ? 'Try another search or choose All ${controller.isDemo ? 'demo' : 'owned'} games.'
        : controller.status == LibraryStatus.verifiedEmpty
        ? 'This source has successfully verified an empty owned Library.'
        : controller.statusMessage;
    return Column(
      key: const Key('library-status'),
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Icon(
          controller.status == LibraryStatus.error
              ? Icons.error_outline_rounded
              : Icons.library_books_outlined,
          size: 32,
          color: RyhzeTokens.accent,
        ),
        const SizedBox(height: 16),
        Text(title, style: heading(24)),
        const SizedBox(height: 12),
        Text(
          message,
          style: const TextStyle(
            color: RyhzeTokens.muted,
            fontSize: 14,
            height: 1.7,
          ),
        ),
        if (controller.status == LibraryStatus.unavailable) ...[
          const SizedBox(height: 12),
          const Text(
            'Store entries, favourites and installed files do not establish ownership. '
            'Demo Library contains fictional games only.',
            style: TextStyle(
              color: RyhzeTokens.subtle,
              fontSize: 12,
              height: 1.6,
            ),
          ),
        ],
        if (controller.status == LibraryStatus.error) ...[
          const SizedBox(height: 16),
          RyhzeControl(
            key: const Key('library-retry'),
            label: 'Retry Library',
            onPressed: controller.load,
            child: const Text('Try again'),
          ),
        ],
      ],
    );
  }
}
