import 'package:flutter/foundation.dart';

import '../features/entertainment/model/catalog.dart';
import '../domain/app_route.dart';
export '../domain/app_route.dart';

class NavigationController extends ChangeNotifier {
  EntertainmentCategory _category = EntertainmentCategory.games;
  bool _library = false;
  final Map<EntertainmentCategory, String> _queries = {};
  final Map<AppDestination, double> _offsets = {};
  final List<AppRoute> _history = [];
  EntertainmentCategory get category => _category;
  bool get libraryDestination => _library;
  AppDestination get destination => _category == EntertainmentCategory.films
      ? AppDestination.filmsStore
      : _library
      ? AppDestination.gamesLibrary
      : AppDestination.gamesStore;
  String get query => _queries[_category] ?? '';
  double get scrollOffset => _offsets[destination] ?? 0;

  void rememberScroll(double offset) {
    if (offset.isFinite && offset >= 0) _offsets[destination] = offset;
  }

  void setQuery(String query) {
    _queries[_category] = query;
  }

  void selectCategory(EntertainmentCategory value) => go(
    AppRoute(
      value == EntertainmentCategory.films
          ? AppDestination.filmsStore
          : _library
          ? AppDestination.gamesLibrary
          : AppDestination.gamesStore,
    ),
  );

  void selectLibrary(bool value) => go(
    AppRoute(value ? AppDestination.gamesLibrary : AppDestination.gamesStore),
  );

  void go(AppRoute route, {bool remember = true}) {
    // Activity is presented by RouteActivationController as a workspace modal;
    // it must never replace the underlying browsing destination or history.
    if (route.destination == AppDestination.activity) return;
    if (route.destination == destination) return;
    if (remember) {
      _history.add(AppRoute(destination));
      if (_history.length > 50) _history.removeAt(0);
    }
    _category = route.destination == AppDestination.filmsStore
        ? EntertainmentCategory.films
        : EntertainmentCategory.games;
    if (_category == EntertainmentCategory.games) {
      _library = route.destination == AppDestination.gamesLibrary;
    }
    notifyListeners();
  }

  bool back() {
    if (_history.isEmpty) return false;
    go(_history.removeLast(), remember: false);
    return true;
  }

  void resetScope() {
    _queries.clear();
    _offsets.clear();
    _history.clear();
    _category = EntertainmentCategory.games;
    _library = false;
    notifyListeners();
  }
}

/// Serializes modal commands and releases ownership even when a route fails.
class OverlayCoordinator extends ChangeNotifier {
  bool _open = false;
  bool get isOpen => _open;
  bool _disposed = false;
  Future<T?> run<T>(Future<T?> Function() action) async {
    if (_open || _disposed) return null;
    _open = true;
    notifyListeners();
    try {
      return await action();
    } finally {
      _open = false;
      if (!_disposed) notifyListeners();
    }
  }

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }
}
