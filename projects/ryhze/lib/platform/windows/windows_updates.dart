import 'dart:async';

import 'package:flutter/services.dart';

import '../../core/app_failure.dart';

enum WindowsUpdateStage {
  unavailable,
  invalid,
  untrusted,
  staged,
  recovery,
  conflict,
}

class WindowsUpdateResult {
  const WindowsUpdateResult(this.state, this.bytes);
  final WindowsUpdateStage state;
  final int bytes;
}

/// Staging and inspection never activate an update. Publisher trust is fixed in
/// the native composition, never accepted from metadata, links or Dart calls.
class WindowsUpdateClient {
  WindowsUpdateClient._(this._channel, this.stagingAvailable);
  final MethodChannel _channel;
  final bool stagingAvailable;
  bool _closed = false, _suspended = false, _running = false;
  int _generation = 0;
  Future<void>? _draining, _closing;
  static const maximumBytes = 16 * 1024 * 1024;
  static Future<WindowsUpdateClient?> connect({
    MethodChannel channel = const MethodChannel(
      'com.ryhze.ryhze/windows.updates.v1',
    ),
  }) async {
    try {
      final value = await channel
          .invokeMethod<Object?>('probe')
          .timeout(const Duration(seconds: 2));
      if (value is! Map ||
          value.length != 3 ||
          value['schemaVersion'] is! int ||
          value['schemaVersion'] != 1 ||
          value['staging'] is! bool ||
          value['activation'] != false) {
        return null;
      }
      return WindowsUpdateClient._(channel, value['staging'] as bool);
    } catch (_) {
      return null;
    }
  }

  Future<WindowsUpdateResult> _invoke(
    String method,
    Uint8List hash,
    int size,
    CancellationToken cancellation, [
    Uint8List? bytes,
  ]) async {
    cancellation.check();
    _checkAdmission();
    if (hash.length != 32 || size < 1 || size > maximumBytes) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (!stagingAvailable) {
      return const WindowsUpdateResult(WindowsUpdateStage.unavailable, 0);
    }
    if (_running) throw const AppFailure(FailureKind.conflict);
    _running = true;
    final generation = _generation;
    final remove = cancellation.onCancel(() {
      unawaited(cancel().catchError((Object _) {}));
    });
    try {
      final value = await _channel
          .invokeMethod<Object?>(method, {
            'sha256': Uint8List.fromList(hash),
            if (bytes != null) 'bytes': bytes else 'size': size,
          })
          .timeout(const Duration(seconds: 35));
      // Cancellation after a write is an unknown completion. Durable staging is
      // retained and must be inspected, never automatically executed/replayed.
      cancellation.check();
      if (_closed || _suspended || generation != _generation) {
        throw const AppFailure(FailureKind.cancelled);
      }
      if (value is! Map ||
          value.length != 2 ||
          value['state'] is! String ||
          value['bytes'] is! int ||
          (value['bytes'] as int) < 0 ||
          (value['bytes'] as int) > maximumBytes) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final state = WindowsUpdateStage.values
          .where((s) => s.name == value['state'])
          .firstOrNull;
      if (state == null ||
          (state == WindowsUpdateStage.staged && value['bytes'] != size)) {
        throw const AppFailure(FailureKind.invalidData);
      }
      return WindowsUpdateResult(state, value['bytes'] as int);
    } on PlatformException catch (error) {
      throw _failure(error);
    } on MissingPluginException {
      throw const AppFailure(FailureKind.unsupported);
    } on TimeoutException {
      // A missing reply is not evidence of worker exit or rollback.
      interrupt();
      throw const AppFailure(FailureKind.recovery);
    } finally {
      remove();
      _running = false;
    }
  }

  Future<WindowsUpdateResult> stage(
    Uint8List bytes,
    Uint8List expectedSha256,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    _checkAdmission();
    if (bytes.isEmpty ||
        bytes.length > maximumBytes ||
        expectedSha256.length != 32) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (!stagingAvailable) {
      return const WindowsUpdateResult(WindowsUpdateStage.unavailable, 0);
    }
    return _invoke(
      'stage',
      expectedSha256,
      bytes.length,
      cancellation,
      Uint8List.fromList(bytes),
    );
  }

  Future<WindowsUpdateResult> inspect(
    Uint8List expectedSha256,
    int expectedSize,
    CancellationToken cancellation,
  ) => _invoke('inspect', expectedSha256, expectedSize, cancellation);

  void _checkAdmission() {
    if (_closed || _suspended) throw const AppFailure(FailureKind.cancelled);
    if (_draining != null) throw const AppFailure(FailureKind.conflict);
  }

  static AppFailure _failure(PlatformException error) =>
      AppFailure(switch (error.code) {
        'cancelled' => FailureKind.cancelled,
        'recovery' => FailureKind.recovery,
        'conflict' => FailureKind.conflict,
        'invalidData' => FailureKind.invalidData,
        _ => FailureKind.unsupported,
      });

  Future<void> _control(String method) async {
    if (!stagingAvailable) return;
    try {
      final value = await _channel.invokeMethod<Object?>(method);
      if (value != null) throw const AppFailure(FailureKind.invalidData);
    } on PlatformException catch (error) {
      throw _failure(error);
    } on MissingPluginException {
      throw const AppFailure(FailureKind.unsupported);
    }
  }

  /// Acknowledges a cancellation request, not native settlement or rollback.
  Future<void> cancel() {
    _generation++;
    return _control('cancel');
  }

  /// Shares the real native settlement barrier. No timeout can manufacture a
  /// successful drain while an OS trust-provider call still owns native files.
  Future<void> drain() =>
      _draining ??= _control('drain')
          .catchError((Object error) {
            _suspended = true;
            throw error;
          })
          .whenComplete(() => _draining = null);

  void interrupt() {
    _suspended = true;
    unawaited(cancel().catchError((Object _) {}));
  }

  Future<void> recoverAfterResume() async {
    if (_closed) throw const AppFailure(FailureKind.cancelled);
    _suspended = true;
    final generation = _generation;
    await drain();
    if (_closed || generation != _generation) {
      // Settlement does not validate a recovery invalidated by newer work.
      // The application owner must keep its coordinator suspended as well.
      throw const AppFailure(FailureKind.cancelled);
    }
    _suspended = false;
  }

  Future<void> close() {
    _closed = true;
    _generation++;
    return _closing ??= _control('close');
  }
}
