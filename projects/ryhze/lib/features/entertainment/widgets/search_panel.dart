import 'package:flutter/material.dart';

import '../../../design/controls.dart';
import '../../../design/tokens.dart';
import '../model/catalog.dart';
import '../../store/search_controller.dart';
import '../../store/catalog_repository.dart';
import '../../../domain/contracts.dart';

class SearchPanel extends StatefulWidget {
  const SearchPanel({
    super.key,
    required this.repository,
    this.initialQuery = '',
    this.onQueryChanged,
    this.category = EntertainmentCategory.games,
  });
  final CatalogRepository repository;
  final EntertainmentCategory category;
  final String initialQuery;
  final ValueChanged<String>? onQueryChanged;
  @override
  State<SearchPanel> createState() => _SearchPanelState();
}

class _SearchPanelState extends State<SearchPanel> {
  late String query = widget.initialQuery;
  late final TextEditingController input = TextEditingController(text: query);
  late final CatalogSearchController search;
  @override
  void initState() {
    super.initState();
    search = CatalogSearchController(
      widget.repository,
      widget.category,
      debounce: Duration.zero,
    );
    search.addListener(changed);
    search.search(query);
  }

  void changed() {
    if (mounted) setState(() {});
  }

  @override
  void dispose() {
    search.removeListener(changed);
    search.dispose();
    input.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final results = search.value.result?.values ?? const <EntertainmentTitle>[];
    return Dialog(
      alignment: Alignment.topCenter,
      backgroundColor: RyhzeTokens.raised,
      insetPadding: EdgeInsets.fromLTRB(
        16,
        ((MediaQuery.sizeOf(context).height -
                    MediaQuery.viewInsetsOf(context).bottom) *
                .1)
            .clamp(8, 100),
        16,
        MediaQuery.viewInsetsOf(context).bottom > 0 ? 8 : 24,
      ),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(RyhzeTokens.radius),
        side: const BorderSide(color: RyhzeTokens.border),
      ),
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 620),
        child: SingleChildScrollView(
          padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              ConstrainedBox(
                constraints: const BoxConstraints(minHeight: 52),
                child: Row(
                  children: [
                    const Icon(Icons.search_rounded, size: 20),
                    const SizedBox(width: 14),
                    Expanded(
                      child: Semantics(
                        label: 'Search titles',
                        child: TextField(
                          key: const Key('search-input'),
                          controller: input,
                          autofocus: true,
                          onChanged: (value) {
                            query = value;
                            search.search(value);
                            widget.onQueryChanged?.call(value);
                          },
                          onSubmitted: (_) {
                            if (results.isNotEmpty) {
                              Navigator.pop(context, results.first);
                            }
                          },
                          style: const TextStyle(
                            color: RyhzeTokens.text,
                            fontSize: 16,
                          ),
                          decoration: const InputDecoration(
                            hintText: 'Search Games and Films',
                            hintStyle: TextStyle(color: RyhzeTokens.subtle),
                            contentPadding: EdgeInsets.symmetric(vertical: 12),
                            enabledBorder: UnderlineInputBorder(
                              borderSide: BorderSide(color: Colors.transparent),
                            ),
                            focusedBorder: UnderlineInputBorder(
                              borderSide: BorderSide(color: RyhzeTokens.text),
                            ),
                          ),
                        ),
                      ),
                    ),
                    const SizedBox(width: 8),
                    RyhzeControl(
                      label: 'Close search',
                      iconOnly: true,
                      onPressed: () => Navigator.pop(context),
                      child: const Icon(Icons.close_rounded),
                    ),
                  ],
                ),
              ),
              const Divider(height: 1),
              const SizedBox(height: 6),
              if (!search.value.loading &&
                  search.value.result != null &&
                  search.value.result!.quality != ReadQuality.complete)
                Padding(
                  padding: const EdgeInsets.only(bottom: 12),
                  child: Text(switch (search.value.result!.quality) {
                    ReadQuality.stale => 'Showing previously fetched catalogue information. The service could not be refreshed.',
                    ReadQuality.partial => 'Showing local catalogue information. The service could not be reached.',
                    _ =>
                      search.value.result!.failure?.message ??
                          'Catalogue information is unavailable.',
                  }),
                ),
              if (search.value.loading)
                const Padding(
                  padding: EdgeInsets.all(24),
                  child: Text('Searching…'),
                )
              else if (search.value.failure != null)
                Text(search.value.failure!.message)
              else if (results.isEmpty)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 24),
                  child: Semantics(
                    liveRegion: true,
                    child: const Text(
                      'No matching titles. Try another search.',
                      style: TextStyle(color: RyhzeTokens.subtle),
                    ),
                  ),
                )
              else
                ConstrainedBox(
                  constraints: const BoxConstraints(maxHeight: 300),
                  child: ListView.builder(
                    shrinkWrap: true,
                    itemCount: results.length,
                    itemBuilder: (context, index) {
                      final result = results[index];
                      return Semantics(
                        label: 'Explore ${result.title}',
                        child: TextButton(
                          onPressed: () => Navigator.pop(context, result),
                          style: ButtonStyle(
                            animationDuration: RyhzeTokens.motion(
                              context,
                              RyhzeTokens.surfaceMotion,
                            ),
                            splashFactory: NoSplash.splashFactory,
                            minimumSize: const WidgetStatePropertyAll(
                              Size(0, 76),
                            ),
                            padding: const WidgetStatePropertyAll(
                              EdgeInsets.symmetric(vertical: 12),
                            ),
                            foregroundColor: const WidgetStatePropertyAll(
                              RyhzeTokens.text,
                            ),
                            backgroundColor: const WidgetStatePropertyAll(
                              Colors.transparent,
                            ),
                            overlayColor: const WidgetStatePropertyAll(
                              Color(0x08FFFFFF),
                            ),
                            textStyle: const WidgetStatePropertyAll(
                              TextStyle(
                                fontFamily: 'Inter',
                                fontSize: 14,
                                fontWeight: FontWeight.w400,
                                height: 1.4,
                              ),
                            ),
                            side: WidgetStateProperty.resolveWith(
                              (states) => BorderSide(
                                color:
                                    states.contains(WidgetState.focused) &&
                                        FocusManager.instance.highlightMode ==
                                            FocusHighlightMode.traditional
                                    ? RyhzeTokens.focus
                                    : Colors.transparent,
                              ),
                            ),
                            shape: WidgetStatePropertyAll(
                              RoundedRectangleBorder(
                                borderRadius: BorderRadius.circular(
                                  RyhzeTokens.itemRadius,
                                ),
                              ),
                            ),
                          ),
                          child: Row(
                            children: [
                              Expanded(
                                child: Column(
                                  crossAxisAlignment: CrossAxisAlignment.start,
                                  children: [
                                    Text(result.title),
                                    const SizedBox(height: 6),
                                    Text(
                                      '${widget.category == EntertainmentCategory.films ? 'Film' : 'Game'} · ${result.status}',
                                      style: const TextStyle(
                                        color: RyhzeTokens.subtle,
                                        fontSize: 11,
                                      ),
                                    ),
                                  ],
                                ),
                              ),
                              const SizedBox(width: 20),
                              const Icon(Icons.arrow_forward_rounded, size: 20),
                            ],
                          ),
                        ),
                      );
                    },
                  ),
                ),
              const SizedBox(height: 6),
            ],
          ),
        ),
      ),
    );
  }
}
