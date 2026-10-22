import 'package:flutter/material.dart';

import '../../../design/controls.dart';
import '../../../design/tokens.dart';

import 'package:flutter/gestures.dart';

import '../../../design/header_menu.dart';
import '../model/catalog.dart';

class EntertainmentHeader extends StatelessWidget {
  const EntertainmentHeader({
    super.key,
    required this.width,
    this.overlayChanges,
    required this.gutter,
    required this.category,
    required this.onCategory,
    required this.onSearch,
    required this.searchLabel,
    required this.onMenuChoice,
    required this.searchFocus,
    required this.menuFocus,
    this.showChooser = true,
    this.libraryDestination = false,
    this.onDestination,
  });
  final Listenable? overlayChanges;
  final double width, gutter;
  final EntertainmentCategory category;
  final ValueChanged<EntertainmentCategory> onCategory;
  final VoidCallback onSearch;
  final String searchLabel;
  final ValueChanged<String> onMenuChoice;
  final FocusNode searchFocus, menuFocus;
  final bool showChooser;
  final bool libraryDestination;
  final ValueChanged<bool>? onDestination;
  @override
  Widget build(BuildContext context) {
    final compact = width <= 760;
    final brandWidth = compact ? 34.0 : 144.0;
    final actionWidth = compact ? 96.0 : 104.0;
    final actionGap = width <= 420 ? 4.0 : (compact ? 8.0 : 10.0);
    double measure(String text, TextStyle style) {
      final painter = TextPainter(
        text: TextSpan(text: text, style: style),
        textDirection: Directionality.of(context),
        textScaler: MediaQuery.textScalerOf(context),
      )..layout();
      final result = painter.width;
      painter.dispose();
      return result;
    }

    final brandGap = compact ? 4.0 : 12.0;
    final headerGap = width <= 420
        ? 6.0
        : compact
        ? 12.0
        : 24.0;
    final badgeStyle = TextStyle(
      fontSize: compact ? 9 : 12,
      fontWeight: FontWeight.w500,
      height: 1.2,
      letterSpacing: compact ? .225 : .3,
      color: RyhzeTokens.onPrimary,
    );
    final badgeWidth = measure('PRE-ALPHA', badgeStyle) + (compact ? 8 : 20);
    final renderedBadgeWidth =
        measure('PRE-ALPHA', badgeStyle.copyWith(fontFamily: 'Inter')) +
        (compact ? 8 : 20);
    final chooserWidth =
        (measure(
                      'Games',
                      TextStyle(
                        fontFamily: 'Inter',
                        fontSize: compact ? 12 : 13,
                      ),
                    ) +
                    24)
                .clamp(compact ? 62.0 : 80.0, double.infinity) *
            2 +
        16;
    const destinationStyle = TextStyle(
      fontFamily: 'Inter',
      fontSize: 14,
      height: 1.4,
    );
    double destinationWidth(String label) {
      // Reserve either selection weight so switching destinations never reflows
      // the header. Include the shared control's padding and border.
      final regular = measure(
        label,
        destinationStyle.copyWith(fontWeight: FontWeight.w400),
      );
      final selected = measure(
        label,
        destinationStyle.copyWith(fontWeight: FontWeight.w600),
      );
      return (regular > selected ? regular : selected).ceilToDouble() + 50;
    }

    final storeWidth = onDestination == null ? 0.0 : destinationWidth('Store');
    final libraryWidth = onDestination == null
        ? 0.0
        : destinationWidth('Library');
    final requiredWidth =
        brandWidth +
        brandGap +
        badgeWidth +
        headerGap +
        chooserWidth +
        (onDestination == null
            ? 0
            : headerGap * 2 + storeWidth + 8 + libraryWidth) +
        actionWidth +
        actionGap;
    final verySmall =
        width - gutter * 2 < requiredWidth ||
        MediaQuery.textScalerOf(context).scale(1) > 1.4;
    final Widget chooser = chooserWidth <= width - gutter * 2
        ? _Chooser(category: category, onChanged: onCategory, compact: compact)
        : Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              for (final value in EntertainmentCategory.values)
                RyhzeControl(
                  key: Key('category-${value.name}'),
                  label: value == EntertainmentCategory.games
                      ? 'Games'
                      : 'Films',
                  selected: category == value,
                  primary: category == value,
                  onPressed: () => onCategory(value),
                  child: Text(
                    value == EntertainmentCategory.games ? 'Games' : 'Films',
                  ),
                ),
            ],
          );
    final navigation = Wrap(
      spacing: headerGap,
      runSpacing: 8,
      crossAxisAlignment: WrapCrossAlignment.center,
      children: [
        chooser,
        if (onDestination != null)
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              for (final library in [false, true])
                SizedBox(
                  width: library ? libraryWidth : storeWidth,
                  child: RyhzeControl(
                    key: Key(
                      library ? 'destination-library' : 'destination-store',
                    ),
                    label: library ? 'Games Library' : 'Games Store',
                    selected: libraryDestination == library,
                    onPressed: () => onDestination!(library),
                    child: Text(
                      library ? 'Library' : 'Store',
                      textAlign: TextAlign.center,
                      style: destinationStyle.copyWith(
                        color: libraryDestination == library
                            ? RyhzeTokens.accent
                            : RyhzeTokens.muted,
                        fontWeight: libraryDestination == library
                            ? FontWeight.w600
                            : FontWeight.w400,
                      ),
                    ),
                  ),
                ),
            ],
          ),
      ],
    );
    final brand = Semantics(
      key: const Key('header-brand'),
      label: 'Ryhze pre-alpha',
      image: true,
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Image.asset(
            compact
                ? 'assets/brand/symbol-480.webp'
                : 'assets/brand/wordmark-348.webp',
            width: brandWidth,
            height: compact ? 34 : 144 * 2480 / 7256,
            fit: BoxFit.contain,
            cacheWidth: compact ? 96 : 348,
          ),
          SizedBox(width: brandGap),
          Container(
            key: const Key('pre-alpha-badge'),
            padding: EdgeInsets.symmetric(
              horizontal: compact ? 4 : 10,
              vertical: compact ? 3 : 6,
            ),
            decoration: BoxDecoration(
              color: RyhzeTokens.primary,
              borderRadius: BorderRadius.circular(999),
            ),
            child: Text('PRE-ALPHA', style: badgeStyle),
          ),
        ],
      ),
    );
    final actions = Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        RyhzeControl(
          key: const Key('search-trigger'),
          focusNode: searchFocus,
          label: searchLabel,
          iconOnly: true,
          outline: true,
          onPressed: onSearch,
          child: const Icon(Icons.search_rounded),
        ),
        SizedBox(width: actionGap),
        HeaderMenu(
          focusNode: menuFocus,
          onSelected: onMenuChoice,
          overlayChanges: overlayChanges,
        ),
      ],
    );
    return Container(
      constraints: BoxConstraints(minHeight: compact ? 88 : 104),
      decoration: const BoxDecoration(
        color: RyhzeTokens.background,
        border: Border(bottom: BorderSide(color: RyhzeTokens.border)),
      ),
      padding: EdgeInsets.symmetric(
        horizontal: gutter,
        vertical: compact ? 16 : 24,
      ),
      child: !showChooser
          ? SizedBox(
              width: double.infinity,
              child: Wrap(
                alignment: WrapAlignment.spaceBetween,
                crossAxisAlignment: WrapCrossAlignment.center,
                spacing: 12,
                runSpacing: 12,
                children: [brand, actions],
              ),
            )
          : verySmall
          ? Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                if (brandWidth +
                        brandGap +
                        renderedBadgeWidth +
                        actionWidth +
                        actionGap <=
                    width - gutter * 2)
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceBetween,
                    children: [brand, actions],
                  )
                else
                  SizedBox(
                    width: double.infinity,
                    child: Wrap(
                      alignment: WrapAlignment.spaceBetween,
                      spacing: 12,
                      runSpacing: 12,
                      crossAxisAlignment: WrapCrossAlignment.center,
                      children: [brand, actions],
                    ),
                  ),
                const SizedBox(height: 14),
                navigation,
              ],
            )
          : Row(
              children: [
                brand,
                SizedBox(width: headerGap),
                navigation,
                const Spacer(),
                actions,
              ],
            ),
    );
  }
}

class _Chooser extends StatefulWidget {
  const _Chooser({
    required this.category,
    required this.onChanged,
    required this.compact,
  });
  final EntertainmentCategory category;
  final ValueChanged<EntertainmentCategory> onChanged;
  final bool compact;
  @override
  State<_Chooser> createState() => _ChooserState();
}

class _ChooserState extends State<_Chooser> {
  double? dragProgress;
  final focusedCategories = <EntertainmentCategory>{};
  bool pointerCancelled = false;
  EntertainmentCategory get category => widget.category;
  bool get compact => widget.compact;
  void onChanged(EntertainmentCategory value) => widget.onChanged(value);
  double get progress =>
      dragProgress ?? (category == EntertainmentCategory.games ? 0 : 1);

  @override
  Widget build(BuildContext context) {
    final style = TextStyle(
      inherit: false,
      fontFamily: 'Inter',
      fontSize: compact ? 12 : 13,
      fontWeight: FontWeight.w400,
      letterSpacing: 0,
    );
    final measurement = TextPainter(
      text: TextSpan(text: 'Games', style: style),
      textDirection: Directionality.of(context),
      textScaler: MediaQuery.textScalerOf(context),
    )..layout();
    final itemWidth = (measurement.width + 24).clamp(
      compact ? 62.0 : 80.0,
      double.infinity,
    );
    final itemHeight = (measurement.height + 24).clamp(48.0, double.infinity);
    measurement.dispose();
    return Listener(
      onPointerDown: (_) => pointerCancelled = false,
      onPointerCancel: (_) => pointerCancelled = true,
      child: GestureDetector(
        dragStartBehavior: DragStartBehavior.down,
        onHorizontalDragStart: (_) => setState(() => dragProgress = progress),
        onHorizontalDragUpdate: (details) => setState(() {
          final direction = Directionality.of(context) == TextDirection.rtl
              ? -1
              : 1;
          dragProgress =
              (progress + direction * details.primaryDelta! / (itemWidth + 4))
                  .clamp(0.0, 1.0);
        }),
        onHorizontalDragEnd: (_) {
          final next = progress >= .5
              ? EntertainmentCategory.films
              : EntertainmentCategory.games;
          setState(() => dragProgress = null);
          if (!pointerCancelled) onChanged(next);
        },
        onHorizontalDragCancel: () => setState(() => dragProgress = null),
        child: Container(
          key: const Key('category-chooser'),
          padding: const EdgeInsets.all(5),
          decoration: BoxDecoration(
            color: RyhzeTokens.surface,
            borderRadius: BorderRadius.circular(999),
            border: Border.all(color: RyhzeTokens.border),
            boxShadow: RyhzeTokens.shadow,
          ),
          child: ClipRRect(
            borderRadius: BorderRadius.circular(999),
            child: SizedBox(
              width: itemWidth * 2 + 4,
              height: itemHeight,
              child: TweenAnimationBuilder<double>(
                tween: Tween(begin: progress, end: progress),
                duration: dragProgress != null
                    ? Duration.zero
                    : RyhzeTokens.motion(
                        context,
                        const Duration(milliseconds: 650),
                      ),
                curve: RyhzeTokens.ease,
                builder: (context, paintedProgress, _) {
                  final direction = Directionality.of(context);
                  final physicalProgress = direction == TextDirection.rtl
                      ? 1 - paintedProgress
                      : paintedProgress;
                  final pillBounds = Rect.fromLTWH(
                    physicalProgress * (itemWidth + 4),
                    0,
                    itemWidth,
                    itemHeight,
                  );
                  return Stack(
                    children: [
                      Positioned.fromRect(
                        rect: pillBounds,
                        child: DecoratedBox(
                          key: const Key('category-indicator'),
                          decoration: BoxDecoration(
                            color: RyhzeTokens.primary,
                            borderRadius: BorderRadius.circular(999),
                          ),
                        ),
                      ),
                      Row(
                        children: [
                          for (final value in EntertainmentCategory.values) ...[
                            if (value == EntertainmentCategory.films)
                              const SizedBox(width: 4),
                            SizedBox(
                              width: itemWidth,
                              height: itemHeight,
                              child: Semantics(
                                selected: category == value,
                                child: TextButton(
                                  key: Key('category-${value.name}'),
                                  style:
                                      TextButton.styleFrom(
                                        // Desktop density must not shrink the hit target independently of the pill.
                                        visualDensity: VisualDensity.standard,
                                        tapTargetSize:
                                            MaterialTapTargetSize.shrinkWrap,
                                        padding: EdgeInsets.zero,
                                        foregroundColor: RyhzeTokens.muted,
                                        shape: const StadiumBorder(),
                                        textStyle: style,
                                      ).copyWith(
                                        side: WidgetStateProperty.resolveWith(
                                          (states) => BorderSide(
                                            color:
                                                states.contains(
                                                  WidgetState.focused,
                                                )
                                                ? RyhzeTokens.focus
                                                : Colors.transparent,
                                            width: 2,
                                          ),
                                        ),
                                      ),
                                  onFocusChange: (focused) => setState(() {
                                    if (focused) {
                                      focusedCategories.add(value);
                                    } else {
                                      focusedCategories.remove(value);
                                    }
                                  }),
                                  onPressed: () => onChanged(value),
                                  child: Text(
                                    value == EntertainmentCategory.games
                                        ? 'Games'
                                        : 'Films',
                                    style: style.copyWith(
                                      color: RyhzeTokens.muted,
                                    ),
                                  ),
                                ),
                              ),
                            ),
                          ],
                        ],
                      ),
                      // Repaint only the label/focus ink covered by the actual pill.
                      // Decorative spans avoid duplicate accessible labels or buttons.
                      Positioned.fill(
                        child: IgnorePointer(
                          child: ExcludeSemantics(
                            child: ClipRRect(
                              clipper: _ChooserInkClipper(pillBounds),
                              child: Row(
                                children: [
                                  for (final value
                                      in EntertainmentCategory.values) ...[
                                    if (value == EntertainmentCategory.films)
                                      const SizedBox(width: 4),
                                    SizedBox(
                                      width: itemWidth,
                                      height: itemHeight,
                                      child: DecoratedBox(
                                        decoration: BoxDecoration(
                                          borderRadius: BorderRadius.circular(
                                            999,
                                          ),
                                          border:
                                              focusedCategories.contains(value)
                                              ? Border.all(
                                                  color: RyhzeTokens.onPrimary,
                                                  width: 2,
                                                )
                                              : null,
                                        ),
                                        child: Center(
                                          child: RichText(
                                            textScaler: MediaQuery.textScalerOf(
                                              context,
                                            ),
                                            textDirection: direction,
                                            text: TextSpan(
                                              text:
                                                  value ==
                                                      EntertainmentCategory
                                                          .games
                                                  ? 'Games'
                                                  : 'Films',
                                              style: style.copyWith(
                                                color: RyhzeTokens.onPrimary,
                                              ),
                                            ),
                                          ),
                                        ),
                                      ),
                                    ),
                                  ],
                                ],
                              ),
                            ),
                          ),
                        ),
                      ),
                    ],
                  );
                },
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _ChooserInkClipper extends CustomClipper<RRect> {
  const _ChooserInkClipper(this.bounds);
  final Rect bounds;
  @override
  RRect getClip(Size size) =>
      RRect.fromRectAndRadius(bounds, const Radius.circular(999));
  @override
  bool shouldReclip(_ChooserInkClipper oldClipper) =>
      oldClipper.bounds != bounds;
}
