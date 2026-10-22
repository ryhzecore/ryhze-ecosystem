import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/design/controls.dart';

/// Observe the real trigger node; debug labels do not exist in Profile builds.
bool menuTriggerHasPrimaryFocus() {
  final matches = find.byKey(const Key('menu-trigger')).evaluate();
  if (matches.length != 1) return false;
  final widget = matches.single.widget;
  if (widget is! RyhzeControl) return false;
  final node = widget.focusNode;
  return node != null &&
      node.hasFocus &&
      identical(FocusManager.instance.primaryFocus, node);
}
