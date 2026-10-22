import 'dart:convert';

import 'package:crypto/crypto.dart';

import '../core/app_failure.dart';

/// Exact anonymous catalogue identity; never a content right or local path.
final class ArtworkReference {
  ArtworkReference({
    required this.itemId,
    required this.revision,
    required this.path,
    required this.note,
  }) {
    if (!_text(itemId, 128) ||
        !RegExp(r'^[a-z0-9]+(?:-[a-z0-9]+)*$').hasMatch(itemId) ||
        revision < 0 ||
        revision > 0x7fffffff ||
        !_text(path, 160) ||
        !RegExp(r'^/art/[a-z0-9][a-z0-9-]*\.png$').hasMatch(path) ||
        !_text(note, 160)) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  final String itemId, path, note;
  final int revision;
  late final String key = sha256
      .convert(
        utf8.encode(
          jsonEncode(['ryhze-public-artwork-v1', itemId, revision, path, note]),
        ),
      )
      .toString();
  static bool _text(String value, int limit) =>
      value.trim().isNotEmpty &&
      value.length <= limit &&
      !RegExp(r'[\x00-\x1f\x7f-\x9f]').hasMatch(value) &&
      utf8.decode(utf8.encode(value)) == value;

  @override
  bool operator ==(Object other) =>
      other is ArtworkReference &&
      itemId == other.itemId &&
      revision == other.revision &&
      path == other.path &&
      note == other.note;
  @override
  int get hashCode => Object.hash(itemId, revision, path, note);
}
