import 'package:flutter/material.dart';

import '../../../design/tokens.dart';
import '../../../design/artwork.dart';
import '../model/library_entry.dart';

String libraryEntryKey(LibraryIdentity identity) => identity.toString();

String installationLabel(LibraryEntry entry) {
  final status = switch (entry.installation.state) {
    LibraryInstallationState.installed => 'Installed',
    LibraryInstallationState.notInstalled => 'Not installed',
    LibraryInstallationState.unknown => 'Installation unknown',
    LibraryInstallationState.unsupported => 'Installation unsupported',
    LibraryInstallationState.failed => 'Installation check failed',
  };
  return entry.isDemo ? 'Simulated · $status' : status;
}

String _statusText(LibraryEntry entry) =>
    '${entry.isDemo ? 'Demo game · ' : ''}${installationLabel(entry)}';

const _rowTitleStyle = TextStyle(
  fontFamily: 'SpaceGrotesk',
  fontSize: 14,
  height: 1.3,
  color: RyhzeTokens.text,
);
const _statusStyle = TextStyle(
  color: RyhzeTokens.subtle,
  fontSize: 11,
  height: 1.4,
);
const _rowPadding = 16.0;
const _borderWidth = 1.0;
const _rowIconWidth = 20.0;
const _rowIconGap = 12.0;

/// Compute only layout metrics when restoring an entry after a responsive
/// transition. This never mounts offscreen rows or loads their artwork.
double libraryRowExtent(
  BuildContext context,
  LibraryEntry entry,
  double width,
) {
  final horizontalInset = (_rowPadding + _borderWidth) * 2;
  final textWidth = (width - horizontalInset - _rowIconWidth - _rowIconGap)
      .clamp(1.0, double.infinity);
  double height(String value, TextStyle style) {
    final inherited = DefaultTextStyle.of(context);
    final effective = inherited.style
        .merge(style)
        .merge(
          TextStyle(
            fontWeight: MediaQuery.boldTextOf(context) ? FontWeight.bold : null,
            height: MediaQuery.maybeLineHeightScaleFactorOverrideOf(context),
            letterSpacing: MediaQuery.maybeLetterSpacingOverrideOf(context),
            wordSpacing: MediaQuery.maybeWordSpacingOverrideOf(context),
          ),
        );
    final painter = TextPainter(
      text: TextSpan(text: value, style: effective),
      textDirection: Directionality.of(context),
      textScaler: MediaQuery.textScalerOf(context),
      locale: Localizations.maybeLocaleOf(context),
      textAlign: inherited.textAlign ?? TextAlign.start,
      textWidthBasis: inherited.textWidthBasis,
      textHeightBehavior:
          inherited.textHeightBehavior ??
          DefaultTextHeightBehavior.maybeOf(context),
      maxLines: inherited.maxLines,
    )..layout(maxWidth: textWidth);
    final result = painter.height;
    painter.dispose();
    return result;
  }

  final content =
      height(entry.title, _rowTitleStyle) +
      6 +
      height(_statusText(entry), _statusStyle);
  return (horizontalInset + content).clamp(64.0, double.infinity);
}

/// Lazy list/grid children own their focus nodes. The collection remains mounted
/// behind details, so Back can restore the exact originating control and offset.
class LibraryEntryTile extends StatefulWidget {
  const LibraryEntryTile({
    super.key,
    required this.entry,
    required this.onSelected,
    this.onFocusNode,
    this.card = false,
    this.selected = false,
    this.favourite = false,
  });

  final LibraryEntry entry;
  final ValueChanged<FocusNode> onSelected;
  final void Function(FocusNode node, bool attached)? onFocusNode;
  final bool card, selected, favourite;

  @override
  State<LibraryEntryTile> createState() => _LibraryEntryTileState();
}

class _LibraryEntryTileState extends State<LibraryEntryTile> {
  late final FocusNode _focus = FocusNode(
    debugLabel: 'Library ${libraryEntryKey(widget.entry.identity)}',
  );
  bool _focused = false;
  bool _hovered = false;

  @override
  void initState() {
    super.initState();
    widget.onFocusNode?.call(_focus, true);
  }

  @override
  void dispose() {
    widget.onFocusNode?.call(_focus, false);
    _focus.dispose();
    super.dispose();
  }

  void _select() {
    _focus.requestFocus();
    widget.onSelected(_focus);
  }

  @override
  Widget build(BuildContext context) {
    final entry = widget.entry;
    final title = Text(
      entry.title,
      maxLines: widget.card ? 3 : null,
      overflow: widget.card ? TextOverflow.ellipsis : null,
      style: _rowTitleStyle.copyWith(fontSize: widget.card ? 19 : 14),
    );
    return Semantics(
      button: true,
      onTap: _select,
      selected: widget.selected,
      label:
          '${entry.isDemo ? 'Demo game. ' : ''}${entry.title}. '
          '${installationLabel(entry)}${widget.favourite ? '. Favourite' : ''}',
      excludeSemantics: true,
      child: FocusableActionDetector(
        focusNode: _focus,
        mouseCursor: SystemMouseCursors.click,
        onShowFocusHighlight: (value) => setState(() => _focused = value),
        onShowHoverHighlight: (value) => setState(() => _hovered = value),
        actions: <Type, Action<Intent>>{
          ActivateIntent: CallbackAction<ActivateIntent>(
            onInvoke: (_) {
              _select();
              return null;
            },
          ),
        },
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTap: _select,
          child: AnimatedContainer(
            duration: RyhzeTokens.motion(context, RyhzeTokens.surfaceMotion),
            curve: RyhzeTokens.ease,
            constraints: const BoxConstraints(minHeight: 64),
            padding: EdgeInsets.all(widget.card ? 12 : _rowPadding),
            decoration: BoxDecoration(
              color: _hovered || widget.selected
                  ? RyhzeTokens.raised
                  : RyhzeTokens.surface,
              borderRadius: BorderRadius.circular(RyhzeTokens.itemRadius),
              border: Border.all(
                color: _focused ? RyhzeTokens.accent : RyhzeTokens.border,
                width: _borderWidth,
              ),
              boxShadow: [
                if (_focused)
                  const BoxShadow(color: RyhzeTokens.accent, spreadRadius: 1),
              ],
            ),
            child: widget.card
                ? Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Expanded(child: LibraryArtwork()),
                      const SizedBox(height: 12),
                      title,
                      const SizedBox(height: 8),
                      _Status(entry: entry),
                      const SizedBox(height: 4),
                    ],
                  )
                : Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Padding(
                        padding: const EdgeInsets.only(top: 2),
                        child: Icon(
                          widget.favourite
                              ? Icons.star_rounded
                              : Icons.sports_esports_outlined,
                          size: _rowIconWidth,
                          color: RyhzeTokens.accent,
                        ),
                      ),
                      const SizedBox(width: _rowIconGap),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            title,
                            const SizedBox(height: 6),
                            _Status(entry: entry),
                          ],
                        ),
                      ),
                    ],
                  ),
          ),
        ),
      ),
    );
  }
}

class _Status extends StatelessWidget {
  const _Status({required this.entry});
  final LibraryEntry entry;

  @override
  Widget build(BuildContext context) =>
      Text(_statusText(entry), style: _statusStyle);
}

/// The approved branded placeholder is deliberately not a game screenshot.
class LibraryArtwork extends StatelessWidget {
  const LibraryArtwork({super.key});

  @override
  Widget build(BuildContext context) => ClipRRect(
    borderRadius: BorderRadius.circular(RyhzeTokens.itemRadius),
    child: const ProjectArtwork(),
  );
}
