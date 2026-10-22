import 'dart:async';

import 'package:flutter/services.dart';

import '../../core/app_failure.dart';
import '../../domain/activity.dart';
import '../../features/activity/activity_repository.dart';
import '../../features/jobs/job_repository.dart';
import '../../features/session/session_controller.dart';

enum WindowsTaskbarState { none, indeterminate, attention }

enum WindowsTaskbarResult { applied, queued, unavailable }

/// Shell status only. No title, path, account identifier or job action crosses
/// this boundary, and an accepted update is not a claim the shell displayed it.
class WindowsTaskbarClient {
  WindowsTaskbarClient._(this._channel, this.available);
  final MethodChannel _channel;
  final bool available;

  static Future<WindowsTaskbarClient?> connect({
    MethodChannel channel = const MethodChannel(
      'com.ryhze.ryhze/windows.taskbar.v1',
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
          value['progress'] is! bool) {
        return null;
      }
      return WindowsTaskbarClient._(channel, value['progress'] as bool);
    } catch (_) {
      return null;
    }
  }

  Future<WindowsTaskbarResult> update(WindowsTaskbarState state) async {
    if (!available) return WindowsTaskbarResult.unavailable;
    try {
      final value = await _channel
          .invokeMethod<Object?>('set', {'state': state.name})
          .timeout(const Duration(seconds: 2));
      if (value is! Map || value.length != 1 || value['state'] is! String) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final result = WindowsTaskbarResult.values
          .where((v) => v.name == value['state'])
          .firstOrNull;
      if (result == null) throw const AppFailure(FailureKind.invalidData);
      return result;
    } on PlatformException {
      throw const AppFailure(FailureKind.unsupported);
    } on MissingPluginException {
      throw const AppFailure(FailureKind.unsupported);
    } on TimeoutException {
      throw const AppFailure(FailureKind.unsupported);
    }
  }
}

/// Account-scoped projection of authoritative job/activity records. Existing
/// checkpoints have no total, so they must never be converted into a percentage.
class WindowsTaskbarBinding {
  WindowsTaskbarBinding(this.client, this.jobs, this.activity, this.session) {
    jobs.addListener(_refresh);
    activity.addListener(_refresh);
    session.addListener(_refresh);
    _refresh();
  }
  final WindowsTaskbarClient client;
  final JobRepository jobs;
  final ActivityRepository activity;
  final SessionController session;
  bool _suspended = false, _closed = false;
  WindowsTaskbarState _desired = WindowsTaskbarState.none;
  WindowsTaskbarState? _sent;
  Future<void>? _sending, _closing;

  static WindowsTaskbarState project(
    List<JobRecord> jobs,
    List<ActivityRecord> activity, {
    bool suspended = false,
  }) {
    if (suspended) return WindowsTaskbarState.none;
    if (activity.any(
      (event) =>
          !event.acknowledged &&
          {
            ActivityKind.failed,
            ActivityKind.recovery,
            ActivityKind.blocked,
          }.contains(event.kind),
    )) {
      return WindowsTaskbarState.attention;
    }
    if (jobs.any(
      (job) => job.state == JobState.running || job.state == JobState.queued,
    )) {
      return WindowsTaskbarState.indeterminate;
    }
    return WindowsTaskbarState.none;
  }

  void setSuspended(bool value) {
    if (_closed || _suspended == value) return;
    _suspended = value;
    _refresh();
  }

  void _refresh() {
    if (_closed) return;
    _desired = project(
      jobs.read(session.scope),
      activity.read(session.scope),
      suspended: _suspended,
    );
    if (_sending != null || _sent == _desired) return;
    _sending = _drain().whenComplete(() {
      _sending = null;
      if (!_closed && _sent != _desired) _refresh();
    });
  }

  Future<void> _drain() async {
    while (!_closed && _sent != _desired) {
      final state = _desired;
      try {
        await client.update(state);
      } catch (_) {
        // Optional shell failure cannot fail a job or poison the durable store.
      }
      _sent = state;
    }
  }

  Future<void> close() => _closing ??= _close();
  Future<void> _close() async {
    _closed = true;
    jobs.removeListener(_refresh);
    activity.removeListener(_refresh);
    session.removeListener(_refresh);
    await _sending;
    try {
      await client.update(WindowsTaskbarState.none);
    } catch (_) {
      // Native Detach clears the indicator too, before the HWND is destroyed.
    }
  }
}
