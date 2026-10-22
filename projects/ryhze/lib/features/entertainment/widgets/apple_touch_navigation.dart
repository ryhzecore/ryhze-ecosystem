import 'package:flutter/material.dart';

import '../../../app/navigation_controller.dart';
import '../../../design/controls.dart';
import '../../../design/tokens.dart';

/// Compact iOS presentation of the shared shell's typed destinations.
/// The shell switches to its scrollable header in short keyboard viewports.
class AppleTouchNavigation extends StatelessWidget {
  const AppleTouchNavigation({
    super.key,
    required this.selected,
    required this.onSelected,
  });

  final AppDestination selected;
  final ValueChanged<AppDestination> onSelected;

  @override
  Widget build(BuildContext context) => DecoratedBox(
    key: const Key('apple-touch-navigation'),
    decoration: const BoxDecoration(
      color: RyhzeTokens.background,
      border: Border(top: BorderSide(color: RyhzeTokens.border)),
    ),
    child: Padding(
      padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Activity is an owned workspace surface, not a fourth browse tab.
          for (final (destination, key, label, icon, text) in const [
            (
              AppDestination.gamesStore,
              'destination-store',
              'Games Store',
              Icons.storefront_outlined,
              'Store',
            ),
            (
              AppDestination.gamesLibrary,
              'destination-library',
              'Games Library',
              Icons.grid_view_rounded,
              'Library',
            ),
            (
              AppDestination.filmsStore,
              'destination-films',
              'Films',
              Icons.movie_outlined,
              'Films',
            ),
          ]) ...[
            if (destination != AppDestination.gamesStore)
              const SizedBox(width: 8),
            Expanded(
              child: RyhzeControl(
                key: Key(key),
                label: label,
                selected: selected == destination,
                padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 8),
                onPressed: () => onSelected(destination),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    ExcludeSemantics(
                      child: Icon(
                        icon,
                        color: selected == destination
                            ? RyhzeTokens.accent
                            : RyhzeTokens.muted,
                      ),
                    ),
                    const SizedBox(height: 4),
                    Text(
                      text,
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        fontSize: 12,
                        color: selected == destination
                            ? RyhzeTokens.accent
                            : RyhzeTokens.muted,
                        fontWeight: selected == destination
                            ? FontWeight.w600
                            : FontWeight.w400,
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ],
        ],
      ),
    ),
  );
}
