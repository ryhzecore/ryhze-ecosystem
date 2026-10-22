import 'package:flutter/material.dart';

import '../store/artwork_cache_panel.dart';

import '../../core/diagnostics.dart';
import '../../app/navigation_controller.dart';
import '../../core/app_failure.dart';
import '../../design/controls.dart';
import '../../design/tokens.dart';
import '../../platform/desktop_contracts.dart';
import '../../providers/provider_registry.dart';
import '../collections/collection_repository.dart';
import '../jobs/job_repository.dart';
import '../installations/installations_panel.dart';
import '../installations/managed_content_panel.dart';
import '../installations/managed_transfer_panel.dart';
import '../store/catalog_cache_panel.dart';
import 'workspace_controller.dart';

class WorkspacePanel extends StatefulWidget {
  const WorkspacePanel({
    super.key,
    required this.controller,
    required this.providers,
    required this.jobs,
    required this.diagnostics,
    required this.durable,
    required this.integrations,
    this.initialSettings = false,
    this.initialActivity = false,
    this.overlays,
  });
  final WorkspaceController controller;
  final ProviderRegistry providers;
  final List<JobRecord> jobs;
  final Diagnostics diagnostics;
  final bool durable;
  final List<OptionalIntegration> integrations;
  final bool initialSettings;
  final bool initialActivity;
  final OverlayCoordinator? overlays;
  @override
  State<WorkspacePanel> createState() => _WorkspacePanelState();
}

class _WorkspacePanelState extends State<WorkspacePanel> {
  final input = TextEditingController();
  late String section = widget.initialActivity
      ? 'Activity'
      : widget.initialSettings
      ? 'Settings'
      : 'Saved';
  int savedPage = 0;
  int activityPage = 0;
  int? activityGeneration;
  bool showDiagnostics = false;
  @override
  void dispose() {
    input.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Dialog(
    insetPadding: const EdgeInsets.all(12),
    shape: RoundedRectangleBorder(
      borderRadius: BorderRadius.circular(RyhzeTokens.radius),
      side: const BorderSide(color: RyhzeTokens.border),
    ),
    child: ConstrainedBox(
      constraints: const BoxConstraints(maxWidth: 860),
      child: ListenableBuilder(
        listenable: widget.controller,
        builder: (context, _) {
          final controller = widget.controller;
          if (activityGeneration != controller.session.generation) {
            activityGeneration = controller.session.generation;
            activityPage = 0;
          }
          final events = controller.events;
          final activityPages = ((events.length + 49) ~/ 50).clamp(1, 4);
          activityPage = activityPage.clamp(0, activityPages - 1);
          final saved = controller.saved;
          final pages = ((saved.length + 49) ~/ 50).clamp(1, 100);
          final page = savedPage.clamp(0, pages - 1);
          return SingleChildScrollView(
            padding: EdgeInsets.all(
              MediaQuery.sizeOf(context).width <= 480 ? 16 : 24,
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Wrap(
                  spacing: 16,
                  runSpacing: 12,
                  crossAxisAlignment: WrapCrossAlignment.center,
                  children: [
                    RyhzeControl(
                      label: 'Close workspace',
                      autofocus: true,
                      onPressed: () => Navigator.pop(context),
                      child: const Text('Back'),
                    ),
                    Text('Your Ryhze', style: heading(30)),
                  ],
                ),
                const SizedBox(height: 20),
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    for (final name in [
                      'Saved',
                      'Connections',
                      'Activity',
                      if (controller.installations?.available == true ||
                          (controller.managedContent?.records.isNotEmpty ??
                              false) ||
                          (controller.transfers?.records.isNotEmpty ?? false))
                        'Installations',
                      'Settings',
                      'Support',
                    ])
                      RyhzeControl(
                        label: name,
                        selected: name == section,
                        onPressed: () => setState(() => section = name),
                        child: Text(
                          name,
                          style: TextStyle(
                            color: name == section
                                ? RyhzeTokens.accent
                                : RyhzeTokens.text,
                            fontWeight: name == section
                                ? FontWeight.w600
                                : FontWeight.w400,
                          ),
                        ),
                      ),
                  ],
                ),
                const SizedBox(height: 24),
                if (!controller.writable)
                  const Text(
                    'Local data needs recovery. Browsing remains available; saving is disabled.',
                  ),
                if (controller.failure != null)
                  Semantics(
                    liveRegion: true,
                    child: Text(controller.failure!.message),
                  ),
                if (section == 'Saved') ...[
                  Text('Saved collection', style: heading(26)),
                  const SizedBox(height: 12),
                  const Text(
                    'Manually added and favourited titles. These records do not verify ownership and are separate from your Library.',
                  ),
                  const SizedBox(height: 8),
                  Text(
                    widget.durable
                        ? 'Saved on this device for the current account scope.'
                        : 'Session only: local persistence is unavailable in this preview.',
                  ),
                  const SizedBox(height: 16),
                  TextField(
                    controller: input,
                    maxLength: 512,
                    decoration: const InputDecoration(
                      labelText: 'Title to save',
                    ),
                    onSubmitted: (_) => add(),
                  ),
                  RyhzeControl(
                    label: 'Save title',
                    onPressed: controller.busy || !controller.writable
                        ? null
                        : add,
                    child: const Text('Save title'),
                  ),
                  const SizedBox(height: 16),
                  if (saved.isEmpty) const Text('No saved titles yet.'),
                  for (final title in saved.skip(page * 50).take(50))
                    Padding(
                      padding: const EdgeInsets.symmetric(vertical: 8),
                      child: Wrap(
                        spacing: 16,
                        runSpacing: 8,
                        crossAxisAlignment: WrapCrossAlignment.center,
                        children: [
                          Text(title.title),
                          RyhzeControl(
                            label:
                                'Remove ${title.title} from saved collection',
                            onPressed: controller.busy || !controller.writable
                                ? null
                                : () => controller.remove(title),
                            child: const Text('Remove'),
                          ),
                        ],
                      ),
                    ),
                ],
                if (section == 'Saved' && pages > 1)
                  Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    children: [
                      RyhzeControl(
                        label: 'Previous saved page',
                        onPressed: page == 0
                            ? null
                            : () => setState(() => savedPage = page - 1),
                        child: const Text('Previous'),
                      ),
                      Text('Page ${page + 1} of $pages'),
                      RyhzeControl(
                        label: 'Next saved page',
                        onPressed: page + 1 >= pages
                            ? null
                            : () => setState(() => savedPage = page + 1),
                        child: const Text('Next'),
                      ),
                    ],
                  ),
                if (section == 'Connections') ...[
                  Text('Connected accounts', style: heading(26)),
                  const SizedBox(height: 12),
                  const Text(
                    'Browsing locally · RYHZE sign-in is not configured. No provider account is connected.',
                  ),
                  for (final provider in widget.providers.providers) ...[
                    const SizedBox(height: 20),
                    Text(provider.name, style: heading(21)),
                    const SizedBox(height: 8),
                    Text(
                      provider
                          .capability(
                            provider.id == 'films'
                                ? ProviderCapability.playback
                                : ProviderCapability.ownedLibrary,
                          )
                          .explanation,
                    ),
                    const SizedBox(height: 8),
                    RyhzeControl(
                      label: 'Connect ${provider.name} unavailable',
                      onPressed: null,
                      child: const Text('Connection unavailable'),
                    ),
                  ],
                ],
                if (section == 'Activity') ...[
                  Text('Downloads and activity', style: heading(26)),
                  const SizedBox(height: 12),
                  const Text(
                    'No authorized download or installation provider is configured. Jobs do not continue after the app quits.',
                  ),
                  const SizedBox(height: 16),
                  if (controller.jobRecords.isEmpty)
                    const Text('No jobs for this account.'),
                  for (final job in controller.jobRecords)
                    Padding(
                      padding: const EdgeInsets.only(bottom: 12),
                      child: Text(
                        '${job.kind} · ${job.state.name}${job.failure == null ? '' : ' · ${AppFailure(job.failure!).message}'}',
                      ),
                    ),
                ],
                if (section == 'Activity' && events.isNotEmpty) ...[
                  const SizedBox(height: 16),
                  Text('Recent activity', style: heading(21)),
                  for (final event in events.skip(activityPage * 50).take(50))
                    Padding(
                      key: ValueKey('activity-${event.id}'),
                      padding: const EdgeInsets.symmetric(vertical: 8),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(event.summary),
                          if (event.route != null &&
                              event.route!.destination !=
                                  AppDestination.activity)
                            RyhzeControl(
                              label: 'Open activity item',
                              onPressed: () =>
                                  Navigator.pop(context, event.route),
                              child: const Text('Open item'),
                            ),
                          Text(
                            '${event.occurredAt.toLocal()} · ${event.count} occurrence(s)',
                          ),
                          if (!event.acknowledged)
                            RyhzeControl(
                              label: 'Acknowledge activity',
                              onPressed: controller.busy || !controller.writable
                                  ? null
                                  : () => controller.acknowledge(event),
                              child: const Text('Acknowledge'),
                            ),
                        ],
                      ),
                    ),
                ],
                if (section == 'Activity' && activityPages > 1)
                  Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    children: [
                      RyhzeControl(
                        label: 'Previous activity page',
                        onPressed: activityPage == 0
                            ? null
                            : () => setState(() => activityPage--),
                        child: const Text('Previous'),
                      ),
                      Text('Page ${activityPage + 1} of $activityPages'),
                      RyhzeControl(
                        label: 'Next activity page',
                        onPressed: activityPage + 1 >= activityPages
                            ? null
                            : () => setState(() => activityPage++),
                        child: const Text('Next'),
                      ),
                    ],
                  ),
                if (section == 'Installations' &&
                    controller.installations?.available == true)
                  InstallationPanel(controller: controller.installations!),
                if (section == 'Installations' &&
                    controller.managedContent != null)
                  ManagedContentPanel(
                    controller: controller.managedContent!,
                    overlays: widget.overlays,
                    parentOwnsOverlay: widget.overlays != null,
                  ),
                if ((section == 'Installations' || section == 'Activity') &&
                    controller.transfers != null)
                  ManagedTransferPanel(controller: controller.transfers!),
                if (section == 'Settings') ...[
                  Text('Settings', style: heading(26)),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Reduce motion'),
                    value: controller.settings.reduceMotion,
                    onChanged: controller.busy || !controller.writable
                        ? null
                        : (value) => controller.updateSettings(
                            Preferences(
                              reduceMotion: value,
                              privatePreviews:
                                  controller.settings.privatePreviews,
                              notifications: controller.settings.notifications,
                            ),
                          ),
                  ),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Hide sensitive notification previews'),
                    subtitle: Text(
                      controller.notificationsAvailable?.call() == true
                          ? 'Use a general summary on the desktop and lock screen.'
                          : 'Privacy preference. Native notifications are not enabled.',
                    ),
                    value: controller.settings.privatePreviews,
                    onChanged: controller.busy || !controller.writable
                        ? null
                        : (value) => controller.updateSettings(
                            Preferences(
                              reduceMotion: controller.settings.reduceMotion,
                              privatePreviews: value,
                              notifications: controller.settings.notifications,
                            ),
                          ),
                  ),
                  if (controller.notificationsAvailable?.call() == true)
                    SwitchListTile(
                      contentPadding: EdgeInsets.zero,
                      title: const Text('Desktop activity notifications'),
                      subtitle: Text(
                        controller.notificationsAvailable?.call() == true
                            ? 'Notify for new activity with an available destination. Windows notification settings also apply.'
                            : 'Unavailable in this build or installation. Activity remains available inside Ryhze.',
                      ),
                      value: controller.settings.notifications,
                      onChanged:
                          controller.busy ||
                              !controller.writable ||
                              controller.notificationsAvailable?.call() != true
                          ? null
                          : (value) => controller.updateSettings(
                              Preferences(
                                reduceMotion: controller.settings.reduceMotion,
                                privatePreviews:
                                    controller.settings.privatePreviews,
                                notifications: value,
                              ),
                            ),
                    ),
                  const Text(
                    'Download locations become available with an authorized installer. No background service or telemetry upload is enabled.',
                  ),
                ],
                if (section == 'Support') ...[
                  Text('Support and recovery', style: heading(26)),
                  const SizedBox(height: 12),
                  if (controller.artwork != null)
                    ArtworkCachePanel(maintenance: controller.artwork!),
                  if (controller.catalogMaintenance != null &&
                      controller.clearCatalog != null)
                    CatalogCachePanel(
                      maintenance: controller.catalogMaintenance!,
                      clear: controller.clearCatalog!,
                    ),
                  Text(
                    widget.durable
                        ? 'Local storage: durable journal, schema 2.'
                        : 'Local storage: session preview.',
                  ),
                  const SizedBox(height: 12),
                  const Text(
                    'Keep a closed-app copy of user-data before changing binaries. A newer schema is never opened by an incompatible version. Corrupt data is preserved for recovery; it is not reset automatically.',
                  ),
                  const SizedBox(height: 12),
                  for (final integration in widget.integrations)
                    Text(
                      '${integration.name} interface v${integration.contractVersion}: ${integration.reason}',
                    ),
                  const SizedBox(height: 16),
                  RyhzeControl(
                    label: 'Review diagnostic export',
                    onPressed: () =>
                        setState(() => showDiagnostics = !showDiagnostics),
                    child: const Text('Review diagnostic export'),
                  ),
                  if (showDiagnostics) ...[
                    const SizedBox(height: 12),
                    const Text(
                      'Local event codes, timestamps and correlation numbers only. Review and copy this text if you choose to share it.',
                    ),
                    SelectableText(widget.diagnostics.export()),
                  ],
                ],
              ],
            ),
          );
        },
      ),
    ),
  );
  Future<void> add() async {
    await widget.controller.addManual(input.text);
    if (mounted && widget.controller.failure == null) input.clear();
  }
}
