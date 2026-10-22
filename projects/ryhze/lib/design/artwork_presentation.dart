import 'dart:async';

import 'package:flutter/material.dart';

import '../core/app_failure.dart';
import 'artwork.dart';
import 'artwork_reference.dart';
import 'artwork_source.dart';

/// The composition owner supplies the source; design has no provider knowledge.
class ArtworkScope extends InheritedWidget {
  const ArtworkScope({super.key, required this.source, required super.child});

  final ArtworkSource? source;

  static ArtworkSource? of(BuildContext context) =>
      context.dependOnInheritedWidgetOfExactType<ArtworkScope>()?.source;

  @override
  bool updateShouldNotify(ArtworkScope oldWidget) => source != oldWidget.source;
}

typedef ArtworkPresentationBuilder = Widget Function(
  BuildContext context,
  Widget image,
  String caption,
  VoidCallback? retry,
);

/// Keeps asynchronous artwork and its description together without introducing
/// controls inside a clickable card. Callers choose where a retry can be placed.
class ArtworkPresentation extends StatefulWidget {
  const ArtworkPresentation({
    super.key,
    required this.reference,
    required this.fallbackCaption,
    required this.builder,
    this.cinema = false,
    this.fallback,
  });

  final ArtworkReference? reference;
  final String fallbackCaption;
  final ArtworkPresentationBuilder builder;
  final bool cinema;
  final Widget? fallback;

  @override
  State<ArtworkPresentation> createState() => _ArtworkPresentationState();
}

enum _ArtworkPhase { fallback, loading, ready, unavailable, cancelled, hidden }

class _ArtworkPresentationState extends State<ArtworkPresentation> {
  ArtworkSource? _source;
  ArtworkReference? _reference;
  CancellationToken? _token;
  ArtworkRead? _read;
  ResizeImage? _imageProvider;
  int _epoch = 0;
  _ArtworkPhase _phase = _ArtworkPhase.fallback;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final source = ArtworkScope.of(context);
    if (!identical(source, _source)) {
      _source?.changes.removeListener(_invalidate);
      _source = source;
      _source?.changes.addListener(_invalidate);
      _start();
    } else if (_reference != widget.reference) {
      _start();
    }
  }

  @override
  void didUpdateWidget(ArtworkPresentation oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.reference != widget.reference) _start();
  }

  void _evict() {
    final provider = _imageProvider;
    _imageProvider = null;
    if (provider != null) unawaited(provider.evict());
  }

  void _reset() {
    _epoch++;
    _token?.cancel();
    _token = null;
    _read = null;
    _evict();
  }

  void _start() {
    _reset();
    _reference = widget.reference;
    final reference = _reference;
    final source = _source;
    if (reference == null) {
      _phase = _ArtworkPhase.fallback;
      return;
    }
    if (source == null) {
      _phase = _ArtworkPhase.unavailable;
      return;
    }
    _phase = _ArtworkPhase.loading;
    final epoch = _epoch;
    final token = _token = CancellationToken();
    unawaited(
      Future<ArtworkRead>.sync(() => source.read(reference, token)).then(
        (read) {
          if (!mounted || epoch != _epoch || token.isCancelled) return;
          setState(() {
            _read = read;
          });
        },
        onError: (Object error, StackTrace stack) {
          if (!mounted || epoch != _epoch || token.isCancelled) return;
          setState(() {
            _phase = error is AppFailure && error.kind == FailureKind.cancelled
                ? _ArtworkPhase.cancelled
                : _ArtworkPhase.unavailable;
          });
        },
      ),
    );
  }

  void _invalidate() {
    if (!mounted) return;
    setState(() {
      _reset();
      _phase = _reference == null
          ? _ArtworkPhase.fallback
          : _ArtworkPhase.hidden;
    });
    // Clearing or interrupting a source must not immediately refill it.
  }

  void _retry() => setState(_start);

  void _decodeFailed(ArtworkRead read, int epoch) {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || epoch != _epoch || !identical(read, _read)) return;
      setState(() {
        _reset();
        _phase = _ArtworkPhase.unavailable;
      });
    });
  }

  void _frameReady(ArtworkRead read, int epoch) {
    if (_phase == _ArtworkPhase.ready) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || epoch != _epoch || !identical(read, _read)) return;
      if (_phase != _ArtworkPhase.ready) {
        setState(() => _phase = _ArtworkPhase.ready);
      }
    });
  }

  @override
  void dispose() {
    _source?.changes.removeListener(_invalidate);
    _reset();
    super.dispose();
  }

  String get _caption => switch (_phase) {
    _ArtworkPhase.fallback => widget.fallbackCaption,
    _ArtworkPhase.loading => '${_reference!.note} · Loading artwork',
    _ArtworkPhase.ready =>
      _read!.stale
          ? '${_reference!.note} · Offline artwork may be out of date'
          : _reference!.note,
    _ArtworkPhase.unavailable => 'Artwork unavailable · Placeholder shown',
    _ArtworkPhase.cancelled => 'Artwork paused · Placeholder shown',
    _ArtworkPhase.hidden => 'Artwork hidden · Reopen or reload to display',
  };

  @override
  Widget build(BuildContext context) {
    final read = _read;
    final epoch = _epoch;
    final fallback = widget.fallback ?? ProjectArtwork(cinema: widget.cinema);
    final image = read == null
        ? fallback
        : RepaintBoundary(
            child: ExcludeSemantics(
              child: LayoutBuilder(
                builder: (context, constraints) {
                  final ratio = MediaQuery.devicePixelRatioOf(context);
                  int bound(double dimension) => dimension.isFinite
                      ? (dimension * ratio).ceil().clamp(1, 2048)
                      : 2048;
                  final width = bound(constraints.maxWidth);
                  final height = bound(constraints.maxHeight);
                  if (_imageProvider?.width != width ||
                      _imageProvider?.height != height) {
                    _evict();
                    _imageProvider = ResizeImage(
                      MemoryImage(read.bytes),
                      width: width,
                      height: height,
                      policy: ResizeImagePolicy.fit,
                    );
                  }
                  return Stack(
                    fit: StackFit.expand,
                    children: [
                      Image(
                        key: const Key('public-artwork-image'),
                        image: _imageProvider!,
                        width: double.infinity,
                        height: double.infinity,
                        fit: BoxFit.cover,
                        filterQuality: FilterQuality.medium,
                        gaplessPlayback: false,
                        frameBuilder: (context, child, frame, synchronous) {
                          if (frame == null) return fallback;
                          _frameReady(read, epoch);
                          return child;
                        },
                        errorBuilder: (context, error, stack) {
                          _decodeFailed(read, epoch);
                          return fallback;
                        },
                      ),
                      const DecoratedBox(
                        decoration: BoxDecoration(
                          gradient: LinearGradient(
                            begin: Alignment.topCenter,
                            end: Alignment.bottomCenter,
                            colors: [Colors.transparent, Color(0xD9000000)],
                          ),
                        ),
                      ),
                    ],
                  );
                },
              ),
            ),
          );
    return widget.builder(
      context,
      image,
      _caption,
      _source != null &&
              _reference != null &&
              _phase != _ArtworkPhase.loading &&
              (_phase != _ArtworkPhase.ready || _read!.stale)
          ? _retry
          : null,
    );
  }
}
