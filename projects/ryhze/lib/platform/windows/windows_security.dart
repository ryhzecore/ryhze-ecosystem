import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';

import '../../core/app_failure.dart';
import '../../features/session/session_controller.dart';
import '../desktop_contracts.dart';

/// This channel conveys opaque credential bytes only to the current-user vault.
/// Availability does not authenticate an account or establish ownership.
class WindowsSecurityClient {
  WindowsSecurityClient._(this._channel, this.secureCredentials);
  final MethodChannel _channel;
  final bool secureCredentials;

  static Future<WindowsSecurityClient?> connect({
    MethodChannel channel = const MethodChannel(
      'com.ryhze.ryhze/windows.security.v1',
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
          value['secureCredentials'] is! bool ||
          value['fileIdentity'] != true) {
        return null;
      }
      return WindowsSecurityClient._(
        channel,
        value['secureCredentials'] as bool,
      );
    } catch (_) {
      return null;
    }
  }

  static void _namespace(AccountScope scope) {
    if (scope.isLocal) throw const AppFailure(FailureKind.authentication);
    for (final value in [scope.authority, scope.accountId]) {
      if (value.isEmpty ||
          utf8.encode(value).length > 512 ||
          utf8.decode(utf8.encode(value)) != value ||
          RegExp(r'[\x00-\x1f]').hasMatch(value)) {
        throw const AppFailure(FailureKind.invalidData);
      }
    }
  }

  Future<Object?> _invoke(String method, Map<String, Object> args) async {
    try {
      return await _channel.invokeMethod<Object?>(method, args);
    } on PlatformException catch (error) {
      throw AppFailure(switch (error.code) {
        'denied' => FailureKind.denied,
        'invalidData' => FailureKind.invalidData,
        'recovery' => FailureKind.recovery,
        'conflict' => FailureKind.conflict,
        _ => FailureKind.unsupported,
      });
    } on MissingPluginException {
      throw const AppFailure(FailureKind.unsupported);
    }
  }

  Future<Object?> _credential(
    String method,
    AccountScope scope, [
    Uint8List? value,
  ]) {
    _namespace(scope);
    if (!secureCredentials) throw const AppFailure(FailureKind.unsupported);
    return _invoke(method, {
      'provider': scope.authority,
      'account': scope.accountId,
      'value': ?value,
    });
  }

  Future<Uint8List?> read(AccountScope scope) async {
    final value = await _credential('readCredential', scope);
    if (value == null) return null;
    if (value is! Uint8List || value.isEmpty || value.length > 65536) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return value;
  }

  Future<void> write(AccountScope scope, Uint8List value) async {
    if (value.isEmpty || value.length > 65536) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final result = await _credential('writeCredential', scope, value);
    if (result != null) throw const AppFailure(FailureKind.invalidData);
  }

  Future<void> delete(AccountScope scope) async {
    final result = await _credential('deleteCredential', scope);
    if (result != null) throw const AppFailure(FailureKind.invalidData);
  }

  /// Identity is an observation only. A future executor must reopen and validate
  /// its own handles immediately before an authorized effect.
  Future<WindowsFileIdentity> inspectFile(String root, String relative) async {
    if (!WindowsPathPolicy.validRoot(root) ||
        !WindowsPathPolicy.validRelative(relative)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final value = await _invoke('inspectFile', {
      'root': root,
      'relative': relative,
    });
    if (value is! Map ||
        value.length != 4 ||
        value['identity'] is! Uint8List ||
        (value['identity'] as Uint8List).length != 16 ||
        value['volume'] is! String ||
        !RegExp(r'^\d{1,20}$').hasMatch(value['volume'] as String) ||
        value['directory'] is! bool ||
        value['links'] is! int ||
        (value['links'] as int) < 1) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return WindowsFileIdentity(
      value['volume'] as String,
      Uint8List.fromList(value['identity'] as Uint8List),
      value['directory'] as bool,
      value['links'] as int,
    );
  }
}

class WindowsFileIdentity {
  WindowsFileIdentity(this.volume, this.id, this.directory, this.links);
  final String volume;
  final Uint8List id;
  final bool directory;
  final int links;
}

/// Session-owned serialization, cancellation and stale-completion suppression.
/// There is no token field in observable application state.
class WindowsCredentials {
  WindowsCredentials(this.session, this.client);
  final SessionController session;
  final WindowsSecurityClient client;
  Future<void> _pending = Future.value();
  bool _closed = false;
  void dispose() {
    _closed = true;
  }

  Future<T> _scoped<T>(Future<T> Function(AccountScope) operation) {
    if (_closed) return Future.error(const AppFailure(FailureKind.cancelled));
    final scope = session.scope;
    final generation = session.generation;
    final token = session.beginWork();
    final result = _pending.then((_) async {
      try {
        token.check();
        if (_closed || generation != session.generation) {
          throw const AppFailure(FailureKind.cancelled);
        }
        final value = await operation(scope);
        if (_closed || token.isCancelled || generation != session.generation) {
          if (value is Uint8List) value.fillRange(0, value.length, 0);
          throw const AppFailure(FailureKind.cancelled);
        }
        return value;
      } finally {
        session.finishWork(token);
      }
    });
    _pending = result.then<void>((_) {}, onError: (Object _, StackTrace _) {});
    return result;
  }

  Future<Uint8List?> read() => _scoped(client.read);
  Future<void> write(Uint8List value) async {
    final copy = Uint8List.fromList(value);
    try {
      await _scoped((scope) => client.write(scope, copy));
    } finally {
      copy.fillRange(0, copy.length, 0);
    }
  }

  Future<void> delete() => _scoped(client.delete);
  Future<void> get settled => _pending;
}
