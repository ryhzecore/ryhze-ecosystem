import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../design/controls.dart';
import '../../design/modal_frame.dart';
import '../../app/navigation_controller.dart';
import 'managed_content_discard_service.dart';
import '../../design/tokens.dart';
import 'managed_content_controller.dart';
import 'managed_installation.dart';

/// Retained local bytes remain separate from installed titles and ownership.
class ManagedContentPanel extends StatelessWidget {
  const ManagedContentPanel({
    super.key,
    required this.controller,
    this.overlays,
    this.parentOwnsOverlay = false,
  });
  final ManagedContentController controller;
  final OverlayCoordinator? overlays;
  final bool parentOwnsOverlay;
  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: controller,
    builder: (context, _) {
      if (controller.records.isEmpty) return const SizedBox.shrink();
      final localizations = MaterialLocalizations.of(context);
      return FocusTraversalGroup(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Retained local content', style: heading(26)),
            const SizedBox(height: 12),
            const Text(
              'These are staged local files, not installed titles. Publisher authenticity and ownership are not verified. Inspection does not install, download or launch anything.',
            ),
            if (controller.activeInCurrentScope) ...[
              const SizedBox(height: 12),
              Semantics(
                liveRegion: true,
                child: Text(
                  controller.removalActivity != null
                      ? (controller.cancellationRequested
                            ? 'Cancelling; waiting for retained-file work to finish.'
                            : controller.removalActivity!)
                      : controller.cancellationRequested
                      ? (controller.resuming
                            ? 'Cancelling local staging; waiting for local work to finish.'
                            : 'Cancelling inspection; waiting for local work to finish.')
                      : (controller.resuming
                            ? 'Checking and resuming local staging…'
                            : 'Inspecting retained content…'),
                ),
              ),
              const SizedBox(height: 8),
              RyhzeControl(
                label: controller.removalActivity != null
                    ? 'Cancel retained-file work'
                    : controller.resuming
                    ? 'Cancel local staging'
                    : 'Cancel retained content inspection',
                onPressed: controller.cancellationRequested
                    ? null
                    : controller.cancel,
                child: Text(
                  controller.removalActivity != null
                      ? 'Cancel'
                      : controller.resuming
                      ? 'Cancel local staging'
                      : 'Cancel inspection',
                ),
              ),
            ],
            if (controller.failure != null) ...[
              const SizedBox(height: 12),
              Semantics(
                liveRegion: true,
                child: Text(controller.failure!.message),
              ),
            ],
            for (final record in controller.visibleRecords)
              Padding(
                key: ValueKey('managed-content-${record.operationId}'),
                padding: const EdgeInsets.symmetric(vertical: 16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(record.intent.packageId, style: heading(20)),
                    Text('Version ${record.intent.version}'),
                    const SizedBox(height: 6),
                    Text(switch (record.state) {
                      ManagedInstallationState.prepared =>
                        'Prepared; copying has not started.',
                      ManagedInstallationState.resumePrepared =>
                        'Resume prepared; retained files are unchanged.',
                      ManagedInstallationState.staging =>
                        'Staging in progress; completion is not verified.',
                      ManagedInstallationState.staged =>
                        'Staged locally; not installed.',
                      ManagedInstallationState.recoveryRequired =>
                        'Recovery required; retained content needs inspection.',
                      ManagedInstallationState.discardPrepared =>
                        'Removal prepared; retained-file work is pending.',
                      ManagedInstallationState.discarding =>
                        'Removing staging copy; completion is not verified.',
                      ManagedInstallationState.discardRecoveryRequired => 'Removal interrupted; inspect retained files before confirming a retry.',
                      ManagedInstallationState.discarded => 'Staging copy removed. Incoming file and history are kept.',
                      ManagedInstallationState.cancelled =>
                        'Cancelled before staging.',
                    }),
                    Text(
                      '${localizations.formatDecimal(record.verifiedBytes)} of ${localizations.formatDecimal(record.intent.expectedSize)} bytes verified at the last inspection.',
                    ),
                    if (record.state == ManagedInstallationState.staged ||
                        record.state ==
                            ManagedInstallationState.recoveryRequired) ...[
                      const SizedBox(height: 12),
                      RyhzeControl(
                        label:
                            'Inspect retained content for ${record.intent.packageId}',
                        onPressed: controller.canInspect(record)
                            ? () => controller.inspect(record.operationId)
                            : null,
                        child: const Text('Inspect retained content'),
                      ),
                    ],
                    if (overlays != null &&
                        controller.canReviewRemoval(record)) ...[
                      const SizedBox(height: 12),
                      RyhzeControl(
                        label: 'Review removal for ${record.intent.packageId}',
                        onPressed: () =>
                            _reviewRemoval(context, record.operationId),
                        child: Text(
                          record.state ==
                                  ManagedInstallationState
                                      .discardRecoveryRequired
                              ? 'Inspect removal recovery'
                              : 'Review removal',
                        ),
                      ),
                    ],
                    if (controller.canResume(record)) ...[
                      const SizedBox(height: 12),
                      const Text(
                        'Resume checks the same incoming file and retained bytes before copying the remainder locally.',
                      ),
                      const SizedBox(height: 8),
                      RyhzeControl(
                        label:
                            'Resume local staging for ${record.intent.packageId}',
                        onPressed: controller.canResume(record)
                            ? () => controller.resumeRecord(record.operationId)
                            : null,
                        child: const Text('Resume local staging'),
                      ),
                    ],
                  ],
                ),
              ),
            if (controller.pageCount > 1)
              Wrap(
                spacing: 12,
                runSpacing: 12,
                crossAxisAlignment: WrapCrossAlignment.center,
                children: [
                  RyhzeControl(
                    label: 'Previous retained content page',
                    onPressed: controller.page == 0
                        ? null
                        : controller.previousPage,
                    child: const Text('Previous'),
                  ),
                  Text(
                    'Page ${controller.page + 1} of ${controller.pageCount}',
                  ),
                  RyhzeControl(
                    label: 'Next retained content page',
                    onPressed: controller.page + 1 == controller.pageCount
                        ? null
                        : controller.nextPage,
                    child: const Text('Next'),
                  ),
                ],
              ),
          ],
        ),
      );
    },
  );
  Future<void> _reviewRemoval(BuildContext context, String id) async {
    final owner = overlays;
    if (owner == null || (!parentOwnsOverlay && owner.isOpen)) return;
    ManagedDiscardConfirmation? approved;
    Future<void> review() async {
      final confirmation = await controller.reviewRemoval(id);
      if (!context.mounted ||
          confirmation == null ||
          !controller.isRemovalCurrent(confirmation)) {
        controller.dismissRemoval();
        return;
      }
      final accepted = await Navigator.of(context).push<bool>(
        RyhzeDialogRoute<bool>(
          enter: RyhzeTokens.motion(context, RyhzeTokens.frameIn),
          exit: RyhzeTokens.motion(context, RyhzeTokens.frameOut),
          child: _RemovalDialog(
            controller: controller,
            confirmation: confirmation,
          ),
        ),
      );
      if (accepted == true && controller.isRemovalCurrent(confirmation)) {
        approved = confirmation;
      } else {
        controller.dismissRemoval();
      }
    }

    if (parentOwnsOverlay) {
      await review();
    } else {
      await owner.run<void>(review);
    }
    if (approved != null && context.mounted) {
      await controller.confirmRemoval(approved!);
    }
  }
}

class _RemovalDialog extends StatefulWidget {
  const _RemovalDialog({required this.controller, required this.confirmation});
  final ManagedContentController controller;
  final ManagedDiscardConfirmation confirmation;
  @override
  State<_RemovalDialog> createState() => _RemovalDialogState();
}

class _RemovalDialogState extends State<_RemovalDialog> {
  bool closing = false;
  final detailsScroll = ScrollController();
  bool detailsFocused = false;

  KeyEventResult _scrollDetails(FocusNode node, KeyEvent event) {
    if (event is! KeyDownEvent && event is! KeyRepeatEvent) {
      return KeyEventResult.ignored;
    }
    if (!detailsScroll.hasClients) {
      return KeyEventResult.ignored;
    }
    final position = detailsScroll.position;
    final key = event.logicalKey;
    final double target;
    if (key == LogicalKeyboardKey.pageDown) {
      target = position.pixels + position.viewportDimension * .8;
    } else if (key == LogicalKeyboardKey.pageUp) {
      target = position.pixels - position.viewportDimension * .8;
    } else if (key == LogicalKeyboardKey.arrowDown) {
      target = position.pixels + 48;
    } else if (key == LogicalKeyboardKey.arrowUp) {
      target = position.pixels - 48;
    } else if (key == LogicalKeyboardKey.home) {
      target = position.minScrollExtent;
    } else if (key == LogicalKeyboardKey.end) {
      target = position.maxScrollExtent;
    } else {
      return KeyEventResult.ignored;
    }
    detailsScroll.jumpTo(
      target.clamp(position.minScrollExtent, position.maxScrollExtent),
    );
    return KeyEventResult.handled;
  }

  @override
  void initState() {
    super.initState();
    widget.controller.addListener(_changed);
  }

  void _changed() {
    if (!mounted ||
        closing ||
        widget.controller.isRemovalCurrent(widget.confirmation)) {
      return;
    }
    closing = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && ModalRoute.of(context)?.isCurrent == true) {
        Navigator.of(context).pop(false);
      }
    });
  }

  @override
  void dispose() {
    detailsScroll.dispose();
    widget.controller.removeListener(_changed);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final confirmation = widget.confirmation;
    return Center(
      child: Dialog(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 520),
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: FocusTraversalGroup(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Flexible(
                    child: Focus(
                      debugLabel: 'Removal details',
                      descendantsAreFocusable: false,
                      onFocusChange: (value) =>
                          setState(() => detailsFocused = value),
                      onKeyEvent: _scrollDetails,
                      child: DecoratedBox(
                        key: const ValueKey('removal-details'),
                        decoration: BoxDecoration(
                          border: Border.all(
                            color: detailsFocused
                                ? RyhzeTokens.accent
                                : Colors.transparent,
                          ),
                        ),
                        child: SingleChildScrollView(
                          controller: detailsScroll,
                          primary: false,
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Semantics(
                                namesRoute: true,
                                child: Text(
                                  'Remove this retained staging copy?',
                                  style: heading(24),
                                ),
                              ),
                              const SizedBox(height: 16),
                              Text(confirmation.packageId, style: heading(20)),
                              Text('Version ${confirmation.version}'),
                              const SizedBox(height: 12),
                              Text(
                                '${MaterialLocalizations.of(context).formatDecimal(confirmation.verifiedBytes)} verified bytes across ${confirmation.fileCount} reviewed staging files.',
                              ),
                              const SizedBox(height: 12),
                              if (confirmation.recovery)
                                Text(
                                  '${MaterialLocalizations.of(context).formatDecimal(confirmation.remainingBytes)} planned bytes across ${confirmation.remainingFileCount} files remain to be removed.',
                                ),
                              const Text(
                                'Only the reviewed staging files will be removed. The incoming file, installed titles and history are kept. This does not uninstall a title or clear your cache.',
                              ),
                            ],
                          ),
                        ),
                      ),
                    ),
                  ),
                  const SizedBox(height: 20),
                  Wrap(
                    spacing: 12,
                    runSpacing: 12,
                    children: [
                      RyhzeControl(
                        label: 'Cancel removal',
                        autofocus: true,
                        onPressed: () => Navigator.of(context).pop(false),
                        child: const Text('Cancel'),
                      ),
                      RyhzeControl(
                        label: 'Remove staging copy',
                        onPressed:
                            widget.controller.isRemovalCurrent(confirmation)
                            ? () => Navigator.of(context).pop(true)
                            : null,
                        child: const Text('Remove staging copy'),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
