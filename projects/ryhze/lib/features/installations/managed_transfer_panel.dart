import 'package:flutter/material.dart';

import '../../design/controls.dart';
import '../../design/tokens.dart';
import 'managed_transfer_controller.dart';
import 'managed_transfer_record.dart';

class ManagedTransferPanel extends StatelessWidget {
  const ManagedTransferPanel({super.key, required this.controller});
  final ManagedTransferController controller;
  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: controller,
    builder: (context, _) {
      if (controller.records.isEmpty) return const SizedBox.shrink();
      final discardable = controller.discardableIds;
      final format = MaterialLocalizations.of(context).formatDecimal;
      return FocusTraversalGroup(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Downloads', style: heading(26)),
            const SizedBox(height: 12),
            const Text(
              'Downloaded files are kept separate from installed games.',
            ),
            if (controller.activeInCurrentScope) ...[
              const SizedBox(height: 12),
              Semantics(
                liveRegion: true,
                child: Text(
                  controller.cancellationRequested
                      ? 'Cancelling; waiting for file work to finish.'
                      : '${format(controller.service.receivedBytes)} bytes received.',
                ),
              ),
              RyhzeControl(
                label: 'Cancel download work',
                onPressed: controller.cancellationRequested
                    ? null
                    : controller.cancel,
                child: const Text('Cancel'),
              ),
            ],
            if (controller.failure != null)
              Semantics(
                liveRegion: true,
                child: Text(controller.failure!.message),
              ),
            for (final record in controller.visibleRecords)
              Padding(
                key: ValueKey('download-${record.operationId}'),
                padding: const EdgeInsets.symmetric(vertical: 16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(record.intent.packageId, style: heading(20)),
                    Text(switch (record.state) {
                      ManagedTransferState.prepared =>
                        'Prepared; download has not started.',
                      ManagedTransferState.receiving =>
                        'Downloading; completion is not verified.',
                      ManagedTransferState.recoveryRequired => 'Download interrupted. Inspect retained files before retrying.',
                      ManagedTransferState.downloaded =>
                        'Downloaded and verified; not installed.',
                      ManagedTransferState.stagingAdmitted =>
                        'Sent to local staging; not installed.',
                      ManagedTransferState.discarding =>
                        'Removing downloaded files.',
                      ManagedTransferState.discarded =>
                        'Downloaded files removed. History is retained.',
                    }),
                    Text(
                      '${format(record.request.observedBytes)} of ${format(record.intent.expectedSize)} bytes at the last inspection.',
                    ),
                    const SizedBox(height: 8),
                    if (controller.removalId == record.operationId) ...[
                      const Text(
                        'Remove these downloaded files? Your installed games and other files will be kept.',
                      ),
                      Wrap(
                        spacing: 8,
                        runSpacing: 8,
                        children: [
                          RyhzeControl(
                            label:
                                'Confirm removal of ${record.intent.packageId}',
                            onPressed: controller.busy
                                ? null
                                : controller.confirmRemoval,
                            child: const Text('Remove downloaded files'),
                          ),
                          RyhzeControl(
                            label: 'Keep downloaded files',
                            onPressed: controller.busy
                                ? null
                                : controller.dismissRemoval,
                            child: const Text('Keep files'),
                          ),
                        ],
                      ),
                    ] else if (record.state != ManagedTransferState.discarded)
                      Wrap(
                        spacing: 8,
                        runSpacing: 8,
                        children: [
                          RyhzeControl(
                            label:
                                'Inspect download ${record.intent.packageId}',
                            onPressed: controller.busy
                                ? null
                                : () => controller.inspect(record.operationId),
                            child: const Text('Inspect'),
                          ),
                          if (record.state ==
                              ManagedTransferState.recoveryRequired)
                            RyhzeControl(
                              label:
                                  'Retry download ${record.intent.packageId}',
                              onPressed:
                                  controller.busy ||
                                      !controller.service.available ||
                                      record.previous != null
                                  ? null
                                  : () => controller.retry(record.operationId),
                              child: const Text('Retry download'),
                            ),
                          if (record.state == ManagedTransferState.downloaded)
                            RyhzeControl(
                              label:
                                  'Stage download ${record.intent.packageId}',
                              onPressed:
                                  controller.busy ||
                                      !controller.service.available ||
                                      controller.staging == null
                                  ? null
                                  : () => controller.stage(record.operationId),
                              child: const Text('Stage files'),
                            ),
                          RyhzeControl(
                            label:
                                'Review removal of download ${record.intent.packageId}',
                            onPressed:
                                controller.busy ||
                                    !discardable.contains(record.operationId)
                                ? null
                                : () => controller.reviewRemoval(
                                    record.operationId,
                                  ),
                            child: const Text('Review removal'),
                          ),
                        ],
                      ),
                    if (record.state == ManagedTransferState.recoveryRequired &&
                        record.previous != null)
                      const Text(
                        'Two download attempts are retained. Review removal before starting a fresh download.',
                      ),
                    if (record.state == ManagedTransferState.recoveryRequired)
                      const Text(
                        'If retained files are missing, review removal before starting a fresh download.',
                      ),
                  ],
                ),
              ),
            if (controller.pageCount > 1)
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  RyhzeControl(
                    label: 'Previous downloads page',
                    onPressed: controller.page == 0
                        ? null
                        : () => controller.changePage(-1),
                    child: const Text('Previous'),
                  ),
                  Text(
                    'Page ${controller.page + 1} of ${controller.pageCount}',
                  ),
                  RyhzeControl(
                    label: 'Next downloads page',
                    onPressed: controller.page + 1 >= controller.pageCount
                        ? null
                        : () => controller.changePage(1),
                    child: const Text('Next'),
                  ),
                ],
              ),
          ],
        ),
      );
    },
  );
}
