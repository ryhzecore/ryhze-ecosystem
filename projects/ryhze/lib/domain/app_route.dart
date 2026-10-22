enum AppDestination { gamesStore, gamesLibrary, filmsStore, activity }

/// Only navigational URIs are accepted. No link carries executable commands.
class AppRoute {
  const AppRoute(this.destination, {this.itemId, this.providerId});
  final AppDestination destination;
  final String? itemId;
  final String? providerId;

  Uri toUri() => Uri(
    scheme: 'ryhze',
    host: 'app',
    path: switch (destination) {
      AppDestination.gamesStore => '/games/store',
      AppDestination.gamesLibrary => '/games/library',
      AppDestination.filmsStore => '/films/store',
      AppDestination.activity => '/activity',
    },
    queryParameters: itemId == null && providerId == null
        ? null
        : {'item': ?itemId, 'provider': ?providerId},
  );

  static AppRoute? parse(Uri uri) {
    if (uri.toString().length > 4096) return null;
    try {
      return _parse(uri);
    } on FormatException {
      return null;
    }
  }

  static AppRoute? _parse(Uri uri) {
    if (uri.scheme != 'ryhze' ||
        uri.host != 'app' ||
        uri.hasPort ||
        uri.userInfo.isNotEmpty ||
        uri.fragment.isNotEmpty ||
        uri.queryParametersAll.keys.any(
          (key) => key != 'item' && key != 'provider',
        ) ||
        uri.queryParametersAll.values.any((values) => values.length != 1)) {
      return null;
    }
    final destination = switch (uri.path) {
      '/games/store' => AppDestination.gamesStore,
      '/games/library' => AppDestination.gamesLibrary,
      '/films/store' => AppDestination.filmsStore,
      '/activity' => AppDestination.activity,
      _ => null,
    };
    final id = uri.queryParameters['item'];
    final provider = uri.queryParameters['provider'];
    if (destination == null ||
        (destination == AppDestination.activity &&
            (uri.hasQuery || uri.hasFragment)) ||
        (id != null &&
            !RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$').hasMatch(id))) {
      return null;
    }
    if ((provider != null &&
            !RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')
                .hasMatch(provider)) ||
        (destination == AppDestination.gamesLibrary
            ? (id == null) != (provider == null)
            : provider != null)) {
      return null;
    }
    return AppRoute(destination, itemId: id, providerId: provider);
  }
}
