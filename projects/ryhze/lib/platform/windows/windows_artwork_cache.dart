import 'dart:async';

import 'package:flutter/services.dart';

import '../../core/app_failure.dart';
import '../../providers/public_artwork_cache.dart';
import '../../providers/public_artwork_png.dart';

/// The native adapter chooses its private root. Only opaque keys and inert,
/// bounded bytes cross this channel; no caller-supplied filesystem paths.
class WindowsArtworkCache implements PublicArtworkCache {
  WindowsArtworkCache({MethodChannel? channel})
    : _channel =
          channel ?? const MethodChannel('com.ryhze.ryhze/windows.artwork.v1');
  final MethodChannel _channel;
  static final _key = RegExp(r'^[a-f0-9]{64}$');
  static const _maxTime = 4102444800000;
  static Future<WindowsArtworkCache?> connect({MethodChannel? channel}) async {
    final cache = WindowsArtworkCache(channel: channel);
    try {
      final result = await cache._call('probe');
      return result is Map &&
              result.length == 2 &&
              result['schemaVersion'] is int &&
              result['schemaVersion'] == 1 &&
              result['cache'] == true
          ? cache
          : null;
    } catch (_) {
      return null;
    }
  }

  static void _validateKey(String key) {
    if (key.length != 64 || !_key.hasMatch(key)) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  static bool _number(Object? value, int maximum) =>
      value is int && value >= 0 && value <= maximum;
  Future<Object?> _call(String method, [Map<String, Object>? args]) async {
    try {
      return await _channel
          .invokeMethod<Object?>(method, args)
          .timeout(const Duration(seconds: 35));
    } on AppFailure {
      rethrow;
    } on PlatformException catch (error) {
      throw AppFailure(switch (error.code) {
        'invalidData' || 'integrity' => FailureKind.invalidData,
        'conflict' => FailureKind.conflict,
        'cancelled' => FailureKind.cancelled,
        'unavailable' => FailureKind.unsupported,
        _ => FailureKind.storage,
      });
    } catch (_) {
      throw const AppFailure(FailureKind.storage);
    }
  }

  @override
  Future<PublicArtworkCached?> get(String key) async {
    _validateKey(key);
    final result = await _call('get', {'key': key});
    if (result == null) return null;
    if (result is! Map ||
        result.length != 2 ||
        result['bytes'] is! Uint8List ||
        (result['bytes'] as Uint8List).isEmpty ||
        (result['bytes'] as Uint8List).length > publicArtworkMaximumBytes ||
        !_number(result['storedAtMs'], _maxTime)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return PublicArtworkCached(
      result['bytes'] as Uint8List,
      DateTime.fromMillisecondsSinceEpoch(
        result['storedAtMs'] as int,
        isUtc: true,
      ),
    );
  }

  @override
  Future<void> put(String key, Uint8List bytes, DateTime storedAt) async {
    _validateKey(key);
    if (bytes.isEmpty ||
        bytes.length > publicArtworkMaximumBytes ||
        !_number(storedAt.millisecondsSinceEpoch, _maxTime)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final result = await _call('put', {
      'key': key,
      'bytes': Uint8List.fromList(bytes),
      'storedAtMs': storedAt.millisecondsSinceEpoch,
    });
    if (result != null) throw const AppFailure(FailureKind.invalidData);
  }

  @override
  Future<PublicArtworkCacheHealth> health() async {
    final result = await _call('health');
    if (result is! Map ||
        result.length != 3 ||
        !_number(result['entries'], 64) ||
        !_number(result['bytes'], 64 * 1024 * 1024) ||
        !_number(result['preserved'], 128)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return PublicArtworkCacheHealth(
      entries: result['entries'] as int,
      bytes: result['bytes'] as int,
      preserved: result['preserved'] as int,
    );
  }

  @override
  Future<PublicArtworkClearResult> clear() async {
    final result = await _call('clear');
    if (result is! Map ||
        result.length != 2 ||
        !_number(result['removed'], 64) ||
        !_number(result['preserved'], 128)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return PublicArtworkClearResult(
      removed: result['removed'] as int,
      preserved: result['preserved'] as int,
    );
  }

  @override
  Future<void> drain() async {
    // A timeout is failure, never acknowledgement that a native write stopped.
    final result = await _call('drain');
    if (result != null) throw const AppFailure(FailureKind.invalidData);
  }
}
