import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';

import '../../core/app_failure.dart';
import '../../domain/activity.dart';
import '../../domain/app_route.dart';
import '../../features/activity/activity_repository.dart';
import '../../features/session/session_controller.dart';

enum WindowsNotificationResult { submitted, coalesced, unavailable }

/// Native availability requires this executable's installed shortcut and URI
/// registration. Submission never proves delivery, user attention or job success.
class WindowsNotificationsClient {
  WindowsNotificationsClient._(this._channel, this.available);
  final MethodChannel _channel;
  final bool available;

  static Future<WindowsNotificationsClient?> connect({
    MethodChannel channel = const MethodChannel(
      'com.ryhze.ryhze/windows.notifications.v1',
    ),
  }) async {
    try {
      final value = await channel
          .invokeMethod<Object?>('probe')
          .timeout(const Duration(seconds: 2));
      if (value is! Map ||
          value.length != 2 ||
          value['schemaVersion'] is! int ||
          value['schemaVersion'] != 1 ||
          value['notifications'] is! bool) {
        return null;
      }
      return WindowsNotificationsClient._(
        channel,
        value['notifications'] as bool,
      );
    } catch (_) {
      return null;
    }
  }

  Future<WindowsNotificationResult> submit({
    required String id,
    required ActivityKind kind,
    required AppRoute route,
    required bool privatePreview,
  }) async {
    final uri = route.toUri();
    if (id.isEmpty ||
        utf8.encode(id).length > 8192 ||
        RegExp(r'[\x00-\x1f\x7f]').hasMatch(id) ||
        AppRoute.parse(uri) == null) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (!available) return WindowsNotificationResult.unavailable;
    final state = await _invoke('show', {
      'id': id,
      'kind': kind.name,
      'uri': uri.toString(),
      'private': privatePreview,
    });
    final result = WindowsNotificationResult.values
        .where((v) => v.name == state)
        .firstOrNull;
    if (result == null) throw const AppFailure(FailureKind.invalidData);
    return result;
  }

  /// Clear is still attempted when notifications are disabled by Windows: old
  /// private previews may remain in history even when new banners are disabled.
  Future<bool> clear() async {
    final state = await _invoke('clear');
    if (state != 'cleared' && state != 'unavailable') {
      throw const AppFailure(FailureKind.invalidData);
    }
    return state == 'cleared';
  }

  Future<String> _invoke(String method, [Map<String, Object>? args]) async {
    try {
      final value = await _channel
          .invokeMethod<Object?>(method, args)
          .timeout(const Duration(seconds: 2));
      if (value is! Map || value.length != 1 || value['state'] is! String) {
        throw const AppFailure(FailureKind.invalidData);
      }
      return value['state'] as String;
    } on PlatformException {
      throw const AppFailure(FailureKind.unsupported);
    } on MissingPluginException {
      throw const AppFailure(FailureKind.unsupported);
    } on TimeoutException {
      throw const AppFailure(FailureKind.unsupported);
    }
  }
}

/// Optional account-scoped projection. Null-route events remain Activity-only;
/// there is no invented fallback destination or command/action payload.
class WindowsNotificationsBinding {
  WindowsNotificationsBinding(
    this.client,
    this.activity,
    this.session, {
    this._enabled = false,
    bool privatePreviews = true,
  }) : _private = privatePreviews {
    activity.addListener(_refresh);
    session.addListener(_scopeChanged);
    _reset();
  }
  final WindowsNotificationsClient client;
  final ActivityRepository activity;
  final SessionController session;
  bool _enabled, _private, _closed = false, _mustClear = false;
  final Map<String, String> _seen = {};
  final Map<String, ActivityRecord> _queued = {};
  Future<void>? _sending, _closing;

  void configure({required bool enabled, required bool privatePreviews}) {
    if (_closed || (_enabled == enabled && _private == privatePreviews)) return;
    _enabled = enabled;
    _private = privatePreviews;
    _reset();
  }

  void _scopeChanged() {
    if (!_closed) _reset();
  }

  static String _signature(ActivityRecord record) =>
      '${record.occurredAt.toUtc().toIso8601String()}:${record.count}';

  void _reset() {
    _queued.clear();
    _seen.clear();
    for (final record in activity.read(session.scope)) {
      _seen[record.id] = _signature(record);
    }
    // Startup also clears a previous account's surviving previews. A toast that
    // launched this process has already handed its validated route to activation.
    _mustClear = true;
    _startDrain();
  }

  void _refresh() {
    if (_closed) return;
    final records = activity.read(session.scope);
    final present = records.map((r) => r.id).toSet();
    _seen.removeWhere((id, _) => !present.contains(id));
    _queued.removeWhere((id, _) => !present.contains(id));
    for (final record in records.reversed) {
      final signature = _signature(record);
      final changed = _seen[record.id] != signature;
      _seen[record.id] = signature;
      if (record.acknowledged) {
        _queued.remove(record.id);
        continue;
      }
      if (!changed ||
          !_enabled ||
          record.route == null ||
          AppRoute.parse(record.route!.toUri()) == null) {
        continue;
      }
      _queued[record.id] = record;
      while (_queued.length > 16) {
        _queued.remove(_queued.keys.first);
      }
    }
    _startDrain();
  }

  void _startDrain() {
    if (_sending != null || (!_mustClear && (_closed || _queued.isEmpty))) {
      return;
    }
    _sending = _drain().whenComplete(() {
      _sending = null;
      if (_mustClear || (!_closed && _queued.isNotEmpty)) _startDrain();
    });
  }

  Future<void> _drain() async {
    while (_mustClear || (!_closed && _queued.isNotEmpty)) {
      if (_mustClear) {
        _mustClear = false;
        try {
          await client.clear();
        } catch (_) {
          /* Optional shell failure. */
        }
        continue;
      }
      final record = _queued.remove(_queued.keys.first)!;
      if (!_enabled) continue;
      try {
        await client.submit(
          id: jsonEncode([session.scope.key, record.id]),
          kind: record.kind,
          route: record.route!,
          privatePreview: _private,
        );
      } catch (_) {
        // No retry loop, job mutation or Activity acknowledgement on OS failure.
      }
    }
  }

  Future<void> close() => _closing ??= _close();
  Future<void> _close() async {
    _closed = true;
    activity.removeListener(_refresh);
    session.removeListener(_scopeChanged);
    _queued.clear();
    // Join the admitted call and any pending privacy/account clear. Normal exit
    // does not remove already-submitted notifications or change durable records.
    _startDrain();
    while (_sending != null) {
      await _sending;
    }
  }
}
