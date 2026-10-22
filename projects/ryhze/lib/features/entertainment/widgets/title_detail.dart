import 'package:flutter/material.dart';

import '../../../design/controls.dart';
import '../../../design/tokens.dart';
import '../model/catalog.dart';
import '../../../design/artwork_presentation.dart';

class TitleDetail extends StatelessWidget {
  const TitleDetail({
    super.key,
    required this.favourite,
    this.title = Catalog.larcenous,
    this.category = EntertainmentCategory.games,
    this.persistent = false,
    this.metadataNotice,
    required this.onFavourite,
  });
  final bool persistent;
  final EntertainmentTitle title;
  final EntertainmentCategory category;
  final String? metadataNotice;
  final ValueNotifier<bool> favourite;
  final VoidCallback onFavourite;
  @override
  Widget build(BuildContext context) => ArtworkPresentation(
    reference: title.artwork,
    fallbackCaption: category == EntertainmentCategory.films
        ? 'Project preview · Not film footage'
        : title.id == Catalog.larcenous.id
        ? 'Project preview · Not gameplay'
        : 'Placeholder artwork',
    builder: (context, image, caption, retry) =>
        _build(context, image, caption, retry),
  );

  Widget _build(
    BuildContext context,
    Widget image,
    String caption,
    VoidCallback? retry,
  ) {
    final width = MediaQuery.sizeOf(context).width;
    final compact = width <= 600;
    final horizontal = compact ? 20.0 : 36.0;
    return Dialog(
      insetPadding: EdgeInsets.all(compact ? 12 : 24),
      backgroundColor: RyhzeTokens.surface,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(RyhzeTokens.radius),
        side: const BorderSide(color: RyhzeTokens.border),
      ),
      clipBehavior: Clip.antiAlias,
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 1080),
        child: Column(
          children: [
            _header(context, compact),
            const Divider(height: 1),
            Expanded(
              child: Scrollbar(
                child: SingleChildScrollView(
                  key: const Key('detail-scroll'),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Padding(
                        key: const Key('detail-intro'),
                        padding: EdgeInsets.fromLTRB(
                          horizontal,
                          compact ? 24 : 30,
                          horizontal,
                          24,
                        ),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Eyebrow(title.status),
                            SizedBox(height: width <= 760 ? 19 : 25),
                            Text(
                              title.title,
                              style: heading((width * .044).clamp(32, 56))
                                  .copyWith(height: 1.1),
                            ),
                            if (title.tags.isNotEmpty) ...[
                              const SizedBox(height: 20),
                              Wrap(
                                spacing: 8,
                                runSpacing: 8,
                                children: [
                                  for (final tag in title.tags) _Tag(tag),
                                ],
                              ),
                            ],
                          ],
                        ),
                      ),
                      Padding(
                        padding: EdgeInsets.symmetric(horizontal: horizontal),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          children: [
                            ClipRRect(
                              borderRadius: BorderRadius.circular(
                                RyhzeTokens.radius,
                              ),
                              child: AspectRatio(
                                key: const Key('detail-art-16-9'),
                                aspectRatio: 16 / 9,
                                child: image,
                              ),
                            ),
                            const SizedBox(height: 12),
                            Text(
                              caption,
                              style: const TextStyle(
                                color: RyhzeTokens.subtle,
                                fontSize: 10,
                                height: 1.65,
                              ),
                            ),
                            if (retry != null) ...[
                              const SizedBox(height: 12),
                              Align(
                                alignment: AlignmentDirectional.centerStart,
                                child: RyhzeControl(
                                  key: const Key('reload-artwork'),
                                  label: 'Reload artwork',
                                  onPressed: retry,
                                  child: const Text('Reload artwork'),
                                ),
                              ),
                            ],
                          ],
                        ),
                      ),
                      Padding(
                        key: const Key('detail-copy'),
                        padding: EdgeInsets.symmetric(
                          horizontal: horizontal,
                          vertical: compact ? 24 : 36,
                        ),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            if (metadataNotice != null) ...[
                              Semantics(
                                liveRegion: true,
                                child: Text(
                                  metadataNotice!,
                                  key: const Key('detail-metadata-notice'),
                                  style: const TextStyle(
                                    color: RyhzeTokens.muted,
                                    fontSize: 12,
                                    height: 1.85,
                                  ),
                                ),
                              ),
                              const SizedBox(height: 16),
                            ],
                            ConstrainedBox(
                              constraints: const BoxConstraints(maxWidth: 760),
                              child: Text(
                                title.description,
                                style: const TextStyle(
                                  color: RyhzeTokens.muted,
                                  height: 1.85,
                                  fontSize: 14,
                                ),
                              ),
                            ),
                            if (title.id == Catalog.larcenous.id ||
                                title.id == Catalog.tommyRiley.id) ...[
                              const SizedBox(height: 24),
                              Container(
                                key: const Key('detail-facts'),
                                width: double.infinity,
                                padding: const EdgeInsets.only(top: 24),
                                decoration: const BoxDecoration(
                                  border: Border(
                                    top: BorderSide(color: RyhzeTokens.border),
                                  ),
                                ),
                                child: title.id == Catalog.tommyRiley.id
                                    ? const _Fact('Release', 'To be announced')
                                    : compact
                                    ? const Column(
                                        crossAxisAlignment:
                                            CrossAxisAlignment.start,
                                        children: [
                                          _Fact('Setting', 'San Coronado'),
                                          SizedBox(height: 18),
                                          _Fact('Release', 'To be announced'),
                                        ],
                                      )
                                    : const Row(
                                        crossAxisAlignment:
                                            CrossAxisAlignment.start,
                                        children: [
                                          Expanded(
                                            child: _Fact(
                                              'Setting',
                                              'San Coronado',
                                            ),
                                          ),
                                          SizedBox(width: 24),
                                          Expanded(
                                            child: _Fact(
                                              'Release',
                                              'To be announced',
                                            ),
                                          ),
                                        ],
                                      ),
                              ),
                            ],
                            const SizedBox(height: 32),
                            Text(
                              title.availability,
                              style: const TextStyle(
                                color: RyhzeTokens.subtle,
                                fontSize: 12,
                                height: 1.85,
                              ),
                            ),
                            const SizedBox(height: 8),
                            Text(
                              persistent
                                  ? 'Saved collection · Does not establish ownership.'
                                  : 'Favourites are kept only for this session.',
                              style: const TextStyle(
                                color: RyhzeTokens.subtle,
                                fontSize: 11,
                                height: 1.65,
                              ),
                            ),
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _header(BuildContext context, bool compact) => Padding(
    padding: EdgeInsets.symmetric(
      horizontal: compact ? 16 : 36,
      vertical: compact ? 16 : 22,
    ),
    child: ValueListenableBuilder<bool>(
      valueListenable: favourite,
      builder: (context, saved, _) => LayoutBuilder(
        builder: (context, constraints) {
          double measure(String text, TextStyle style) {
            final painter = TextPainter(
              text: TextSpan(text: text, style: style),
              textDirection: Directionality.of(context),
              textScaler: MediaQuery.textScalerOf(context),
            )..layout();
            final width = painter.width;
            painter.dispose();
            return width;
          }

          const controlStyle = TextStyle(
            fontFamily: 'Inter',
            fontSize: 14,
            fontWeight: FontWeight.w500,
            height: 1.4,
          );
          const brandStyle = TextStyle(
            fontFamily: 'Inter',
            color: RyhzeTokens.subtle,
            fontSize: 10,
            letterSpacing: .8,
            height: 1.65,
          );
          final label = saved ? 'Favourited' : 'Favourites';
          final backWidth = measure('Back', controlStyle) + 82;
          // Both labels share one responsive boundary, so saving cannot move
          // the focused control between header layouts.
          final savedWidth = measure('Favourited', controlStyle);
          final unsavedWidth = measure('Favourites', controlStyle);
          final saveWidth =
              (savedWidth > unsavedWidth ? savedWidth : unsavedWidth) + 82;
          final compactSave =
              compact || backWidth + saveWidth + 12 > constraints.maxWidth;
          final extent = MediaQuery.sizeOf(context).width <= 760 ? 48.0 : 52.0;
          final actionWidth = backWidth + (compactSave ? extent : saveWidth);
          final brandLabel = category == EntertainmentCategory.films
              ? 'RYHZE FILMS'
              : 'RYHZE GAMES';
          final inlineBrand =
              !compact &&
              actionWidth + measure(brandLabel, brandStyle) + 40 <=
                  constraints.maxWidth;
          final back = RyhzeControl(
            key: const Key('detail-back'),
            autofocus: true,
            label: 'Back',
            onPressed: () => Navigator.pop(context),
            child: const Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(Icons.arrow_back_rounded),
                SizedBox(width: 12),
                Text('Back'),
              ],
            ),
          );
          final save = RyhzeControl(
            label: saved ? 'Remove favourite' : 'Add favourite',
            selected: saved,
            iconOnly: compactSave,
            onPressed: onFavourite,
            child: compactSave
                ? Icon(saved ? Icons.check_rounded : Icons.add_rounded)
                : Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Icon(saved ? Icons.check_rounded : Icons.add_rounded),
                      const SizedBox(width: 12),
                      Text(label),
                    ],
                  ),
          );
          final brand = Text(brandLabel, style: brandStyle);
          if (inlineBrand) {
            return Row(
              children: [
                back,
                const SizedBox(width: 20),
                brand,
                const Spacer(),
                const SizedBox(width: 20),
                save,
              ],
            );
          }
          return Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              brand,
              const SizedBox(height: 12),
              if (actionWidth + 12 <= constraints.maxWidth)
                Row(children: [back, const Spacer(), save])
              else
                SizedBox(
                  width: double.infinity,
                  child: Wrap(
                    alignment: WrapAlignment.spaceBetween,
                    crossAxisAlignment: WrapCrossAlignment.center,
                    spacing: 12,
                    runSpacing: 12,
                    children: [back, save],
                  ),
                ),
            ],
          );
        },
      ),
    ),
  );
}

class _Tag extends StatelessWidget {
  const _Tag(this.text);
  final String text;
  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
    decoration: BoxDecoration(
      border: Border.all(color: RyhzeTokens.border),
      borderRadius: BorderRadius.circular(999),
    ),
    child: Text(
      text,
      style: const TextStyle(
        color: RyhzeTokens.muted,
        fontSize: 10,
        height: 1.65,
      ),
    ),
  );
}

class _Fact extends StatelessWidget {
  const _Fact(this.label, this.value);
  final String label, value;
  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: [
      Text(
        label,
        style: const TextStyle(
          color: RyhzeTokens.subtle,
          fontSize: 10,
          height: 1.65,
        ),
      ),
      const SizedBox(height: 8),
      Text(value, style: const TextStyle(fontSize: 13, height: 1.65)),
    ],
  );
}
