import 'dart:typed_data';
import 'dart:ui' as ui;
import 'dart:ui';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/design/header_menu.dart';

// Observe serialized updates, not just the framework's pre-serialization tree.
class _WireBinding extends AutomatedTestWidgetsFlutterBinding {
  final nodes =
      <
        int,
        ({String label, int? parent, List<int> traversal, List<int> hit})
      >{};
  final failures = <String>[];
  @override
  ui.SemanticsUpdateBuilder createSemanticsUpdateBuilder() =>
      _WireBuilder(this);
  void verify(Set<int> changed) {
    if (!nodes.containsKey(0)) return;
    Set<int> reachable(bool traversal) {
      final seen = <int>{};
      void walk(int id) {
        if (!seen.add(id)) return;
        final node = nodes[id];
        if (node == null) {
          failures.add('Missing serialized node $id');
          return;
        }
        for (final child in traversal ? node.traversal : node.hit) {
          walk(child);
        }
      }

      walk(0);
      return seen;
    }

    final traversal = reachable(true);
    final hit = reachable(false);
    // Untouched removed nodes may remain in this incremental snapshot. However,
    // every node emitted in THIS update must belong to the resulting traversal
    // tree, including new overlay nodes absent from both traversal and hit-test
    // child lists. Such emitted orphans are rejected by the native AXTree.
    for (final id in changed.difference(traversal)) {
      failures.add(
        'Emitted node $id (${nodes[id]?.label}) is absent from the resulting traversal tree; parent=${nodes[id]?.parent}',
      );
    }
    final parents = <int, List<int>>{};
    for (final id in traversal) {
      for (final child in nodes[id]?.traversal ?? const <int>[]) {
        parents.putIfAbsent(child, () => <int>[]).add(id);
      }
    }
    for (final id in traversal.where((id) => id != 0)) {
      if (parents[id]?.length != 1) {
        failures.add(
          'Traversal node $id must have exactly one parent claim: ${parents[id]}',
        );
      }
    }
    for (final id in hit.difference(traversal)) {
      failures.add(
        'Node $id (${nodes[id]?.label}) is hit-test reachable but absent from traversal; parent=${nodes[id]?.parent}',
      );
    }
    for (final id in traversal) {
      final parent = nodes[id]?.parent;
      if (parent != null &&
          parent >= 0 &&
          !(nodes[parent]?.traversal.contains(id) ?? false)) {
        failures.add(
          'Node $id declares traversal parent $parent without a child claim',
        );
      }
    }
  }
}

class _WireBuilder extends Fake implements ui.SemanticsUpdateBuilder {
  _WireBuilder(this.owner);
  final _WireBinding owner;
  final _builder = ui.SemanticsUpdateBuilder();
  final changed = <int>{};
  @override
  void updateNode({
    required int id,
    required SemanticsFlags flags,
    required int actions,
    required int maxValueLength,
    required int currentValueLength,
    required int textSelectionBase,
    required int textSelectionExtent,
    required int platformViewId,
    required int scrollChildren,
    required int scrollIndex,
    required int? traversalParent,
    required double scrollPosition,
    required double scrollExtentMax,
    required double scrollExtentMin,
    required Rect rect,
    required String identifier,
    required String label,
    List<StringAttribute>? labelAttributes,
    required String value,
    List<StringAttribute>? valueAttributes,
    required String increasedValue,
    List<StringAttribute>? increasedValueAttributes,
    required String decreasedValue,
    List<StringAttribute>? decreasedValueAttributes,
    required String hint,
    List<StringAttribute>? hintAttributes,
    String? tooltip,
    TextDirection? textDirection,
    required Float64List transform,
    required Float64List hitTestTransform,
    required Int32List childrenInTraversalOrder,
    required Int32List childrenInHitTestOrder,
    required Int32List additionalActions,
    int headingLevel = 0,
    String? linkUrl,
    SemanticsRole role = SemanticsRole.none,
    required List<String>? controlsNodes,
    SemanticsValidationResult validationResult = SemanticsValidationResult.none,
    ui.SemanticsHitTestBehavior hitTestBehavior =
        ui.SemanticsHitTestBehavior.defer,
    required ui.SemanticsInputType inputType,
    required ui.Locale? locale,
    required String minValue,
    required String maxValue,
  }) {
    changed.add(id);
    owner.nodes[id] = (
      label: label,
      parent: traversalParent,
      traversal: List<int>.of(childrenInTraversalOrder),
      hit: List<int>.of(childrenInHitTestOrder),
    );
  }

  @override
  void updateCustomAction({
    required int id,
    String? label,
    String? hint,
    int overrideId = -1,
  }) => _builder.updateCustomAction(
    id: id,
    label: label,
    hint: hint,
    overrideId: overrideId,
  );
  @override
  ui.SemanticsUpdate build() {
    owner.verify(changed);
    return _builder.build();
  }
}

void main() {
  final binding = _WireBinding();
  testWidgets(
    'HeaderMenu tooltip and menu serialize reachable traversal graphs',
    (tester) async {
      final handle = tester.ensureSemantics();
      final focus = FocusNode();
      final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
      try {
        binding.nodes.clear();
        binding.failures.clear();
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: Align(
                alignment: Alignment.topRight,
                child: HeaderMenu(focusNode: focus, onSelected: (_) {}),
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final trigger = find.byKey(const Key('menu-trigger'));
        void checkTrigger(bool expanded) {
          final data = tester.getSemantics(trigger).getSemanticsData();
          expect(
            data.label,
            contains(expanded ? 'Close Ryhze menu' : 'Ryhze menu'),
          );
          expect(data.flagsCollection.isButton, isTrue);
          expect(data.hasAction(ui.SemanticsAction.tap), isTrue);
          expect(
            data.flagsCollection.isExpanded,
            expanded ? ui.Tristate.isTrue : ui.Tristate.isFalse,
          );
        }

        checkTrigger(false);
        await mouse.addPointer(location: Offset.zero);
        await mouse.moveTo(tester.getCenter(trigger));
        await tester.pump(const Duration(seconds: 1));
        await tester.pumpAndSettle();
        expect(find.text('Ryhze menu'), findsOneWidget);
        expect(binding.failures, isEmpty, reason: 'tooltip show');
        checkTrigger(false);
        await tester.tap(trigger);
        await tester.pumpAndSettle();
        expect(find.byType(MenuItemButton), findsNWidgets(4));
        expect(binding.failures, isEmpty, reason: 'menu open');
        checkTrigger(true);
        await tester.tap(trigger);
        await tester.pumpAndSettle();
        expect(find.byType(MenuItemButton), findsNothing);
        checkTrigger(false);
        await mouse.moveTo(Offset.zero);
        Tooltip.dismissAllToolTips();
        await tester.pumpAndSettle();
        expect(find.text('Ryhze menu'), findsNothing);
        expect(
          binding.failures,
          isEmpty,
          reason: 'menu close and tooltip removal',
        );
        expect(
          binding.nodes.values.any((node) => node.label.contains('Ryhze menu')),
          isTrue,
        );
        expect(tester.takeException(), isNull);
      } finally {
        await mouse.removePointer();
        await tester.pumpWidget(const SizedBox());
        focus.dispose();
        handle.dispose();
      }
    },
  );
}
