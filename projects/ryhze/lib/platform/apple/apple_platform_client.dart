import 'dart:convert';

import 'package:flutter/services.dart';

import '../../core/app_failure.dart';
import '../../data/store_schema.dart';
import '../../data/transaction_store.dart';
import '../../features/session/session_controller.dart';

/// Native error details never become user messages or diagnostics.
final class ApplePlatformException implements Exception {
  const ApplePlatformException(this.code);
  final String code;
  AppFailure get failure => AppFailure(switch (code) {
    'denied' || 'locked' => FailureKind.denied,
    'conflict' => FailureKind.conflict,
    'invalidArguments' => FailureKind.invalidData,
    'corruptState' || 'unsupportedSchema' => FailureKind.recovery,
    _ => FailureKind.storage,
  });
  @override
  String toString() => failure.message;
}

final class AppleBridgeCapabilities {
  const AppleBridgeCapabilities({required this.platform});
  final String platform;
}

final class AppleStoreSnapshot {
  const AppleStoreSnapshot({
    required this.revision,
    required this.data,
    this.sourceBytes,
  });
  final int revision;
  final StoreData data;

  /// The exact stored bytes, retained for an atomic legacy migration backup.
  final Uint8List? sourceBytes;
}

/// Raw UTF-8 bytes preserve exact shared dictionary keys through Swift, whose
/// String equality otherwise folds canonically equivalent Unicode spellings.
class ApplePlatformClient {
  const ApplePlatformClient({
    this.channel = const MethodChannel('com.ryhze.ryhze/apple.v1'),
  });
  static const maximumSnapshotBytes = 4 * 1024 * 1024;
  final MethodChannel channel;

  Future<Object?> _invoke(String method, [Map<String, Object?>? args]) async {
    try {
      return await channel.invokeMethod<Object?>(method, args);
    } on MissingPluginException {
      throw const ApplePlatformException('unavailable');
    } on PlatformException catch (error) {
      const known = {
        'invalidArguments',
        'unavailable',
        'locked',
        'denied',
        'conflict',
        'corruptState',
        'unsupportedSchema',
        'ioFailure',
      };
      throw ApplePlatformException(
        known.contains(error.code) ? error.code : 'unavailable',
      );
    }
  }

  static Map<String, Object?> _object(Object? value, Set<String> keys) {
    if (value is! Map ||
        value.length != keys.length ||
        !value.keys.every(keys.contains)) {
      throw const FormatException();
    }
    return Map<String, Object?>.from(value);
  }

  Future<AppleBridgeCapabilities> bootstrap() async {
    final value = await _invoke('bootstrap');
    try {
      final map = _object(value, {
        'schemaVersion',
        'platform',
        'secureCredentials',
        'durableState',
      });
      if (map['schemaVersion'] is! int ||
          map['schemaVersion'] != 1 ||
          !{'ios', 'macos'}.contains(map['platform']) ||
          map['secureCredentials'] != true ||
          map['durableState'] != true) {
        throw const FormatException();
      }
      return AppleBridgeCapabilities(platform: map['platform']! as String);
    } catch (_) {
      throw const ApplePlatformException('unavailable');
    }
  }

  static void _validUnicode(String value) {
    for (var i = 0; i < value.length; i++) {
      final unit = value.codeUnitAt(i);
      if (unit >= 0xd800 && unit <= 0xdbff) {
        if (++i >= value.length ||
            value.codeUnitAt(i) < 0xdc00 ||
            value.codeUnitAt(i) > 0xdfff) {
          throw const AppFailure(FailureKind.invalidData);
        }
      } else if (unit >= 0xdc00 && unit <= 0xdfff) {
        throw const AppFailure(FailureKind.invalidData);
      }
    }
  }

  static void _validateStrings(Object? value, [int depth = 0]) {
    if (depth > 64) throw const AppFailure(FailureKind.invalidData);
    if (value is String) {
      _validUnicode(value);
    } else if (value is Map) {
      for (final entry in value.entries) {
        if (entry.key is! String) {
          throw const AppFailure(FailureKind.invalidData);
        }
        _validUnicode(entry.key as String);
        _validateStrings(entry.value, depth + 1);
      }
    } else if (value is List) {
      for (final item in value) {
        _validateStrings(item, depth + 1);
      }
    }
  }

  static void _validateIdentityEncoding(StoreData data) {
    // Shared account/favourite identities are JSON arrays encoded inside map
    // keys or list strings. Check the decoded parts before UTF-8 conversion too.
    for (final entry in (data['accounts'] as Map).entries) {
      for (final part in jsonDecode(entry.key as String) as List) {
        _validUnicode(part as String);
      }
      final account = entry.value as Map;
      for (final encoded in account['libraryFavourites'] as List? ?? []) {
        for (final part in jsonDecode(encoded as String) as List) {
          _validUnicode(part as String);
        }
      }
      for (final field in ['jobs', 'activity']) {
        for (final encoded in (account[field] as Map? ?? {}).keys) {
          for (final part in jsonDecode(encoded as String) as List) {
            _validUnicode(part as String);
          }
        }
      }
    }
  }

  static Uint8List encodeSnapshot(StoreData data) {
    try {
      if (data['schema'] is! int || data['schema'] != currentStoreSchema) {
        throw const AppFailure(FailureKind.recovery);
      }
      validateStoreSnapshot(data);
      _validateStrings(data);
      _validateIdentityEncoding(data);
      final bytes = Uint8List.fromList(utf8.encode(jsonEncode(data)));
      if (bytes.length > maximumSnapshotBytes) {
        throw const AppFailure(FailureKind.storage);
      }
      return bytes;
    } on AppFailure {
      rethrow;
    } catch (_) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  AppleStoreSnapshot _snapshot(Object? value, {int? expectedRevision}) {
    try {
      final map = _object(value, {'revision', 'value'});
      final revision = map['revision'];
      if (revision is! int ||
          revision < 0 ||
          (expectedRevision != null && revision != expectedRevision + 1)) {
        throw const FormatException();
      }
      final bytes = map['value'];
      if (bytes == null && revision == 0 && expectedRevision == null) {
        return AppleStoreSnapshot(
          revision: 0,
          data: {
            'schema': currentStoreSchema,
            'accounts': <String, Object?>{},
            'preferences': <String, Object?>{},
          },
        );
      }
      if (bytes is! Uint8List ||
          bytes.isEmpty ||
          bytes.length > maximumSnapshotBytes ||
          revision == 0) {
        throw const FormatException();
      }
      final data = Map<String, Object?>.from(
        jsonDecode(utf8.decode(bytes)) as Map,
      );
      if (data['schema'] is! int ||
          !{0, 1, currentStoreSchema}.contains(data['schema']) ||
          (expectedRevision != null && data['schema'] != currentStoreSchema)) {
        throw const ApplePlatformException('unsupportedSchema');
      }
      validateStoreSnapshot(data);
      _validateStrings(data);
      _validateIdentityEncoding(data);
      return AppleStoreSnapshot(
        revision: revision,
        data: data,
        sourceBytes: Uint8List.fromList(bytes),
      );
    } on ApplePlatformException {
      rethrow;
    } catch (_) {
      throw const ApplePlatformException('corruptState');
    }
  }

  Future<AppleStoreSnapshot> readState() async =>
      _snapshot(await _invoke('state.read', const {}));

  /// Shared schemas 0 and 1 differ only in their version marker. Native code
  /// backs up the exact source bytes and installs this schema-2 snapshot in one
  /// transaction, guarded by both revision and original bytes. No callbacks or
  /// provider operations are replayed while migrating.
  Future<AppleStoreSnapshot> migrateState(AppleStoreSnapshot snapshot) async {
    final bytes = snapshot.sourceBytes;
    if (bytes == null || snapshot.revision <= 0) {
      throw const ApplePlatformException('corruptState');
    }
    final original = _snapshot({
      'revision': snapshot.revision,
      'value': Uint8List.fromList(bytes),
    });
    if (!{0, 1}.contains(original.data['schema'])) {
      throw const ApplePlatformException('unsupportedSchema');
    }
    final migrated = cloneData(original.data)..['schema'] = currentStoreSchema;
    return _snapshot(
      await _invoke('state.migrate', {
        'expectedRevision': snapshot.revision,
        'expectedValue': original.sourceBytes,
        'value': encodeSnapshot(migrated),
      }),
      expectedRevision: snapshot.revision,
    );
  }

  Future<AppleStoreSnapshot> compareAndSetState(
    int expectedRevision,
    StoreData data,
  ) async {
    if (expectedRevision < 0 || expectedRevision == 0x7fffffffffffffff) {
      throw const ApplePlatformException('invalidArguments');
    }
    return _snapshot(
      await _invoke('state.compareAndSet', {
        'expectedRevision': expectedRevision,
        'value': encodeSnapshot(data),
      }),
      expectedRevision: expectedRevision,
    );
  }

  static String credentialScopeKey(AccountScope scope) {
    if (scope.isLocal) throw const AppFailure(FailureKind.authentication);
    for (final part in [scope.authority, scope.accountId]) {
      if (part.isEmpty ||
          part.length > 256 ||
          RegExp(r'[\x00-\x1f\x7f]').hasMatch(part)) {
        throw const AppFailure(FailureKind.invalidData);
      }
      _validUnicode(part);
    }
    return base64Url.encode(utf8.encode(scope.key));
  }

  Map<String, Object?> _credentialArgs(AccountScope scope) => {
    'scope': credentialScopeKey(scope),
    'slot': 'session',
  };

  Future<Uint8List?> readCredential(AccountScope scope) async {
    final value = await _invoke('credentials.read', _credentialArgs(scope));
    if (value == null) return null;
    if (value is! Uint8List || value.isEmpty || value.length > 65536) {
      throw const ApplePlatformException('corruptState');
    }
    return Uint8List.fromList(value);
  }

  Future<void> writeCredential(AccountScope scope, Uint8List value) async {
    if (value.isEmpty || value.length > 65536) {
      throw const ApplePlatformException('invalidArguments');
    }
    await _invoke('credentials.write', {
      ..._credentialArgs(scope),
      'value': Uint8List.fromList(value),
    });
  }

  Future<void> deleteCredential(AccountScope scope) async {
    await _invoke('credentials.delete', _credentialArgs(scope));
  }
}
