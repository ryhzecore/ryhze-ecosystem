import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

import 'host_goldens.dart';
export 'host_goldens.dart' show installHostGoldens;

/// Observes settled render state before strict comparison, without pumping,
/// changing layout, consuming framework exceptions or updating references.
/// Explicit CI capture defers golden assertion failures to mandatory teardown.
Future<void> expectGoldenWithDiagnostics(
  WidgetTester tester,
  Finder target,
  String goldenPath, {
  Map<String, String>? environment,
}) async {
  final env = environment ?? Platform.environment;
  if (env['RYHZE_GOLDEN_DIAGNOSTICS'] == '1') {
    try {
      final snapshot = _snapshot(tester, target, goldenPath);
      final encoded = const JsonEncoder.withIndent('  ').convert(snapshot);
      final writeFailure = await tester.runAsync<String>(() async {
        try {
          // Preserve the entire relative key: equal basenames in different golden
          // directories must never overwrite one another's diagnostics.
          final segments = goldenPath.split('/');
          if (segments.first != 'goldens' ||
              segments.any(
                (part) =>
                    !RegExp(r'^[A-Za-z0-9._-]+$').hasMatch(part) ||
                    part == '.' ||
                    part == '..',
              )) {
            throw ArgumentError('Invalid diagnostic golden path');
          }
          final root =
              env['RYHZE_GOLDEN_DIAGNOSTICS_ROOT'] ?? 'test/golden-diagnostics';
          final file = File('$root/$goldenPath.json');
          await file.parent.create(recursive: true);
          await file.writeAsString('$encoded\n', flush: true);
          return '';
        } catch (error) {
          // runAsync reports escaping errors to the binding instead of throwing
          // them to our outer catch. Handle only this optional diagnostic work
          // here; never drain the binding's pending framework exceptions.
          return error.runtimeType.toString();
        }
      });
      if (writeFailure != null && writeFailure.isNotEmpty) {
        debugPrint(
          'Golden diagnostics unavailable: $goldenPath ($writeFailure)',
        );
      }
    } catch (error) {
      // Diagnostics must not replace or prevent the original pixel assertion.
      // Print only the fixed reference path and failure type, never host errors.
      debugPrint(
        'Golden diagnostics unavailable: $goldenPath (${error.runtimeType})',
      );
    }
  }
  await compareHostGolden(tester, target, goldenPath);
}

Map<String, Object?> _snapshot(
  WidgetTester tester,
  Finder target,
  String goldenPath,
) {
  final keyed = <Map<String, Object?>>[];
  final paragraphs = <Map<String, Object?>>[];
  final mediaQueries = <Map<String, Object?>>[];
  final seen = <RenderParagraph>{};
  var elements = 0;
  var capped = false;
  for (final element in tester.allElements) {
    if (elements >= 20000) {
      capped = true;
      break;
    }
    final index = elements++;
    if (element.widget case MediaQuery(:final data)) {
      mediaQueries.add({'elementIndex': index, ..._media(data)});
    }
    if (element.widget.key == null && element.widget is! RichText) continue;
    final render = element.findRenderObject();
    final identity = <String, Object?>{
      'elementIndex': index,
      'widgetType': element.widget.runtimeType.toString(),
      if (element.widget.key != null) 'key': _key(element.widget.key!),
    };
    if (element.widget.key != null) {
      if (keyed.length < 4096) {
        keyed.add({...identity, ..._geometry(render)});
      } else {
        capped = true;
      }
    }
    if (render is! RenderParagraph || !seen.add(render)) continue;
    if (paragraphs.length >= 2048) {
      capped = true;
      continue;
    }
    final text = render.text.toPlainText(
      includeSemanticsLabels: false,
      includePlaceholders: true,
    );
    final ancestors = <Object?>[];
    element.visitAncestorElements((ancestor) {
      if (ancestor.widget.key != null) {
        ancestors.add(_key(ancestor.widget.key!));
      }
      return ancestors.length < 8;
    });
    final record = <String, Object?>{
      ...identity,
      ..._geometry(render),
      'ancestorKeys': ancestors,
      'utf16Length': text.length,
      if (text.length <= 65536) 'text': text else 'textOmittedOverLimit': true,
      'style': _style(render.text.style),
      'inlineSpans': _spans(render.text, 0),
      'softWrap': render.softWrap,
      'overflow': render.overflow.name,
      'maxLines': render.maxLines,
      'textAlign': render.textAlign.name,
      'textDirection': render.textDirection.name,
      'textWidthBasis': render.textWidthBasis.name,
      'locale': render.locale?.toLanguageTag(),
      'textScaler': render.textScaler.runtimeType.toString(),
      'scaled16': _number(render.textScaler.scale(16)),
    };
    if (render.attached &&
        render.hasSize &&
        !render.debugNeedsLayout &&
        text.length <= 65536) {
      record['textSize'] = _size(render.textSize);
      record['didExceedMaxLines'] = render.didExceedMaxLines;
      final transform = render.getTransformTo(null);
      // These are actual laid-out selection fragments. Bidi text/inline spans
      // can produce multiple fragments per line; count is NOT a line count.
      record['selectionBoxes'] = [
        for (final box in render.getBoxesForSelection(
          TextSelection(baseOffset: 0, extentOffset: text.length),
        ))
          {
            'direction': box.direction.name,
            'localRect': _rect(box.toRect()),
            'globalRect': _rect(
              MatrixUtils.transformRect(transform, box.toRect()),
            ),
          },
      ];
    }
    paragraphs.add(record);
  }
  final targetElement = target.evaluate().single;
  final media = targetElement.getInheritedWidgetOfExactType<MediaQuery>()?.data;
  return {
    'schema': 1,
    'goldenKey': goldenPath,
    'goldenPath': 'test/$goldenPath',
    'testDescription': tester.testDescription,
    'host': {
      'os': Platform.operatingSystem,
      'osVersion': Platform.operatingSystemVersion,
      'dartVersion': Platform.version,
      'targetPlatformOverride': defaultTargetPlatform.name,
    },
    'viewport': {
      'physicalSize': _size(tester.view.physicalSize),
      'devicePixelRatio': _number(tester.view.devicePixelRatio),
      'logicalSize': _size(
        tester.view.physicalSize / tester.view.devicePixelRatio,
      ),
      if (media != null) 'captureAncestorMediaQuery': _media(media),
    },
    // A capture boundary may wrap a fixture MediaQuery. Record those contexts
    // explicitly; the boundary's ancestor scale is not necessarily text scale.
    'mediaQueries': mediaQueries,
    'captureTarget': _geometry(targetElement.findRenderObject()),
    'elementsVisited': elements,
    'collectionCapped': capped,
    'notes':
        'Coordinates are global logical pixels, including offscreen and retained widgets. '
        'Paint suppression is observed along render ancestry; bounds alone do not prove visibility or absence of clipping. '
        'Selection boxes are visual fragments, not inferred line counts. '
        'Nonfinite numbers are named strings, not JSON numbers.',
    'keyedWidgets': keyed,
    'paragraphs': paragraphs,
  };
}

Map<String, Object?> _geometry(RenderObject? render) {
  if (render == null) return {'available': false};
  final result = <String, Object?>{
    'renderType': render.runtimeType.toString(),
    'attached': render.attached,
    'needsLayout': render.debugNeedsLayout,
  };
  if (render is! RenderBox ||
      !render.attached ||
      !render.hasSize ||
      render.debugNeedsLayout) {
    result['available'] = false;
    return result;
  }
  result['available'] = true;
  result['size'] = _size(render.size);
  result['globalRect'] = _rect(
    MatrixUtils.transformRect(
      render.getTransformTo(null),
      Offset.zero & render.size,
    ),
  );
  final suppressed = <String>[];
  RenderObject current = render;
  while (current.parent != null) {
    final parent = current.parent!;
    var painted = parent.paintsChild(current);
    // This pinned RenderIndexedStack retains laid-out children and does not
    // override paintsChild. Identify its selected child without changing state.
    if (parent is RenderIndexedStack) {
      var selected = parent.firstChild;
      for (
        var index = 0;
        selected != null && index < (parent.index ?? -1);
        index++
      ) {
        selected = parent.childAfter(selected);
      }
      painted = parent.index != null && identical(selected, current);
    }
    if (!painted) {
      suppressed.add(parent.runtimeType.toString());
    }
    current = parent;
  }
  result['paintSuppressedBy'] = suppressed;
  return result;
}

Object? _key(Key key) {
  if (key is ValueKey) {
    final value = key.value;
    return {
      'type': key.runtimeType.toString(),
      'value': value is String || value is bool || value == null
          ? value
          : value is num
          ? _number(value)
          : value.runtimeType.toString(),
    };
  }
  return {'type': key.runtimeType.toString()};
}

Map<String, Object?>? _style(TextStyle? style) => style == null
    ? null
    : {
        'fontFamily': style.fontFamily,
        'fontFamilyFallback': style.fontFamilyFallback,
        'fontSize': style.fontSize == null ? null : _number(style.fontSize!),
        'fontWeight': style.fontWeight?.value,
        'fontStyle': style.fontStyle?.name,
        'height': style.height == null ? null : _number(style.height!),
        'letterSpacing': style.letterSpacing == null
            ? null
            : _number(style.letterSpacing!),
        'wordSpacing': style.wordSpacing == null
            ? null
            : _number(style.wordSpacing!),
        'leadingDistribution': style.leadingDistribution?.name,
        'fontVariations': [
          for (final value in style.fontVariations ?? [])
            {'axis': value.axis, 'value': _number(value.value)},
        ],
        'fontFeatures': [
          for (final value in style.fontFeatures ?? [])
            {'feature': value.feature, 'value': value.value},
        ],
      };

Map<String, Object?> _spans(InlineSpan span, int depth) => {
  'type': span.runtimeType.toString(),
  'style': _style(span.style),
  if (span is TextSpan) ...{
    if ((span.text?.length ?? 0) <= 65536) 'text': span.text,
    if (depth < 16)
      'children': [
        for (final child in (span.children ?? []).take(256))
          _spans(child, depth + 1),
      ],
  },
};

Object _number(num value) => value.isFinite ? value : value.toString();
Map<String, Object> _size(Size size) => {
  'width': _number(size.width),
  'height': _number(size.height),
};
Map<String, Object> _rect(Rect rect) => {
  'left': _number(rect.left),
  'top': _number(rect.top),
  'right': _number(rect.right),
  'bottom': _number(rect.bottom),
};
Map<String, Object> _insets(EdgeInsets insets) => {
  'left': _number(insets.left),
  'top': _number(insets.top),
  'right': _number(insets.right),
  'bottom': _number(insets.bottom),
};

Map<String, Object?> _media(MediaQueryData data) => {
  'size': _size(data.size),
  'scaled16': _number(data.textScaler.scale(16)),
  'padding': _insets(data.padding),
  'viewPadding': _insets(data.viewPadding),
  'viewInsets': _insets(data.viewInsets),
  'disableAnimations': data.disableAnimations,
};
