import 'package:flutter/material.dart';

import '../../core/app_failure.dart';
import '../../design/controls.dart';
import '../../design/tokens.dart';
import 'catalog_maintenance.dart';

class CatalogCachePanel extends StatefulWidget {
  const CatalogCachePanel({
    super.key,
    required this.maintenance,
    required this.clear,
  });
  final CatalogMaintenance maintenance;
  final Future<void> Function() clear;
  @override
  State<CatalogCachePanel> createState() => _CatalogCachePanelState();
}

class _CatalogCachePanelState extends State<CatalogCachePanel> {
  bool busy = false;
  String? message;
  @override
  Widget build(BuildContext context) {
    final health = widget.maintenance.cacheHealth();
    final localizations = MaterialLocalizations.of(context);
    final observedAt = health.observedAt?.toLocal();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const SizedBox(height: 24),
        Text('Public catalogue cache', style: heading(21)),
        const SizedBox(height: 12),
        Text(switch (health.state) {
          CatalogCacheState.empty => 'No saved catalogue metadata.',
          CatalogCacheState.fresh =>
            'Saved catalogue metadata is within its refresh period.',
          CatalogCacheState.stale =>
            'Saved catalogue metadata needs a refresh.',
          CatalogCacheState.expired =>
            'Saved metadata has expired and is not used for browsing.',
          CatalogCacheState.invalid =>
            'Saved metadata is unreadable. Your collections are separate.',
          CatalogCacheState.unavailable =>
            'Catalogue cache storage is unavailable.',
        }),
        if (observedAt != null)
          Text(
            'Last fetched: ${localizations.formatMediumDate(observedAt)} '
            '${localizations.formatTimeOfDay(TimeOfDay.fromDateTime(observedAt), alwaysUse24HourFormat: MediaQuery.alwaysUse24HourFormatOf(context))}',
          ),
        if (health.itemCount != null)
          Text(
            '${health.itemCount} public ${health.itemCount == 1 ? 'title' : 'titles'}',
          ),
        const SizedBox(height: 12),
        const Text(
          'Clearing removes only public catalogue metadata. Saved titles, account data and preferences are kept. Refresh the Store to fetch the catalogue again.',
        ),
        const SizedBox(height: 12),
        RyhzeControl(
          label: 'Clear catalogue cache',
          onPressed: busy ? null : clear,
          child: Text(busy ? 'Clearing…' : 'Clear catalogue cache'),
        ),
        if (message != null) Semantics(liveRegion: true, child: Text(message!)),
      ],
    );
  }

  Future<void> clear() async {
    setState(() {
      busy = true;
      message = null;
    });
    try {
      await widget.clear();
      if (mounted) setState(() => message = 'Catalogue cache cleared.');
    } catch (error) {
      if (mounted) {
        setState(
          () => message = error is AppFailure
              ? error.message
              : const AppFailure(FailureKind.storage).message,
        );
      }
    } finally {
      if (mounted) setState(() => busy = false);
    }
  }
}
