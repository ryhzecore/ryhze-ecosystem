import '../../../design/artwork_reference.dart';

enum EntertainmentCategory { games, films }

class EntertainmentTitle {
  const EntertainmentTitle({
    required this.id,
    required this.title,
    required this.description,
    required this.tags,
    this.artwork,
    this.status = 'Availability unverified',
    this.availability = 'No availability has been verified for this title.',
  });
  final String id;
  final String title;
  final String description;
  final List<String> tags;
  final ArtworkReference? artwork;
  final String status;
  final String availability;
}

abstract final class Catalog {
  static const larcenous = EntertainmentTitle(
    id: 'larcenous-driftscape',
    status: 'To Be Continued',
    availability: availability,
    title: 'Larcenous Driftscape',
    description:
        'An open-world crime and driving game set across San Coronado. '
        'From the working docks of Oakhaven to the mountain roads of Cascadia, '
        'every route is another way into the story.',
    tags: ['Open world', 'Driving', 'Crime'],
  );
  static const games = [larcenous];
  static const tommyRiley = EntertainmentTitle(
    id: 'tommy-riley',
    title: 'Tommy Riley',
    description:
        'Lured by money, an influencer joins an underground organisation. '
        'When he regrets his allegiance, he turns against it.',
    tags: [],
    status: 'In development',
    availability:
        'The film is not available to watch. No release date is available yet.',
  );
  static const films = [tommyRiley];
  static const availability =
      'No playable build, download or release date is available yet.';

  static List<EntertainmentTitle> search(
    String query, {
    EntertainmentCategory category = EntertainmentCategory.games,
  }) {
    final words = query.trim().toLowerCase().split(RegExp(r'\s+'));
    return (category == EntertainmentCategory.games ? games : films)
        .where(
          (title) => words.every(
            (word) => '${title.title} ${title.tags.join(' ')}'
                .toLowerCase()
                .contains(word),
          ),
        )
        .toList(growable: false);
  }
}
