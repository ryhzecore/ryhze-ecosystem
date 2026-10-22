import 'package:flutter/material.dart';

import 'tokens.dart';

/// One control implementation keeps hover, focus, sizing and icon geometry aligned.
/// Hover changes paint/scale only; it never changes layout or translates vertically.
class RyhzeControl extends StatefulWidget {
  const RyhzeControl({
    super.key,
    required this.child,
    required this.label,
    required this.onPressed,
    this.primary = false,
    this.outline = false,
    this.iconOnly = false,
    this.autofocus = false,
    this.focusNode,
    this.selected = false,
    this.padding = const EdgeInsets.symmetric(horizontal: 24, vertical: 14),
  });
  final Widget child;
  final String label;
  final VoidCallback? onPressed;
  final bool primary, outline, iconOnly, autofocus, selected;
  final FocusNode? focusNode;
  final EdgeInsetsGeometry padding;
  @override
  State<RyhzeControl> createState() => _RyhzeControlState();
}

class _RyhzeControlState extends State<RyhzeControl> {
  bool hovered = false;
  bool focused = false;
  @override
  Widget build(BuildContext context) {
    final enabled = widget.onPressed != null;
    final hover = enabled && hovered;
    final extent = MediaQuery.sizeOf(context).width <= 760 ? 48.0 : 52.0;
    final foreground = !enabled
        ? RyhzeTokens.subtle
        : widget.primary
        ? RyhzeTokens.onPrimary
        : widget.selected
        ? RyhzeTokens.accentSoft
        : RyhzeTokens.text;
    return Semantics(
      button: true,
      enabled: widget.onPressed != null,
      label: widget.label,
      selected: widget.selected,
      child: Tooltip(
        message: widget.iconOnly ? widget.label : '',
        child: FocusableActionDetector(
          focusNode: widget.focusNode,
          autofocus: widget.autofocus,
          enabled: widget.onPressed != null,
          mouseCursor: widget.onPressed == null
              ? SystemMouseCursors.basic
              : SystemMouseCursors.click,
          onShowHoverHighlight: (value) => setState(() => hovered = value),
          onShowFocusHighlight: (value) => setState(() => focused = value),
          actions: <Type, Action<Intent>>{
            ActivateIntent: CallbackAction<ActivateIntent>(
              onInvoke: (_) {
                widget.onPressed?.call();
                return null;
              },
            ),
          },
          child: GestureDetector(
            behavior: HitTestBehavior.opaque,
            onTap: widget.onPressed,
            child: AnimatedContainer(
              duration: RyhzeTokens.motion(context, RyhzeTokens.surfaceMotion),
              curve: RyhzeTokens.ease,
              constraints: widget.iconOnly
                  ? BoxConstraints.tightFor(width: extent, height: extent)
                  : BoxConstraints(minHeight: extent),
              padding: widget.iconOnly
                  ? const EdgeInsets.all(12)
                  : widget.padding,
              decoration: BoxDecoration(
                color: !enabled
                    ? RyhzeTokens.raised
                    : widget.primary
                    ? hover
                          ? RyhzeTokens.primaryHover
                          : RyhzeTokens.primary
                    : widget.outline
                    ? hover
                          ? const Color(0x07FFFFFF)
                          : Colors.transparent
                    : hover
                    ? RyhzeTokens.secondaryHover
                    : RyhzeTokens.raised,
                borderRadius: BorderRadius.circular(999),
                border: Border.all(
                  color: enabled && focused
                      ? RyhzeTokens.focus
                      : hover || (enabled && widget.selected)
                      ? RyhzeTokens.borderHover
                      : widget.primary && enabled
                      ? Colors.transparent
                      : RyhzeTokens.border,
                  width: 1,
                ),
                boxShadow: [
                  if (hover)
                    ...RyhzeTokens.shadow
                  else
                    ...RyhzeTokens.buttonShadow,
                  if (enabled && focused) ...[
                    // A dark gap keeps the focus ring distinct on white actions.
                    const BoxShadow(color: RyhzeTokens.focus, spreadRadius: 3),
                    const BoxShadow(
                      color: RyhzeTokens.background,
                      spreadRadius: 1,
                    ),
                  ],
                ],
              ),
              child: IconTheme(
                data: IconThemeData(size: 20, color: foreground),
                child: DefaultTextStyle(
                  style: TextStyle(
                    fontFamily: 'Inter',
                    color: foreground,
                    fontSize: 14,
                    fontWeight: FontWeight.w500,
                    height: 1.4,
                  ),
                  child: widget.child,
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class Eyebrow extends StatelessWidget {
  const Eyebrow(this.text, {super.key, this.fontSize = 12});
  final String text;
  final double fontSize;
  @override
  Widget build(BuildContext context) => Text(
    text.toUpperCase(),
    style: TextStyle(
      color: RyhzeTokens.accentSoft,
      fontSize: fontSize,
      fontWeight: FontWeight.w500,
      letterSpacing: fontSize * .14,
    ),
  );
}

class StatusPill extends StatelessWidget {
  const StatusPill(this.text, {super.key});
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
        fontSize: 12,
        fontWeight: FontWeight.w500,
        height: 1.5,
      ),
    ),
  );
}
