import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'controls.dart';
import 'tokens.dart';

class HeaderMenu extends StatefulWidget {
  const HeaderMenu({
    super.key,
    required this.focusNode,
    required this.onSelected,
    this.overlayChanges,
  });
  final Listenable? overlayChanges;
  final FocusNode focusNode;
  final ValueChanged<String> onSelected;
  @override
  State<HeaderMenu> createState() => _HeaderMenuState();
}

class _HeaderMenuState extends State<HeaderMenu> {
  final controller = MenuController();
  final firstItemFocus = FocusNode(debugLabel: 'Menu Games');
  final triggerKey = GlobalKey();
  double anchorBottom = 96.0;
  bool anchorMeasurementScheduled = false;
  bool open = false;

  @override
  void initState() {
    super.initState();
    widget.overlayChanges?.addListener(closeForOverlay);
  }

  void closeForOverlay() {
    if (mounted && controller.isOpen) controller.close();
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    scheduleAnchorMeasurement();
  }

  @override
  void didUpdateWidget(covariant HeaderMenu oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.overlayChanges != widget.overlayChanges) {
      oldWidget.overlayChanges?.removeListener(closeForOverlay);
      widget.overlayChanges?.addListener(closeForOverlay);
    }
    scheduleAnchorMeasurement();
  }

  void scheduleAnchorMeasurement() {
    if (anchorMeasurementScheduled) return;
    anchorMeasurementScheduled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      anchorMeasurementScheduled = false;
      if (mounted) updateAnchorBottom();
    });
  }

  void updateAnchorBottom() {
    if (!mounted) return;
    // The borrowed focus node may still reference the outgoing header layout.
    // Measure this state's current trigger only after layout or when opening.
    final anchor = triggerKey.currentContext?.findRenderObject();
    if (anchor is! RenderBox || !anchor.attached || !anchor.hasSize) return;
    final bottom = anchor.localToGlobal(Offset(0, anchor.size.height)).dy;
    if (!bottom.isFinite || bottom == anchorBottom) return;
    setState(() => anchorBottom = bottom);
  }

  @override
  void dispose() {
    widget.overlayChanges?.removeListener(closeForOverlay);
    firstItemFocus.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final size = MediaQuery.sizeOf(context);
    final menuWidth = (size.width - 40).clamp(0.0, 270.0);
    final maxHeight = math.max(0.0, size.height - 40);
    final availableHeight = (size.height - anchorBottom - 20).clamp(
      math.min(96.0, maxHeight),
      maxHeight,
    );
    return MenuAnchor(
      controller: controller,
      childFocusNode: widget.focusNode,
      consumeOutsideTap: true,
      alignmentOffset: Offset(-menuWidth, 12),
      onOpen: () {
        if (!mounted) return;
        updateAnchorBottom();
        setState(() => open = true);
        firstItemFocus.requestFocus();
      },
      onClose: () {
        if (!mounted) return;
        setState(() => open = false);
        widget.focusNode.requestFocus();
      },
      style: MenuStyle(
        minimumSize: WidgetStatePropertyAll(Size(menuWidth, 0)),
        maximumSize: WidgetStatePropertyAll(
          Size(menuWidth, availableHeight.toDouble()),
        ),
        alignment: AlignmentDirectional.bottomEnd,
        backgroundColor: const WidgetStatePropertyAll(RyhzeTokens.raised),
        surfaceTintColor: const WidgetStatePropertyAll(Colors.transparent),
        padding: const WidgetStatePropertyAll(EdgeInsets.all(22)),
        shape: WidgetStatePropertyAll(
          RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(RyhzeTokens.radius),
            side: const BorderSide(color: RyhzeTokens.border),
          ),
        ),
      ),
      menuChildren: [
        for (final item in [
          ('Games', Icons.sports_esports_outlined, 'games'),
          ('Films', Icons.movie_outlined, 'films'),
          ('About this app', Icons.info_outline_rounded, 'about'),
          ('Your Ryhze', Icons.tune_rounded, 'workspace'),
        ])
          CallbackShortcuts(
            bindings: {
              const SingleActivator(LogicalKeyboardKey.gameButtonB):
                  controller.close,
            },
            child: MenuItemButton(
              key: Key('menu-${item.$3}'),
              focusNode: item.$3 == 'games' ? firstItemFocus : null,
              requestFocusOnHover: false,
              overflowAxis: Axis.vertical,
              style: ButtonStyle(
                fixedSize: WidgetStatePropertyAll(
                  Size(math.max(0.0, menuWidth - 44), double.infinity),
                ),
                minimumSize: const WidgetStatePropertyAll(Size(0, 48)),
                padding: const WidgetStatePropertyAll(
                  EdgeInsets.symmetric(horizontal: 4, vertical: 10),
                ),
                foregroundColor: const WidgetStatePropertyAll(RyhzeTokens.text),
                overlayColor: const WidgetStatePropertyAll(Color(0x08FFFFFF)),
                textStyle: const WidgetStatePropertyAll(
                  TextStyle(
                    fontFamily: 'Inter',
                    fontSize: 13,
                    fontWeight: FontWeight.w500,
                    height: 1.4,
                  ),
                ),
                side: WidgetStateProperty.resolveWith(
                  (states) => BorderSide(
                    color:
                        states.contains(WidgetState.focused) &&
                            FocusManager.instance.highlightMode ==
                                FocusHighlightMode.traditional
                        ? RyhzeTokens.accent
                        : Colors.transparent,
                  ),
                ),
                shape: WidgetStatePropertyAll(
                  RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(RyhzeTokens.itemRadius),
                  ),
                ),
              ),
              trailingIcon: Icon(item.$2, size: 18, color: RyhzeTokens.subtle),
              onPressed: () => widget.onSelected(item.$3),
              child: Text(item.$1, softWrap: true),
            ),
          ),
      ],
      builder: (context, controller, child) => Semantics(
        // Separate MenuAnchor and Tooltip traversal anchors while keeping the
        // expanded state on the labelled button (flutter/flutter#182444).
        container: true,
        expanded: open,
        child: KeyedSubtree(
          key: triggerKey,
          child: RyhzeControl(
            key: const Key('menu-trigger'),
            focusNode: widget.focusNode,
            label: open ? 'Close Ryhze menu' : 'Ryhze menu',
            iconOnly: true,
            outline: true,
            onPressed: () =>
                controller.isOpen ? controller.close() : controller.open(),
            child: MenuToggleIcon(isOpen: open),
          ),
        ),
      ),
    );
  }
}

class MenuToggleIcon extends StatelessWidget {
  const MenuToggleIcon({super.key, required this.isOpen});
  final bool isOpen;
  @override
  Widget build(BuildContext context) => TweenAnimationBuilder<double>(
    tween: Tween(end: isOpen ? 1 : 0),
    duration: RyhzeTokens.motion(context, RyhzeTokens.frameOut),
    curve: RyhzeTokens.ease,
    builder: (context, value, child) =>
        CustomPaint(size: const Size(20, 20), painter: _MenuLines(value)),
  );
}

class _MenuLines extends CustomPainter {
  const _MenuLines(this.progress);
  final double progress;
  @override
  void paint(Canvas canvas, Size size) {
    final pen = Paint()
      ..color = RyhzeTokens.text
      ..strokeWidth = 1.8
      ..strokeCap = StrokeCap.round;
    for (final direction in [-1.0, 1.0]) {
      canvas.save();
      canvas.translate(
        size.width / 2,
        size.height / 2 + direction * 3 * (1 - progress),
      );
      canvas.rotate(direction * math.pi / 4 * progress);
      canvas.drawLine(const Offset(-7, 0), const Offset(7, 0), pen);
      canvas.restore();
    }
  }

  @override
  bool shouldRepaint(_MenuLines oldDelegate) =>
      oldDelegate.progress != progress;
}
