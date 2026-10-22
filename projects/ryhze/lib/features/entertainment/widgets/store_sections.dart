import 'package:flutter/material.dart';

import '../../../design/controls.dart';
import '../../../design/tokens.dart';
import '../model/catalog.dart';
import '../../../design/artwork.dart';
import '../../../design/artwork_presentation.dart';

/// Responsive measurements shared by the Store's hero and catalogue.
abstract final class StoreLayout {
  static double gutter(double width) => width <= 350
      ? 16
      : width <= 700
      ? 22
      : (width * .045).clamp(20, 88);

  static double cardGap(double width) => width <= 760 ? 24 : 32;

  static double cardWidth(double width) {
    final available = (width - 2 * gutter(width)).clamp(0.0, width);
    return width <= 480
        ? available
        : width <= 760
        ? available * .6 - 14.4
        : available * .4 - 25.6;
  }

  static int columns(double width) =>
      ((width - 2 * gutter(width) + cardGap(width)) /
              (cardWidth(width) + cardGap(width)))
          .floor()
          .clamp(1, 3);
}

class StoreHero extends StatelessWidget {
  const StoreHero({
    super.key,
    required this.width,
    required this.gutter,
    required this.category,
    required this.favourite,
    required this.onFavourite,
    required this.onExplore,
    this.title = Catalog.larcenous,
  });
  final double width, gutter;
  final EntertainmentCategory category;
  final ValueNotifier<bool> favourite;
  final VoidCallback onFavourite, onExplore;
  final EntertainmentTitle title;

  /// Matches the website's Exclusive container, independently of its 16:9 media.
  /// This is a minimum: larger text or longer titles may grow the content.
  static double minimumHeight(double width) =>
      width <= 700 ? 360 : (width * .367).clamp(400, 580);

  @override
  Widget build(BuildContext context) {
    final films = category == EntertainmentCategory.films;
    return ArtworkPresentation(
      reference: title.artwork,
      fallback: const ProjectArtwork(logoHeightFactor: .78, bottomShade: false),
      fallbackCaption: films
          ? 'Project preview · Not film footage'
          : title.id == Catalog.larcenous.id
          ? 'Project preview · Not gameplay'
          : 'Placeholder artwork',
      builder: (context, image, caption, retry) =>
          _build(context, image, caption),
    );
  }

  Widget _build(BuildContext context, Widget image, String caption) {
    final films = category == EntertainmentCategory.films;
    final mobile = width <= 700;
    final compact = width <= 1000;
    final enlarged = MediaQuery.textScalerOf(context).scale(1) > 1.3;
    final copyWidth = mobile || enlarged
        ? width
        : (width * (compact ? .58 : .46)).clamp(0, 740).toDouble();
    final titleSize = mobile
        ? (width * .07).clamp(28.0, 44.0)
        : compact
        ? (width * .038).clamp(28.0, 40.0)
        : (width * .036).clamp(32.0, 64.0);
    final titleText = !mobile && title.title == Catalog.larcenous.title
        ? 'Larcenous\nDriftscape'
        : !mobile && title.title == Catalog.tommyRiley.title
        ? 'Tommy\nRiley'
        : title.title;
    return Semantics(
      container: true,
      label: 'Ryhze Exclusives',
      child: ArtworkFrame(
        framed: false,
        child: ColoredBox(
          color: Colors.black,
          child: Stack(
            key: const Key('hero-surface'),
            children: [
              Positioned.fill(
                child: Align(
                  alignment: Alignment.centerRight,
                  child: FractionallySizedBox(
                    widthFactor: mobile ? 1 : .64,
                    heightFactor: 1,
                    child: ColoredBox(
                      key: const Key('exclusive-art-region'),
                      color: RyhzeTokens.raised,
                      child: Stack(
                        fit: StackFit.expand,
                        children: [
                          LayoutBuilder(
                            builder: (context, constraints) {
                              final mediaWidth = constraints.maxWidth.clamp(
                                0.0,
                                constraints.maxHeight * 16 / 9,
                              );
                              return Align(
                                alignment: Alignment.centerRight,
                                child: SizedBox(
                                  width: mediaWidth,
                                  child: AspectRatio(
                                    key: const Key('hero-art-16-9'),
                                    aspectRatio: 16 / 9,
                                    child: image,
                                  ),
                                ),
                              );
                            },
                          ),
                          DecoratedBox(
                            decoration: BoxDecoration(
                              gradient: LinearGradient(
                                colors: mobile || enlarged
                                    ? const [
                                        Colors.black,
                                        Color(0xEE000000),
                                        Color(0x88000000),
                                        Colors.transparent,
                                      ]
                                    : const [
                                        Colors.black,
                                        Colors.black,
                                        Color(0xBB000000),
                                        Colors.transparent,
                                      ],
                                stops: mobile || enlarged
                                    ? const [0, .24, .58, 1]
                                    : const [0, .18, .38, .70],
                              ),
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
              ConstrainedBox(
                constraints: BoxConstraints(minHeight: minimumHeight(width)),
                child: Align(
                  alignment: Alignment.centerLeft,
                  heightFactor: 1,
                  child: SizedBox(
                    key: const Key('exclusive-copy-region'),
                    width: copyWidth,
                    child: Padding(
                      padding: EdgeInsets.fromLTRB(
                        gutter,
                        compact ? 28 : 32,
                        mobile || enlarged ? gutter : 0,
                        compact ? 28 : 32,
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          Eyebrow(
                            films ? 'Ryhze Films' : 'Ryhze Games',
                            fontSize: mobile ? 10 : 12,
                          ),
                          SizedBox(height: mobile ? 12 : 16),
                          Text(
                            titleText,
                            style: heading(titleSize).copyWith(height: 1.06),
                          ),
                          const SizedBox(height: 16),
                          ConstrainedBox(
                            constraints: const BoxConstraints(maxWidth: 460),
                            child: Text(
                              compact && title.id == Catalog.larcenous.id
                                  ? 'Open-world crime and driving in San Coronado.'
                                  : compact &&
                                        title.id == Catalog.tommyRiley.id &&
                                        title.description ==
                                            Catalog.tommyRiley.description
                                  ? 'An influencer joins an underground organisation for money, then turns against it.'
                                  : title.description,
                              style: const TextStyle(
                                fontSize: 14,
                                height: 1.7,
                                color: RyhzeTokens.muted,
                              ),
                            ),
                          ),
                          const SizedBox(height: 20),
                          Wrap(
                            key: const Key('hero-actions'),
                            spacing: 12,
                            runSpacing: 12,
                            children: [
                              _HeroButton(
                                child: RyhzeControl(
                                  key: const Key('hero-explore'),
                                  primary: true,
                                  label: films
                                      ? 'Explore the film'
                                      : 'Explore the game',
                                  onPressed: onExplore,
                                  child: Row(
                                    mainAxisSize: MainAxisSize.min,
                                    children: [
                                      Flexible(
                                        child: Text(
                                          films
                                              ? 'Explore the film'
                                              : 'Explore the game',
                                        ),
                                      ),
                                      const SizedBox(width: 12),
                                      const Icon(Icons.arrow_forward_rounded),
                                    ],
                                  ),
                                ),
                              ),
                              ValueListenableBuilder<bool>(
                                valueListenable: favourite,
                                builder: (context, saved, _) => _HeroButton(
                                  child: RyhzeControl(
                                    key: const Key('hero-favourite'),
                                    iconOnly: compact,
                                    label: saved
                                        ? 'Remove favourite'
                                        : 'Add favourite',
                                    selected: saved,
                                    onPressed: onFavourite,
                                    child: compact
                                        ? Icon(
                                            saved
                                                ? Icons.check_rounded
                                                : Icons.add_rounded,
                                          )
                                        : Row(
                                            mainAxisSize: MainAxisSize.min,
                                            children: [
                                              Icon(
                                                saved
                                                    ? Icons.check_rounded
                                                    : Icons.add_rounded,
                                              ),
                                              const SizedBox(width: 12),
                                              Flexible(
                                                child: Text(
                                                  saved
                                                      ? 'Favourited'
                                                      : 'Favourites',
                                                ),
                                              ),
                                            ],
                                          ),
                                  ),
                                ),
                              ),
                            ],
                          ),
                          const SizedBox(height: 16),
                          Row(
                            children: [
                              Container(
                                width: 6,
                                height: 6,
                                decoration: const BoxDecoration(
                                  color: RyhzeTokens.accent,
                                  shape: BoxShape.circle,
                                ),
                              ),
                              const SizedBox(width: 12),
                              Flexible(
                                child: Text(
                                  '${title.status}${title.tags.isEmpty ? '' : '   /   ${title.tags.first}'}',
                                  style: const TextStyle(
                                    color: RyhzeTokens.muted,
                                    height: 1.7,
                                    fontSize: 12,
                                  ),
                                ),
                              ),
                            ],
                          ),
                          const SizedBox(height: 24),
                          Padding(
                            padding: const EdgeInsets.only(right: 90),
                            child: Text(
                              caption,
                              style: TextStyle(
                                fontSize: mobile ? 10 : 11,
                                height: 1.65,
                                color: RyhzeTokens.subtle,
                              ),
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
              Positioned(
                right: gutter,
                bottom: compact ? 28 : 32,
                child: Text(
                  '01  /  01',
                  style: TextStyle(
                    fontSize: mobile ? 10 : 11,
                    color: RyhzeTokens.subtle,
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _HeroButton extends StatelessWidget {
  const _HeroButton({required this.child});
  final Widget child;
  @override
  Widget build(BuildContext context) => ConstrainedBox(
    constraints: const BoxConstraints(minHeight: 52, minWidth: 52),
    child: DefaultTextStyle.merge(
      style: const TextStyle(
        fontSize: 14,
        height: 1.4,
        fontWeight: FontWeight.w500,
        letterSpacing: 0,
      ),
      child: IconTheme.merge(data: const IconThemeData(size: 22), child: child),
    ),
  );
}

class _ThumbnailBrand extends StatelessWidget {
  const _ThumbnailBrand({this.category = EntertainmentCategory.games});
  final EntertainmentCategory category;
  @override
  Widget build(BuildContext context) => Container(
    key: const Key('thumbnail-brand'),
    padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
    decoration: BoxDecoration(
      color: const Color(0x99000000),
      borderRadius: BorderRadius.circular(999),
    ),
    child: Text(
      category == EntertainmentCategory.films ? 'RYHZE FILMS' : 'RYHZE GAMES',
      style: const TextStyle(
        fontSize: 9,
        letterSpacing: .36,
        color: RyhzeTokens.text,
      ),
    ),
  );
}

class CatalogHeading extends StatelessWidget {
  const CatalogHeading({
    super.key,
    required this.category,
    required this.compact,
    this.width = 1280,
    this.count = 1,
  });
  final EntertainmentCategory category;
  final bool compact;
  final double width;
  final int count;
  @override
  Widget build(BuildContext context) {
    final copy = Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Eyebrow(category.name),
        const SizedBox(height: 16),
        Text(
          'In development.',
          style: heading((width * .04).clamp(36.8, 54.4)),
        ),
      ],
    );
    final status = StatusPill(
      count == 1 ? '01 title in development' : '$count titles',
    );
    return compact
        ? Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [copy, const SizedBox(height: 18), status],
          )
        : Row(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              Expanded(child: copy),
              status,
            ],
          );
  }
}

class GameTile extends StatefulWidget {
  const GameTile({
    super.key,
    required this.onOpen,
    this.title = Catalog.larcenous,
    this.category = EntertainmentCategory.games,
  });
  final VoidCallback onOpen;
  final EntertainmentTitle title;
  final EntertainmentCategory category;
  @override
  State<GameTile> createState() => _GameTileState();
}

class _GameTileState extends State<GameTile> {
  bool hovered = false;
  bool focused = false;
  @override
  Widget build(BuildContext context) => ArtworkPresentation(
    reference: widget.title.artwork,
    fallback: const ProjectArtwork(
      logoWidthFactor: 1,
      logoHeightFactor: 1,
      bottomShade: false,
    ),
    fallbackCaption: widget.category == EntertainmentCategory.films
        ? 'Project preview · Not film footage'
        : 'Placeholder artwork',
    builder: (context, image, caption, retry) => LayoutBuilder(
      builder: (context, constraints) {
        final viewportWidth = MediaQuery.sizeOf(context).width;
        final textScaler = MediaQuery.textScalerOf(context);
        final titleStyle = heading(
          viewportWidth <= 480 ? 24 : (viewportWidth * .02).clamp(20, 24),
        ).copyWith(height: 1.12, letterSpacing: -.96);
        Size measure(String text, TextStyle style, double width) {
          final painter = TextPainter(
            text: TextSpan(text: text, style: style),
            textDirection: Directionality.of(context),
            textScaler: textScaler,
          )..layout(maxWidth: width.clamp(1.0, double.infinity));
          final size = painter.size;
          painter.dispose();
          return size;
        }

        final titleSize = measure(
          widget.title.title,
          titleStyle,
          constraints.maxWidth - 72,
        );
        final brandSize = measure(
          widget.category == EntertainmentCategory.films
              ? 'RYHZE FILMS'
              : 'RYHZE GAMES',
          DefaultTextStyle.of(context).style
              .merge(const TextStyle(fontSize: 9, letterSpacing: .36)),
          constraints.maxWidth - 60,
        );
        // Keep the website's overlay while it fits. Enlarged or long text gets
        // its own space below the unchanged 16:9 artwork instead of clipping.
        final titleBelow =
            18 + brandSize.height + 10 + 16 + titleSize.height + 24 >
            constraints.maxWidth * 9 / 16;
        final title = Text(
          widget.title.title,
          key: ValueKey('game-tile-title-${widget.title.id}'),
          style: titleStyle,
        );
        return Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Semantics(
              key: const Key('catalog-tile'),
              button: true,
              label: 'Explore ${widget.title.title}',
              child: FocusableActionDetector(
                onShowHoverHighlight: (value) =>
                    setState(() => hovered = value),
                onShowFocusHighlight: (value) =>
                    setState(() => focused = value),
                mouseCursor: SystemMouseCursors.click,
                actions: <Type, Action<Intent>>{
                  ActivateIntent: CallbackAction<ActivateIntent>(
                    onInvoke: (_) {
                      widget.onOpen();
                      return null;
                    },
                  ),
                },
                child: GestureDetector(
                  onTap: widget.onOpen,
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      AnimatedScale(
                        duration: RyhzeTokens.motion(
                          context,
                          const Duration(milliseconds: 600),
                        ),
                        curve: RyhzeTokens.ease,
                        scale:
                            hovered && !MediaQuery.disableAnimationsOf(context)
                            ? 1.025
                            : 1,
                        child: ArtworkFrame(
                          focused: focused,
                          child: AspectRatio(
                            key: const Key('tile-art-16-9'),
                            aspectRatio: 16 / 9,
                            child: Stack(
                              fit: StackFit.expand,
                              children: [
                                image,
                                const DecoratedBox(
                                  decoration: BoxDecoration(
                                    gradient: LinearGradient(
                                      begin: Alignment.topCenter,
                                      end: Alignment.bottomCenter,
                                      colors: [
                                        Colors.transparent,
                                        Color(0xEF000000),
                                      ],
                                      stops: [.25, 1],
                                    ),
                                  ),
                                ),
                                Positioned(
                                  left: 22,
                                  right: 22,
                                  top: 18,
                                  child: Align(
                                    alignment: Alignment.centerLeft,
                                    child: _ThumbnailBrand(
                                      category: widget.category,
                                    ),
                                  ),
                                ),
                                if (!titleBelow)
                                  Positioned(
                                    left: 22,
                                    right: 50,
                                    bottom: 24,
                                    child: title,
                                  ),
                                const Positioned(
                                  right: 20,
                                  bottom: 24,
                                  child: Icon(
                                    Icons.arrow_forward_rounded,
                                    size: 20,
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                      if (titleBelow)
                        Padding(
                          padding: const EdgeInsets.only(top: 12),
                          child: title,
                        ),
                    ],
                  ),
                ),
              ),
            ),
            if (widget.title.artwork != null)
              Padding(
                padding: const EdgeInsets.only(top: 12),
                child: Text(
                  caption,
                  style: const TextStyle(
                    fontSize: 11,
                    color: RyhzeTokens.muted,
                  ),
                ),
              ),
          ],
        );
      },
    ),
  );
}

class FilmsEmpty extends StatelessWidget {
  const FilmsEmpty({super.key});
  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.all(32),
    decoration: BoxDecoration(
      color: RyhzeTokens.surface,
      borderRadius: BorderRadius.circular(RyhzeTokens.radius),
      border: Border.all(color: RyhzeTokens.border),
    ),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Icon(Icons.movie_outlined, color: RyhzeTokens.accent, size: 36),
        const SizedBox(height: 24),
        Text('No films available yet.', style: heading(28)),
        const SizedBox(height: 12),
        const Text(
          'New films will appear here when they’re ready. Nothing is available to watch yet.',
          style: TextStyle(color: RyhzeTokens.muted, height: 1.8),
        ),
      ],
    ),
  );
}
