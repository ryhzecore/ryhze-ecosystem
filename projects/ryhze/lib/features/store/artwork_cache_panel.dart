import 'package:flutter/material.dart';

import '../../core/app_failure.dart';
import '../../design/controls.dart';
import '../../design/tokens.dart';
import 'artwork_maintenance.dart';

class ArtworkCachePanel extends StatefulWidget {
  const ArtworkCachePanel({super.key, required this.maintenance});
  final ArtworkMaintenance maintenance;
  @override
  State<ArtworkCachePanel> createState() => _ArtworkCachePanelState();
}

class _ArtworkCachePanelState extends State<ArtworkCachePanel> {
  late Future<PublicArtworkHealth> _health = widget.maintenance.health();
  bool _busy = false;
  String? _message;
  @override
  void didUpdateWidget(covariant ArtworkCachePanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.maintenance, widget.maintenance)) {
      _health = widget.maintenance.health();
      _message = null;
    }
  }

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 24),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text('Public artwork cache', style: heading(21)),
        const SizedBox(height: 12),
        FutureBuilder<PublicArtworkHealth>(
          future: _health,
          builder: (context, snapshot) {
            final health = snapshot.data;
            if (snapshot.hasError) {
              return const Text('Artwork cache is unavailable.');
            }
            if (health == null) return const Text('Checking artwork cache…');
            final disk = health.disk;
            return Text(
              '${health.memoryEntries} images in memory '
              '(${_size(health.memoryBytes)}). '
              '${health.diskFailure != null
                  ? 'Artwork storage is unavailable.'
                  : disk == null
                  ? 'Images are kept for this session only.'
                  : '${disk.entries} saved images (${_size(disk.bytes)}). '
                        '${disk.preserved} unrecognized files preserved.'}',
            );
          },
        ),
        const SizedBox(height: 12),
        const Text(
          'Artwork refreshes after one hour. During a connection failure, '
          'previously validated images may be shown for up to seven days with an '
          'offline label. Clearing removes public artwork only; saved titles, '
          'catalogue metadata, installed content and preferences are separate. '
          'Reopen a title to fetch its artwork again.',
        ),
        const SizedBox(height: 12),
        Wrap(
          spacing: 12,
          runSpacing: 12,
          children: [
            RyhzeControl(
              label: 'Refresh artwork cache health',
              onPressed: _busy
                  ? null
                  : () => setState(() {
                      _health = widget.maintenance.health();
                    }),
              child: const Text('Refresh cache health'),
            ),
            RyhzeControl(
              label: 'Clear artwork cache',
              onPressed: _busy ? null : _clear,
              child: Text(_busy ? 'Clearing…' : 'Clear artwork cache'),
            ),
          ],
        ),
        if (_message != null)
          Semantics(liveRegion: true, child: Text(_message!)),
      ],
    ),
  );
  static String _size(int bytes) =>
      '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MiB';
  Future<void> _clear() async {
    final owner = widget.maintenance;
    setState(() {
      _busy = true;
      _message = null;
    });
    try {
      final result = await owner.clear();
      if (mounted && identical(owner, widget.maintenance)) {
        setState(
          () => _message =
              'Artwork cache cleared. '
              '${result.removed} saved images removed; '
              '${result.preserved} unrecognized files preserved.',
        );
      }
    } catch (error) {
      if (mounted && identical(owner, widget.maintenance)) {
        setState(
          () => _message = error is AppFailure
              ? error.message
              : 'Artwork cache could not be cleared.',
        );
      }
    } finally {
      if (mounted) {
        setState(() {
          _busy = false;
          _health = widget.maintenance.health();
        });
      }
    }
  }
}
