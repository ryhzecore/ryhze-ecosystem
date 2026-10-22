import '../core/app_failure.dart';
import '../design/artwork_reference.dart';
import '../features/entertainment/model/catalog.dart';

/// Shared validation for network responses and optional persisted metadata.
class PublicCatalogCodec {
  static const maxItems = 1000;
  static const metadataAvailability =
      'Public catalogue information only. Ownership, playable builds and download rights are not verified.';
  static final _controls = RegExp(r'[\x00-\x1f\x7f-\x9f]');
  static final _id = RegExp(r'^[a-z0-9]+(?:-[a-z0-9]+)*$');
  static String _text(Object? value, int limit) {
    if (value is! String ||
        value.trim().isEmpty ||
        value.length > limit ||
        _controls.hasMatch(value)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return value;
  }

  static List<EntertainmentTitle> parse(Object? input) {
    if (input is! List || input.length > maxItems) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final ids = <String>{};
    final titles = <EntertainmentTitle>[];
    for (final entry in input) {
      if (entry is! Map<String, dynamic>) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final id = _text(entry['id'], 128);
      final kind = entry['kind'];
      final revision = entry['revision'];
      if (!_id.hasMatch(id) ||
          !ids.add(id) ||
          (kind != 'game' && kind != 'film') ||
          entry['hidden'] is! bool ||
          revision is! int ||
          revision < 0 ||
          revision > 0x7fffffff) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final title = _text(entry['title'], 160);
      final description = _text(entry['description'], 8000);
      final status = _text(entry['status'], 80);
      final categories = entry['categories'];
      if (categories is! List || categories.length > 16) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final tags = categories
          .map((value) => _text(value, 80))
          .toList(growable: false);
      if (tags.toSet().length != tags.length) {
        throw const AppFailure(FailureKind.invalidData);
      }
      // An optional image must carry its public promotional label and exact
      // catalogue revision. Absence preserves the branded fallback.
      ArtworkReference? artwork;
      if (entry['image'] != null || entry['imageNote'] != null) {
        artwork = ArtworkReference(
          itemId: id,
          revision: revision,
          path: _text(entry['image'], 160),
          note: _text(entry['imageNote'], 160),
        );
      }
      // Hidden and unsupported-kind records are still validated, but never
      // projected into the visible games catalogue or film capability.
      if (entry['hidden'] == true || kind != 'game') continue;
      titles.add(
        EntertainmentTitle(
          id: id,
          title: title,
          description: description,
          tags: List.unmodifiable(tags),
          status: status,
          availability: metadataAvailability,
          artwork: artwork,
        ),
      );
    }
    return List.unmodifiable(titles);
  }
}
