import 'dart:async';

import 'package:flutter/services.dart';

import '../../core/app_failure.dart';
import '../../domain/contracts.dart';
import '../../features/installations/installation_repository.dart';

/// Only the native adapter selects registry/library roots. Dart sends no paths,
/// credentials, executable names or ownership requests through this channel.
class WindowsInstallationRepository implements InstallationRepository {
  WindowsInstallationRepository({
    this._channel = const MethodChannel(
      'com.ryhze.ryhze/windows.installations.v1',
    ),
    DateTime Function()? clock,
  }) : _clock = clock ?? DateTime.now;

  final MethodChannel _channel;
  final DateTime Function() _clock;
  bool _running = false;

  static Future<WindowsInstallationRepository?> connect({
    MethodChannel channel = const MethodChannel(
      'com.ryhze.ryhze/windows.installations.v1',
    ),
    DateTime Function()? clock,
  }) async {
    final repository = WindowsInstallationRepository(
      channel: channel,
      clock: clock,
    );
    return await repository.probe() ? repository : null;
  }

  /// Checks the read-only adapter contract without scanning registry or files.
  /// Supported means inspection is available, not that Steam is installed.
  Future<bool> probe() async {
    try {
      final value = await _channel
          .invokeMethod<Object?>('probe')
          .timeout(const Duration(seconds: 2));
      return value is Map &&
          value.length == 4 &&
          value['schemaVersion'] is int &&
          value['schemaVersion'] == 1 &&
          value['readOnlyInventory'] == true &&
          value['ownership'] == false &&
          value['launch'] == false;
    } catch (_) {
      return false;
    }
  }

  static final _controls = RegExp(r'[\x00-\x1f\x7f-\x9f]');
  static bool _number(String value, {bool allowZero = false}) {
    final number = int.tryParse(value);
    return RegExp(r'^(0|[1-9][0-9]{0,9})$').hasMatch(value) &&
        number != null &&
        number <= 0xffffffff &&
        (allowZero || number > 0);
  }

  static String _text(Object? value, int limit) {
    if (value is! String ||
        value.trim().isEmpty ||
        value.length > limit ||
        _controls.hasMatch(value)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return value;
  }

  @override
  Future<RepositoryRead<InstallationObservation>> inspect(
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    if (_running) throw const AppFailure(FailureKind.conflict);
    _running = true;
    var requested = false;
    final removeListener = cancellation.onCancel(() {
      if (requested) {
        unawaited(
          _channel.invokeMethod<void>('cancel').catchError((Object _) {}),
        );
      }
    });
    try {
      final supported = await probe();
      cancellation.check();
      if (!supported) throw const AppFailure(FailureKind.unsupported);
      requested = true;
      final value = await _channel
          .invokeMethod<Object?>('inspectSteam')
          .timeout(const Duration(seconds: 17));
      cancellation.check();
      if (value is! Map ||
          value.length != 2 ||
          value['items'] is! List ||
          (value['items'] as List).length > 1000) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final quality = switch (value['quality']) {
        'complete' => ReadQuality.complete,
        'partial' => ReadQuality.partial,
        'unavailable' => ReadQuality.unavailable,
        _ => throw const AppFailure(FailureKind.invalidData),
      };
      final ids = <String>{};
      final observations = <InstallationObservation>[];
      for (final raw in value['items'] as List) {
        if (raw is! Map || raw.length != 5) {
          throw const AppFailure(FailureKind.invalidData);
        }
        final id = _text(raw['appId'], 10);
        final state = _text(raw['stateFlags'], 10);
        final root = _text(raw['libraryRoot'], 32767);
        final relative = _text(raw['relativeDirectory'], 300);
        if (!_number(id) ||
            !ids.add(id) ||
            !_number(state, allowZero: true) ||
            !RegExp(r'^[A-Za-z]:\\').hasMatch(root) ||
            root
                .substring(3)
                .split('\\')
                .any(
                  (part) =>
                      part.isEmpty ||
                      part == '.' ||
                      part == '..' ||
                      part.contains(':'),
                ) ||
            !relative.startsWith('steamapps\\common\\') ||
            relative.substring(17).isEmpty ||
            relative.substring(17).contains(RegExp(r'[\\/:]')) ||
            relative.substring(17) == '.' ||
            relative.substring(17) == '..') {
          throw const AppFailure(FailureKind.invalidData);
        }
        observations.add(
          InstallationObservation(
            item: ProviderItemRef('steam', id),
            title: _text(raw['title'], 512),
            libraryRoot: root,
            relativeDirectory: relative,
            providerState: state,
          ),
        );
      }
      if (quality == ReadQuality.unavailable && observations.isNotEmpty) {
        throw const AppFailure(FailureKind.invalidData);
      }
      return RepositoryRead(
        observations,
        quality: quality,
        observedAt: _clock().toUtc(),
        failure: quality == ReadQuality.complete
            ? null
            : const AppFailure(FailureKind.unsupported),
      );
    } on AppFailure {
      rethrow;
    } on PlatformException catch (error) {
      cancellation.check();
      throw AppFailure(switch (error.code) {
        'cancelled' => FailureKind.cancelled,
        'conflict' => FailureKind.conflict,
        'invalidData' => FailureKind.invalidData,
        _ => FailureKind.unsupported,
      });
    } on TimeoutException {
      if (requested) {
        unawaited(
          _channel.invokeMethod<void>('cancel').catchError((Object _) {}),
        );
      }
      cancellation.check();
      throw const AppFailure(FailureKind.unsupported);
    } on MissingPluginException {
      throw const AppFailure(FailureKind.unsupported);
    } finally {
      removeListener();
      _running = false;
    }
  }
}
