import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;

import '../../core/app_failure.dart';
import '../../domain/contracts.dart';

const managedManifestMaximumPayloadBytes = 1024 * 1024 * 1024;

/// Normalized local input supplied by an independently authorized adapter.
/// This encoding is not a signed provider protocol. The adapter must verify its
/// real source before constructing the grant that binds these exact bytes.
class ManagedContentManifest {
  ManagedContentManifest._({
    required this.sha256,
    required this.item,
    required this.packageId,
    required this.packageVersion,
    required this.payloadSize,
    required this.payloadSha256,
    required this.files,
    required this.expandedBytes,
  });

  static const maximumManifestBytes = 64 * 1024;
  static const maximumFiles = 256;
  static const maximumPathDepth = 8;
  final String sha256, packageId, packageVersion, payloadSha256;
  final ProviderItemRef item;
  final int payloadSize, expandedBytes;
  final List<ManagedContentFile> files;

  static ManagedContentManifest decode(Uint8List bytes) {
    if (bytes.isEmpty || bytes.length > maximumManifestBytes) _invalid();
    try {
      final text = utf8.decode(bytes, allowMalformed: false);
      _checkJsonDepth(text);
      final raw = jsonDecode(text);
      // Exact normalized encoding also refuses duplicate object keys instead of
      // accepting jsonDecode's last-wins interpretation of an ambiguous input.
      if (jsonEncode(raw) != text) _invalid();
      final value = _object(raw, const {
        'version',
        'provider',
        'item',
        'namespace',
        'packageId',
        'packageVersion',
        'payloadSize',
        'payloadSha256',
        'files',
      });
      if (value['version'] != 1 || value['version'] is! int) _invalid();
      final item = ProviderItemRef(
        _text(value['provider']),
        _text(value['item']),
        namespace: _text(value['namespace'], allowEmpty: true),
      );
      final packageId = _text(value['packageId']);
      final packageVersion = _text(value['packageVersion']);
      final payloadSize = _size(value['payloadSize'], allowEmpty: false);
      final payloadSha256 = _digest(value['payloadSha256']);
      final entries = value['files'];
      if (entries is! List ||
          entries.isEmpty ||
          entries.length > maximumFiles) {
        _invalid();
      }
      final files = <ManagedContentFile>[];
      final filePaths = <String>{};
      final directoryPaths = <String>{};
      var expandedBytes = 0;
      for (final entry in entries) {
        final file = _object(entry, const {
          'path',
          'size',
          'sha256',
          'executable',
        });
        final path = _path(file['path']);
        final folded = path.toLowerCase();
        if (directoryPaths.contains(folded) || !filePaths.add(folded)) {
          _invalid();
        }
        final segments = folded.split('/');
        for (var depth = 1; depth < segments.length; depth++) {
          final directory = segments.take(depth).join('/');
          if (filePaths.contains(directory)) _invalid();
          directoryPaths.add(directory);
        }
        final size = _size(file['size'], allowEmpty: true);
        expandedBytes += size;
        if (expandedBytes > managedManifestMaximumPayloadBytes) _invalid();
        final digest = _digest(file['sha256']);
        final executable = file['executable'];
        if (executable is! bool || executable != folded.endsWith('.exe')) {
          _invalid();
        }
        files.add(ManagedContentFile._(path, size, digest, executable));
      }
      return ManagedContentManifest._(
        sha256: crypto.sha256.convert(bytes).toString(),
        item: item,
        packageId: packageId,
        packageVersion: packageVersion,
        payloadSize: payloadSize,
        payloadSha256: payloadSha256,
        files: List.unmodifiable(files),
        expandedBytes: expandedBytes,
      );
    } on AppFailure {
      rethrow;
    } on Object {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  bool binds({
    required ProviderItemRef item,
    required String packageId,
    required String packageVersion,
    required String manifestSha256,
    required int payloadSize,
    required String payloadSha256,
  }) =>
      this.item == item &&
      this.packageId == packageId &&
      this.packageVersion == packageVersion &&
      sha256 == manifestSha256 &&
      this.payloadSize == payloadSize &&
      this.payloadSha256 == payloadSha256;
}

/// A declared expected file, never proof it was extracted or is safe to launch.
/// Executable means a declared .exe location; no native validation is implied.
class ManagedContentFile {
  const ManagedContentFile._(
    this.path,
    this.size,
    this.sha256,
    this.executable,
  );
  final String path, sha256;
  final int size;
  final bool executable;
}

/// Immutable history of an adapter admission. Reopening this value grants no
/// permission. Every later effect must obtain fresh source authority.
class ManagedManifestProvenance {
  ManagedManifestProvenance._(this._json, this.manifest, this.normalizedBytes);
  static const maximumEncodedBytes = 96 * 1024;
  final Map<String, Object?> _json;
  final ManagedContentManifest manifest;
  final Uint8List normalizedBytes;
  String get authority => _json['authority'] as String;
  String get policy => _json['policy'] as String;
  String get sourceManifestSha256 => _json['sourceManifestSha256'] as String;
  DateTime get observedAt => DateTime.parse(_json['observedAt'] as String);
  DateTime get expiresAt => DateTime.parse(_json['expiresAt'] as String);
  String get recordIdentity =>
      crypto.sha256.convert(utf8.encode(jsonEncode(_json))).toString();
  Map<String, Object?> toJson() =>
      Map<String, Object?>.from(jsonDecode(jsonEncode(_json)) as Map);

  bool sameArtifact(ManagedManifestProvenance other) =>
      authority == other.authority &&
      policy == other.policy &&
      sourceManifestSha256 == other.sourceManifestSha256 &&
      manifest.sha256 == other.manifest.sha256;

  static ManagedManifestProvenance create({
    required String authority,
    required String policy,
    required String sourceManifestSha256,
    required Uint8List normalizedBytes,
    required DateTime observedAt,
    required DateTime expiresAt,
  }) {
    if (normalizedBytes.isEmpty ||
        normalizedBytes.length > ManagedContentManifest.maximumManifestBytes) {
      _invalid();
    }
    return decode(<String, Object?>{
      'version': 1,
      'authority': authority,
      'policy': policy,
      'sourceManifestSha256': sourceManifestSha256,
      'normalizedManifestSha256': crypto.sha256
          .convert(normalizedBytes)
          .toString(),
      'normalizedManifest': base64Encode(normalizedBytes),
      'observedAt': observedAt.toUtc().toIso8601String(),
      'expiresAt': expiresAt.toUtc().toIso8601String(),
    });
  }

  static ManagedManifestProvenance decode(Object? value) {
    try {
      if (value is! Map || value['version'] != 1 || value['version'] is! int) {
        _invalid();
      }
      final encoded = value['normalizedManifest'];
      if (encoded is! String || encoded.isEmpty || encoded.length > 87384) {
        _invalid();
      }
      final json = jsonEncode(value);
      if (utf8.encode(json).length > maximumEncodedBytes) _invalid();
      final bytes = base64Decode(encoded);
      if (bytes.length > ManagedContentManifest.maximumManifestBytes ||
          base64Encode(bytes) != encoded) {
        _invalid();
      }
      final manifest = ManagedContentManifest.decode(bytes);
      if (_digest(value['normalizedManifestSha256']) != manifest.sha256) {
        _invalid();
      }
      _digest(value['sourceManifestSha256']);
      _text(value['authority']);
      _text(value['policy']);
      final observed = _timestamp(value['observedAt']);
      final expires = _timestamp(value['expiresAt']);
      if (!expires.isAfter(observed)) _invalid();
      return ManagedManifestProvenance._(
        Map<String, Object?>.from(jsonDecode(json) as Map),
        manifest,
        Uint8List.fromList(bytes).asUnmodifiableView(),
      );
    } on AppFailure {
      rethrow;
    } on Object {
      _invalid();
    }
  }
}

DateTime _timestamp(Object? value) {
  if (value is! String || value.length > 32 || !value.endsWith('Z')) _invalid();
  final result = DateTime.tryParse(value);
  if (result == null) _invalid();
  return result;
}

Never _invalid() => throw const AppFailure(FailureKind.invalidData);

Map<String, dynamic> _object(Object? value, Set<String> fields) {
  if (value is! Map<String, dynamic> ||
      value.length != fields.length ||
      !fields.every(value.containsKey)) {
    _invalid();
  }
  return value;
}

String _text(Object? value, {bool allowEmpty = false}) {
  if (value is! String ||
      (!allowEmpty && value.trim().isEmpty) ||
      value.length > 256 ||
      RegExp(r'[\x00-\x1f\x7f]').hasMatch(value)) {
    _invalid();
  }
  return value;
}

String _digest(Object? value) {
  if (value is! String || !RegExp(r'^[0-9a-f]{64}$').hasMatch(value)) {
    _invalid();
  }
  return value;
}

int _size(Object? value, {required bool allowEmpty}) {
  if (value is! int ||
      value < (allowEmpty ? 0 : 1) ||
      value > managedManifestMaximumPayloadBytes) {
    _invalid();
  }
  return value;
}

String _path(Object? value) {
  if (value is! String || value.isEmpty || value.length > 240) _invalid();
  final segments = value.split('/');
  if (segments.length > ManagedContentManifest.maximumPathDepth) _invalid();
  for (final segment in segments) {
    final stem = segment.split('.').first.toUpperCase();
    if (!RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$').hasMatch(segment) ||
        segment.endsWith('.') ||
        {'CON', 'PRN', 'AUX', 'NUL'}.contains(stem) ||
        RegExp(r'^(COM|LPT)[0-9]$').hasMatch(stem)) {
      _invalid();
    }
  }
  return value;
}

void _checkJsonDepth(String text) {
  var depth = 0;
  var quoted = false, escaped = false;
  for (final character in text.codeUnits) {
    if (quoted) {
      if (escaped) {
        escaped = false;
      } else if (character == 92) {
        escaped = true;
      } else if (character == 34) {
        quoted = false;
      }
    } else if (character == 34) {
      quoted = true;
    } else if (character == 91 || character == 123) {
      if (++depth > 8) _invalid();
    } else if (character == 93 || character == 125) {
      if (--depth < 0) _invalid();
    }
  }
  if (quoted || depth != 0) _invalid();
}
