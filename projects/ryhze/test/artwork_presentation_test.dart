import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/design/artwork_presentation.dart';
import 'package:ryhze/design/artwork_reference.dart';
import 'package:ryhze/design/artwork_source.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/store_sections.dart';
import 'package:ryhze/features/entertainment/widgets/title_detail.dart';

class _Request {
  _Request(this.reference, this.token);
  final ArtworkReference reference;
  final CancellationToken token;
  final completer = Completer<ArtworkRead>();
}

class _Changes extends ValueNotifier<int> {
  _Changes() : super(0);
  bool get listening => hasListeners;
}

class _Source implements ArtworkSource {
  @override
  final _Changes changes = _Changes();
  final requests = <_Request>[];
  @override
  Future<ArtworkRead> read(
    ArtworkReference reference,
    CancellationToken token,
  ) {
    final request = _Request(reference, token);
    requests.add(request);
    return request.completer.future;
  }

  void invalidate() => changes.value++;
}

final _reference = ArtworkReference(
  itemId: 'catalogue-title',
  revision: 3,
  path: '/art/test-concept.png',
  note: 'World concept artwork · Not gameplay',
);

final _revised = ArtworkReference(
  itemId: 'catalogue-title',
  revision: 4,
  path: '/art/test-concept.png',
  note: 'Revised concept artwork · Not gameplay',
);

final _title = EntertainmentTitle(
  id: 'catalogue-title',
  title: 'Catalogue title',
  description: 'An exact public catalogue description.',
  tags: const ['Adventure'],
  artwork: _reference,
);

Widget _harness(
  _Source? source, {
  ArtworkReference? reference,
  double width = 320,
  double scale = 1,
}) => MaterialApp(
  theme: RyhzeTokens.theme,
  home: Scaffold(
    body: MediaQuery(
      data: MediaQueryData(textScaler: TextScaler.linear(scale)),
      child: ArtworkScope(
        source: source,
        child: Center(
          child: SizedBox(
            width: width,
            child: ArtworkPresentation(
              reference: reference ?? _reference,
              fallbackCaption: 'Placeholder artwork',
              builder: (context, image, caption, retry) => Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  AspectRatio(
                    key: const Key('art-bounds'),
                    aspectRatio: 16 / 9,
                    child: image,
                  ),
                  Text(caption),
                  if (retry != null)
                    TextButton(onPressed: retry, child: const Text('Retry')),
                ],
              ),
            ),
          ),
        ),
      ),
    ),
  ),
);

Future<Uint8List> _png(WidgetTester tester) async =>
    (await tester.runAsync(() async {
      final recorder = ui.PictureRecorder();
      final canvas = Canvas(recorder);
      canvas.drawColor(const Color(0xFF648CC8), BlendMode.src);
      final picture = recorder.endRecording();
      final image = await picture.toImage(160, 90);
      picture.dispose();
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      image.dispose();
      return bytes!.buffer.asUint8List();
    }))!;

ArtworkRead _read(Uint8List bytes, {bool stale = false}) => ArtworkRead(
  bytes: bytes,
  storedAt: DateTime.utc(2026, 10, 11),
  origin: stale ? ArtworkOrigin.disk : ArtworkOrigin.network,
  stale: stale,
);

Future<void> _renderImage(WidgetTester tester) async {
  // Decoder callbacks run outside the fake test clock. Wait for an actual
  // decoded frame, not just absence of scheduled Flutter animation frames.
  final watch = Stopwatch()..start();
  while (watch.elapsed < const Duration(seconds: 3)) {
    await tester.pump();
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 10)),
    );
    final rendered = find.descendant(
      of: find.byKey(const Key('public-artwork-image')),
      matching: find.byWidgetPredicate(
        (widget) => widget is RawImage && widget.image != null,
      ),
    );
    if (rendered.evaluate().isNotEmpty &&
        find.textContaining('Loading artwork').evaluate().isEmpty) {
      return;
    }
  }
  fail('Artwork did not produce a decoded visible frame within three seconds.');
}

void main() {
  testWidgets('loading and ready retain geometry and truthful attribution', (
    tester,
  ) async {
    final source = _Source();
    final bytes = await _png(tester);
    await tester.pumpWidget(_harness(source));
    final bounds = tester.getSize(find.byKey(const Key('art-bounds')));
    expect(find.textContaining('Loading artwork'), findsOneWidget);
    expect(source.requests.single.reference, _reference);
    source.requests.single.completer.complete(_read(bytes));
    await _renderImage(tester);
    expect(find.text(_reference.note), findsOneWidget);
    expect(tester.getSize(find.byKey(const Key('art-bounds'))), bounds);
    final image = tester.widget<Image>(
      find.byKey(const Key('public-artwork-image')),
    );
    expect(image.fit, BoxFit.cover);
    final provider = image.image as ResizeImage;
    expect(provider.width, 320);
    expect(provider.height, 180);
    expect(provider.policy, ResizeImagePolicy.fit);
    expect(image.gaplessPlayback, isFalse);
    expect(tester.takeException(), isNull);
  });

  testWidgets('saved stale artwork is visibly distinct from a fresh read', (
    tester,
  ) async {
    final source = _Source();
    final bytes = await _png(tester);
    await tester.pumpWidget(_harness(source));
    source.requests.single.completer.complete(_read(bytes, stale: true));
    await _renderImage(tester);
    expect(
      find.textContaining('Offline artwork may be out of date'),
      findsOneWidget,
    );
    expect(find.textContaining('Not gameplay'), findsOneWidget);
    await tester.tap(find.text('Retry'));
    await tester.pump();
    expect(source.requests, hasLength(2));
    expect(find.textContaining('Loading artwork'), findsOneWidget);
  });

  testWidgets('read failure shows closed copy and permits an explicit retry', (
    tester,
  ) async {
    final source = _Source();
    await tester.pumpWidget(_harness(source));
    source.requests.single.completer.completeError(
      StateError('private provider exception must not be displayed'),
    );
    await tester.pumpAndSettle();
    expect(find.textContaining('Artwork unavailable'), findsOneWidget);
    expect(find.textContaining('private provider'), findsNothing);
    await tester.tap(find.text('Retry'));
    await tester.pump();
    expect(source.requests, hasLength(2));
    expect(find.textContaining('Loading artwork'), findsOneWidget);
    expect(find.text('Retry'), findsNothing);
  });

  testWidgets('cancelled source read remains a truthful paused placeholder', (
    tester,
  ) async {
    final source = _Source();
    await tester.pumpWidget(_harness(source));
    source.requests.single.completer.completeError(
      const AppFailure(FailureKind.cancelled),
    );
    await tester.pumpAndSettle();
    expect(find.textContaining('Artwork paused'), findsOneWidget);
    expect(find.byKey(const Key('public-artwork-image')), findsNothing);
  });

  testWidgets('invalidation cancels pending work and rejects its late result', (
    tester,
  ) async {
    final source = _Source();
    final bytes = await _png(tester);
    await tester.pumpWidget(_harness(source));
    final old = source.requests.single;
    source.invalidate();
    await tester.pump();
    expect(old.token.isCancelled, isTrue);
    expect(find.textContaining('Artwork hidden'), findsOneWidget);
    old.completer.complete(_read(bytes));
    await tester.pumpAndSettle();
    await tester.pumpWidget(_harness(source, width: 360));
    expect(find.byKey(const Key('public-artwork-image')), findsNothing);
    expect(source.requests, hasLength(1));
    await tester.tap(find.text('Retry'));
    await tester.pump();
    expect(source.requests, hasLength(2));
    source.requests.last.completer.complete(_read(bytes));
    await _renderImage(tester);
    expect(find.byKey(const Key('public-artwork-image')), findsOneWidget);
  });

  testWidgets('clearing removes displayed bytes without immediately fetching', (
    tester,
  ) async {
    final source = _Source();
    final bytes = await _png(tester);
    await tester.pumpWidget(_harness(source));
    source.requests.single.completer.complete(_read(bytes));
    await _renderImage(tester);
    final provider = tester
        .widget<Image>(find.byKey(const Key('public-artwork-image')))
        .image;
    final cacheKey = await provider.obtainKey(ImageConfiguration.empty);
    source.invalidate();
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('public-artwork-image')), findsNothing);
    expect(find.textContaining('Artwork hidden'), findsOneWidget);
    expect(source.requests, hasLength(1));
    expect(PaintingBinding.instance.imageCache.containsKey(cacheKey), isFalse);
  });

  testWidgets('replacement reference cannot display an older completion', (
    tester,
  ) async {
    final source = _Source();
    final bytes = await _png(tester);
    await tester.pumpWidget(_harness(source));
    final old = source.requests.single;
    await tester.pumpWidget(_harness(source, reference: _revised));
    expect(old.token.isCancelled, isTrue);
    expect(source.requests, hasLength(2));
    expect(source.requests.last.reference, _revised);
    old.completer.complete(_read(bytes));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('public-artwork-image')), findsNothing);
    expect(find.textContaining(_revised.note), findsOneWidget);
    source.requests.last.completer.complete(_read(bytes));
    await _renderImage(tester);
    expect(find.text(_revised.note), findsOneWidget);
  });

  testWidgets('scope replacement detaches the old source and its listeners', (
    tester,
  ) async {
    final oldSource = _Source();
    final source = _Source();
    final bytes = await _png(tester);
    await tester.pumpWidget(_harness(oldSource));
    await tester.pumpWidget(_harness(source));
    expect(oldSource.requests.single.token.isCancelled, isTrue);
    oldSource.invalidate();
    oldSource.requests.single.completer.complete(_read(bytes));
    source.requests.single.completer.complete(_read(bytes));
    await _renderImage(tester);
    expect(find.text(_reference.note), findsOneWidget);
    expect(source.requests, hasLength(1));
    expect(oldSource.changes.listening, isFalse);
  });

  testWidgets('disposal cancels work and ignores both completion and events', (
    tester,
  ) async {
    final source = _Source();
    await tester.pumpWidget(_harness(source));
    await tester.pumpWidget(const SizedBox());
    expect(source.requests.single.token.isCancelled, isTrue);
    expect(source.changes.listening, isFalse);
    source.invalidate();
    source.requests.single.completer.completeError(
      const AppFailure(FailureKind.cancelled),
    );
    await tester.pump();
    expect(tester.takeException(), isNull);
  });

  testWidgets('unavailable scope keeps fallback without a dead retry control', (
    tester,
  ) async {
    await tester.pumpWidget(_harness(null));
    expect(find.textContaining('Artwork unavailable'), findsOneWidget);
    expect(find.text('Retry'), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('resize bounds decode allocation and never refetches artwork', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(4096, 3000);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final source = _Source();
    final bytes = await _png(tester);
    await tester.pumpWidget(_harness(source, width: 4000));
    source.requests.single.completer.complete(_read(bytes));
    await _renderImage(tester);
    var provider =
        tester
                .widget<Image>(find.byKey(const Key('public-artwork-image')))
                .image
            as ResizeImage;
    expect(provider.width, 2048);
    expect(provider.height, 2048);
    await tester.pumpWidget(_harness(source, width: 360, scale: 2));
    await tester.pumpAndSettle();
    provider =
        tester
                .widget<Image>(find.byKey(const Key('public-artwork-image')))
                .image
            as ResizeImage;
    expect(provider.width, 360);
    expect(provider.height, 203);
    expect(source.requests, hasLength(1));
    expect(tester.takeException(), isNull);
  });

  testWidgets('corrupt renderer data falls back without losing the caption', (
    tester,
  ) async {
    final source = _Source();
    await tester.pumpWidget(_harness(source));
    source.requests.single.completer.complete(_read(Uint8List(4)));
    await tester.pump();
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 30)),
    );
    await tester.pumpAndSettle();
    expect(find.textContaining('Artwork unavailable'), findsOneWidget);
    expect(find.byKey(const Key('public-artwork-image')), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('Store art preserves title actions, keyboard access and 2x fit', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(360, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final source = _Source();
    final favourite = ValueNotifier(false);
    addTearDown(favourite.dispose);
    var opened = 0;
    var saved = 0;
    await tester.pumpWidget(
      MaterialApp(
        theme: RyhzeTokens.theme,
        home: MediaQuery(
          data: const MediaQueryData(
            size: Size(360, 800),
            textScaler: TextScaler.linear(2),
          ),
          child: ArtworkScope(
            source: source,
            child: Scaffold(
              body: SingleChildScrollView(
                child: Column(
                  children: [
                    StoreHero(
                      width: 360,
                      gutter: 20,
                      category: EntertainmentCategory.games,
                      title: _title,
                      favourite: favourite,
                      onFavourite: () => saved++,
                      onExplore: () => opened++,
                    ),
                    GameTile(title: _title, onOpen: () => opened++),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
    for (final request in source.requests) {
      request.completer.completeError(const AppFailure(FailureKind.network));
    }
    await tester.pumpAndSettle();
    expect(find.text('Catalogue title'), findsNWidgets(2));
    expect(find.textContaining('Artwork unavailable'), findsNWidgets(2));
    final bounds = tester.getSize(find.byKey(const Key('tile-art-16-9')));
    expect(bounds.width / bounds.height, closeTo(16 / 9, .001));
    expect(find.byKey(const Key('reload-artwork')), findsNothing);
    await tester.tap(find.byKey(const Key('hero-favourite')));
    await tester.pumpAndSettle();
    expect(saved, 1);
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pumpAndSettle();
    expect(opened, 1);
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'detail has a separate accessible retry and unchanged back action',
    (tester) async {
      tester.view.physicalSize = const Size(360, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final source = _Source();
      final favourite = ValueNotifier(false);
      addTearDown(favourite.dispose);
      await tester.pumpWidget(
        MaterialApp(
          theme: RyhzeTokens.theme,
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context)
                .copyWith(textScaler: const TextScaler.linear(2)),
            child: ArtworkScope(source: source, child: child!),
          ),
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                onPressed: () => showDialog<void>(
                  context: context,
                  builder: (context) => TitleDetail(
                    title: _title,
                    favourite: favourite,
                    onFavourite: () => favourite.value = !favourite.value,
                  ),
                ),
                child: const Text('Open detail'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('Open detail'));
      await tester.pumpAndSettle();
      source.requests.single.completer.completeError(
        const AppFailure(FailureKind.network),
      );
      await tester.pumpAndSettle();
      final reload = find.byKey(const Key('reload-artwork'));
      await tester.ensureVisible(reload);
      await tester.pumpAndSettle();
      expect(tester.getSize(reload).height, greaterThanOrEqualTo(48));
      final backRect = tester.getRect(find.byKey(const Key('detail-back')));
      expect(backRect.top, greaterThanOrEqualTo(0));
      await tester.tap(reload);
      await tester.pump();
      expect(source.requests, hasLength(2));
      expect(find.textContaining('Loading artwork'), findsOneWidget);
      await tester.tap(find.byKey(const Key('detail-back')));
      await tester.pumpAndSettle();
      expect(source.requests.last.token.isCancelled, isTrue);
      expect(find.text('Open detail'), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );
}
