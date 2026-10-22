import 'package:flutter/material.dart';

import '../../design/controls.dart';
import '../../design/tokens.dart';
import '../../domain/contracts.dart';
import 'installation_controller.dart';

class InstallationPanel extends StatefulWidget {
  const InstallationPanel({super.key, required this.controller});
  final InstallationController controller;
  @override
  State<InstallationPanel> createState() => _InstallationPanelState();
}

class _InstallationPanelState extends State<InstallationPanel> {
  int page = 0;
  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: widget.controller,
    builder: (context, _) {
      final controller = widget.controller;
      final result = controller.read;
      final records = result?.values.toList() ?? [];
      final pages = ((records.length + 49) ~/ 50).clamp(1, 20);
      page = page.clamp(0, pages - 1);
      return Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('Device installations', style: heading(26)),
          const SizedBox(height: 12),
          const Text(
            'Inspect Steam’s local installation records. These files do not verify ownership, completeness or permission to play. They remain separate from your Library.',
          ),
          const SizedBox(height: 16),
          Wrap(
            spacing: 12,
            runSpacing: 12,
            children: [
              RyhzeControl(
                label: 'Inspect Steam installations',
                onPressed: controller.busy ? null : controller.inspect,
                child: Text(
                  controller.busy
                      ? 'Inspecting…'
                      : 'Inspect Steam installations',
                ),
              ),
              if (controller.busy)
                RyhzeControl(
                  label: 'Cancel inspection',
                  onPressed: controller.cancel,
                  child: const Text('Cancel'),
                ),
            ],
          ),
          const SizedBox(height: 16),
          if (controller.failure != null)
            Semantics(
              liveRegion: true,
              child: Text(controller.failure!.message),
            ),
          if (result != null)
            Text(switch (result.quality) {
              ReadQuality.complete => 'Local inspection completed.',
              ReadQuality.partial =>
                'Some declared installation records could not be inspected.',
              ReadQuality.stale => 'Showing the previous inspection. Current installation state is unverified.',
              ReadQuality.unavailable =>
                'Steam installation records are unavailable on this device.',
            }),
          if (result?.quality == ReadQuality.complete && records.isEmpty)
            const Text(
              'No local installation records were found in Steam’s declared libraries.',
            ),
          for (final item in records.skip(page * 50).take(50))
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(item.title, style: heading(20)),
                  Text('Steam app ${item.item.item} · Ownership not verified'),
                  const SizedBox(height: 6),
                  SelectableText(
                    '${item.libraryRoot}\\${item.relativeDirectory}',
                  ),
                ],
              ),
            ),
          if (pages > 1)
            Wrap(
              spacing: 12,
              runSpacing: 8,
              children: [
                RyhzeControl(
                  label: 'Previous installation page',
                  onPressed: page == 0 ? null : () => setState(() => page--),
                  child: const Text('Previous'),
                ),
                Text('Page ${page + 1} of $pages'),
                RyhzeControl(
                  label: 'Next installation page',
                  onPressed: page + 1 >= pages
                      ? null
                      : () => setState(() => page++),
                  child: const Text('Next'),
                ),
              ],
            ),
        ],
      );
    },
  );
}
