import 'package:flutter/material.dart';

import '../../../design/controls.dart';
import '../../../design/tokens.dart';
import '../model/library_entry.dart';
import 'library_entry_tile.dart';

class LibraryDetail extends StatelessWidget {
  const LibraryDetail({
    super.key,
    required this.entry,
    this.durableFavourites = false,
    this.failureMessage,
    this.verificationNotice,
    required this.favourite,
    required this.onFavourite,
    required this.onBack,
    required this.backFocusNode,
    required this.scrollController,
  });

  final String? failureMessage;
  final String? verificationNotice;
  final bool durableFavourites;
  final LibraryEntry entry;
  final bool favourite;
  final VoidCallback onFavourite, onBack;
  final FocusNode backFocusNode;
  final ScrollController scrollController;

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, constraints) => Scrollbar(
      controller: scrollController,
      child: ListView(
        key: const Key('library-detail-scroll'),
        controller: scrollController,
        padding: EdgeInsets.symmetric(
          horizontal: constraints.maxWidth < 600 ? 16 : 24,
          vertical: 24,
        ),
        children: [
          if (verificationNotice != null)
            Padding(
              padding: const EdgeInsets.only(bottom: 16),
              child: Semantics(
                liveRegion: true,
                child: Text(verificationNotice!),
              ),
            ),
          Align(
            alignment: Alignment.centerLeft,
            child: RyhzeControl(
              key: const Key('library-back'),
              focusNode: backFocusNode,
              label: 'Back to Library',
              onPressed: onBack,
              child: const Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(Icons.arrow_back_rounded),
                  SizedBox(width: 8),
                  Text('Back'),
                ],
              ),
            ),
          ),
          const SizedBox(height: 24),
          Eyebrow(
            entry.isDemo ? 'Demo game · Fictional title' : 'Your Library',
          ),
          const SizedBox(height: 12),
          Text(
            entry.title,
            style: heading(constraints.maxWidth < 600 ? 30 : 36),
          ),
          const SizedBox(height: 20),
          const AspectRatio(aspectRatio: 16 / 9, child: LibraryArtwork()),
          const SizedBox(height: 8),
          const Text(
            'Placeholder artwork · Not gameplay',
            style: TextStyle(color: RyhzeTokens.subtle, fontSize: 11),
          ),
          const SizedBox(height: 24),
          Text(
            entry.description,
            style: const TextStyle(
              color: RyhzeTokens.muted,
              height: 1.7,
              fontSize: 14,
            ),
          ),
          if (entry.tags.isNotEmpty) ...[
            const SizedBox(height: 20),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: entry.tags.map(StatusPill.new).toList(),
            ),
          ],
          const SizedBox(height: 24),
          _Fact(
            label: 'Ownership',
            value: switch (entry.ownership.state) {
              LibraryOwnershipState.verifiedOwned => 'Verified owned',
              LibraryOwnershipState.unknown => 'Ownership unknown',
              LibraryOwnershipState.notOwned => 'Not owned',
              LibraryOwnershipState.demonstration =>
                'Demo only · Not ownership evidence',
            },
            reason: entry.ownership.reason,
          ),
          _Fact(
            label: 'Installation',
            value: installationLabel(entry),
            reason: entry.installation.reason,
          ),
          _Fact(
            label: 'Launch support',
            value: _actionLabel(entry.launchSupport),
            reason: entry.launchSupport.reason,
          ),
          _Fact(
            label: 'Installation support',
            value: _actionLabel(entry.installSupport),
            reason: entry.installSupport.reason,
          ),
          const SizedBox(height: 8),
          RyhzeControl(
            key: const Key('library-launch'),
            label: entry.isDemo
                ? 'Demo only. Play unavailable'
                : 'Play unavailable',
            onPressed: null,
            child: Text(
              entry.isDemo
                  ? 'Demo only · Play unavailable'
                  : 'Play unavailable',
            ),
          ),
          const SizedBox(height: 12),
          RyhzeControl(
            key: const Key('library-install'),
            label: entry.isDemo
                ? 'Demo only. Install unavailable'
                : 'Install unavailable',
            onPressed: null,
            child: Text(
              entry.isDemo
                  ? 'Demo only · Install unavailable'
                  : 'Install unavailable',
            ),
          ),
          const SizedBox(height: 12),
          if (failureMessage != null)
            Semantics(liveRegion: true, child: Text(failureMessage!)),
          RyhzeControl(
            key: const Key('library-favourite'),
            label: favourite
                ? 'Remove Library favourite'
                : 'Add Library favourite',
            selected: favourite,
            onPressed: onFavourite,
            child: Text(favourite ? 'Remove favourite' : 'Add favourite'),
          ),
          const SizedBox(height: 12),
          Text(
            durableFavourites
                ? 'Library favourites are saved for this account on this device. A favourite does not establish ownership or installation.'
                : 'Library favourites are kept only for this session. '
                      'A favourite does not establish ownership or installation.',
            style: TextStyle(
              color: RyhzeTokens.subtle,
              fontSize: 11,
              height: 1.6,
            ),
          ),
        ],
      ),
    ),
  );

  String _actionLabel(LibraryActionSupport support) => switch (support.state) {
    LibraryActionState.unavailable => 'Unavailable',
    LibraryActionState.unsupported => 'Unsupported',
    LibraryActionState.failed => 'Check failed',
  };
}

class _Fact extends StatelessWidget {
  const _Fact({required this.label, required this.value, required this.reason});
  final String label, value, reason;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.only(bottom: 24),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          label,
          style: const TextStyle(color: RyhzeTokens.subtle, fontSize: 11),
        ),
        const SizedBox(height: 8),
        Text(
          value,
          style: const TextStyle(
            color: RyhzeTokens.text,
            fontSize: 15,
            height: 1.4,
          ),
        ),
        const SizedBox(height: 6),
        Text(
          reason,
          style: const TextStyle(
            color: RyhzeTokens.muted,
            fontSize: 13,
            height: 1.6,
          ),
        ),
      ],
    ),
  );
}
