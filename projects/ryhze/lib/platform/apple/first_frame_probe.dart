import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Opt-in debug simulator evidence. This is not visual or device acceptance.
/// Release/profile builds and every other platform perform no channel calls.
final class FirstFrameProbe {
  FirstFrameProbe._(
    this._channel,
    this._nonce,
    this._processID,
    this._waitForRaster,
    this._rasterTimeout,
  );

  static const channelName = 'com.ryhze.ryhze/first-frame-probe.v1';
  static const failureCodes = {
    'bootstrap-degraded',
    'bootstrap-failed',
    'flutter-error',
    'async-error',
    'shell-layout',
    'raster-timeout',
    'probe-failed',
  };
  static const _landmarks = {
    'app-chrome-scroll',
    'catalog-scroll',
    'hero-surface',
    'destination-store',
    'destination-library',
  };

  static Future<FirstFrameProbe?> connect({
    MethodChannel channel = const MethodChannel(channelName),
    Future<void> Function()? waitForRaster,
    Duration rasterTimeout = const Duration(seconds: 60),
  }) async {
    if (!kDebugMode || kIsWeb || defaultTargetPlatform != TargetPlatform.iOS) {
      return null;
    }
    try {
      final value = await channel
          .invokeMethod<Object?>('configuration')
          .timeout(const Duration(seconds: 2));
      if (value is! Map ||
          value.length != 3 ||
          value['schemaVersion'] != 1 ||
          value['schemaVersion'] is! int ||
          value['nonce'] is! String ||
          !RegExp(r'^[a-f0-9]{32}$').hasMatch(value['nonce'] as String) ||
          value['processID'] is! int ||
          (value['processID'] as int) <= 0 ||
          (value['processID'] as int) > 0x7fffffff) {
        return null;
      }
      return FirstFrameProbe._(
        channel,
        value['nonce'] as String,
        value['processID'] as int,
        waitForRaster ??
            () => WidgetsBinding.instance.waitUntilFirstFrameRasterized,
        rasterTimeout,
      );
    } catch (_) {
      // No opt-in channel on ordinary debug/device launches. A requested probe
      // that cannot communicate cannot produce the host's matching ready file.
      return null;
    }
  }

  final MethodChannel _channel;
  final String _nonce;
  final int _processID;
  final Future<void> Function() _waitForRaster;
  final Duration _rasterTimeout;
  Future<void> _pending = Future.value();
  final _stopped = Completer<void>();
  bool _failed = false;
  bool _disposed = false;
  bool _healthyBootstrap = false;
  bool _layoutStarted = false;
  FlutterExceptionHandler? _previousFlutterError;
  FlutterExceptionHandler? _flutterHandler;
  bool Function(Object, StackTrace)? _previousAsyncError;
  bool Function(Object, StackTrace)? _asyncHandler;

  void installErrorHandlers() {
    if (_disposed || _flutterHandler != null) return;
    _previousFlutterError = FlutterError.onError;
    _previousAsyncError = PlatformDispatcher.instance.onError;
    _flutterHandler = (details) {
      unawaited(fail('flutter-error'));
      _previousFlutterError?.call(details);
    };
    _asyncHandler = (error, stack) {
      unawaited(fail('async-error'));
      // Preserve the prior handling decision; a probe does not consume errors.
      return _previousAsyncError?.call(error, stack) ?? false;
    };
    FlutterError.onError = _flutterHandler;
    PlatformDispatcher.instance.onError = _asyncHandler;
  }

  Future<void> bootstrapCompleted({required bool healthy}) {
    if (_disposed || _failed) return _pending;
    _healthyBootstrap = healthy;
    if (!healthy) {
      _failed = true;
      _stopWaiting();
    }
    return _send(
      healthy ? 'bootstrap-healthy' : 'bootstrap-degraded',
      failureCode: healthy ? null : 'bootstrap-degraded',
    );
  }

  Future<void> fail(String code) {
    if (!failureCodes.contains(code)) {
      throw ArgumentError('Unknown probe failure code');
    }
    if (_disposed || _failed) return _pending;
    _failed = true;
    _stopWaiting();
    return _send('failed', failureCode: code);
  }

  Future<void> _send(String event, {String? failureCode}) {
    _pending = _pending.then((_) async {
      if (_disposed ||
          (_failed && event != 'failed' && event != 'bootstrap-degraded')) {
        return;
      }
      final message = {
        'nonce': _nonce,
        'processID': _processID,
        'event': event,
        'failureCode': failureCode,
      };
      try {
        await _channel
            .invokeMethod<void>('update', message)
            .timeout(const Duration(seconds: 2));
      } catch (_) {
        _failed = true;
        _stopWaiting();
        // A failed transport can only fail the opt-in check, never readiness.
        // Make one bounded best-effort terminal report; never include an error.
        try {
          await _channel
              .invokeMethod<void>('update', {
                'nonce': _nonce,
                'processID': _processID,
                'event': 'failed',
                'failureCode': 'probe-failed',
              })
              .timeout(const Duration(seconds: 2));
        } catch (_) {}
      }
    });
    return _pending;
  }

  Widget wrap(Widget child) => _ProbeShell(probe: this, child: child);

  Future<void> _afterLayout(BuildContext context) async {
    if (_disposed || _failed || _layoutStarted) return;
    _layoutStarted = true;
    if (!_healthyBootstrap) {
      await fail('bootstrap-failed');
      return;
    }
    try {
      final view = View.of(context);
      final viewport =
          Offset.zero & (view.physicalSize / view.devicePixelRatio);
      final found = <String>{};
      void visit(Element element) {
        final widget = element.widget;
        if ((widget is Offstage && widget.offstage) ||
            (widget is Visibility && !widget.visible) ||
            (widget is Opacity && widget.opacity == 0)) {
          return;
        }
        final key = widget.key;
        if (key is ValueKey<String> && _landmarks.contains(key.value)) {
          final box = element.findRenderObject();
          if (box is RenderBox && box.attached && box.hasSize) {
            final rect = box.localToGlobal(Offset.zero) & box.size;
            if (rect.isFinite &&
                rect.width > 0 &&
                rect.height > 0 &&
                rect.overlaps(viewport)) {
              found.add(key.value);
            }
          }
        }
        element.debugVisitOnstageChildren(visit);
      }

      (context as Element).debugVisitOnstageChildren(visit);
      if (!found.containsAll(_landmarks)) {
        await fail('shell-layout');
        return;
      }
      await _send('shell-layout');
      if (_disposed || _failed) return;
      await Future.any([_waitForRaster(), _stopped.future])
          .timeout(_rasterTimeout);
      if (!_disposed && !_failed) await _send('first-frame-rasterized');
    } on TimeoutException {
      await fail('raster-timeout');
    } catch (_) {
      await fail('probe-failed');
    }
  }

  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _stopWaiting();
    if (identical(FlutterError.onError, _flutterHandler)) {
      FlutterError.onError = _previousFlutterError;
    }
    if (identical(PlatformDispatcher.instance.onError, _asyncHandler)) {
      PlatformDispatcher.instance.onError = _previousAsyncError;
    }
  }

  void _stopWaiting() {
    if (!_stopped.isCompleted) _stopped.complete();
  }
}

class _ProbeShell extends StatefulWidget {
  const _ProbeShell({required this.probe, required this.child});
  final FirstFrameProbe probe;
  final Widget child;

  @override
  State<_ProbeShell> createState() => _ProbeShellState();
}

class _ProbeShellState extends State<_ProbeShell> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) unawaited(widget.probe._afterLayout(context));
    });
  }

  @override
  void dispose() {
    widget.probe.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => widget.child;
}
