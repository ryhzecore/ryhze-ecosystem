import 'package:flutter/foundation.dart';

import '../core/app_failure.dart';
import '../domain/contracts.dart';
import '../features/entertainment/model/catalog.dart';
import '../features/library/library_controller.dart';
import '../features/library/model/library_entry.dart';
import '../features/session/session_controller.dart';
import '../features/store/catalog_repository.dart';
import 'navigation_controller.dart';

class RouteActivationState {
  const RouteActivationState({
    this.title,
    this.failure,
    this.activity = false,
    this.quality = ReadQuality.complete,
  });
  final EntertainmentTitle? title;
  final AppFailure? failure;
  final bool activity;
  final ReadQuality quality;
  bool get hasPendingPresentation => title != null || activity;
}

/// Native activation and in-app item commands share one validation/resolution
/// path. An activation never carries an executable action or creates ownership.
class RouteActivationController extends ValueNotifier<RouteActivationState> {
  RouteActivationController({
    required this.catalog,
    required this.navigation,
    required this.session,
    required this.library,
    required this.overlays,
  }) : super(const RouteActivationState()) {
    navigation.addListener(invalidate);
    session.addListener(invalidate);
  }
  final CatalogRepository catalog;
  final NavigationController navigation;
  final SessionController session;
  final LibraryController library;
  final OverlayCoordinator overlays;
  CancellationToken? _token;
  bool _disposed = false;
  void invalidate() {
    _token?.cancel();
    _token = null;
    if (!_disposed) value = const RouteActivationState();
  }

  Future<void> activateUri(Uri uri) async {
    final route = AppRoute.parse(uri);
    if (route == null) {
      invalidate();
      if (!_disposed) {
        value = const RouteActivationState(
          failure: AppFailure(FailureKind.invalidData),
        );
      }
      return;
    }
    await activate(route);
  }

  Future<void> activate(AppRoute requested) async {
    if (_disposed || overlays.isOpen) return;
    // Revalidate even typed requests: notification records are untrusted input.
    final route = AppRoute.parse(requested.toUri());
    if (route == null) {
      invalidate();
      value = const RouteActivationState(
        failure: AppFailure(FailureKind.invalidData),
      );
      return;
    }
    invalidate();
    if (route.destination == AppDestination.activity) {
      value = const RouteActivationState(activity: true);
      return;
    }
    navigation.go(AppRoute(route.destination));
    if (route.itemId == null) return;
    final token = _token = CancellationToken();
    final generation = session.generation;
    try {
      if (route.destination == AppDestination.gamesLibrary) {
        final identity = LibraryIdentity(
          providerId: route.providerId!,
          titleId: route.itemId!,
        );
        if (library.isDemo ||
            !library.entries.any((entry) => entry.identity == identity)) {
          throw const AppFailure(FailureKind.unsupported);
        }
        library.select(identity);
        return;
      }
      final lookup = catalog;
      final category = route.destination == AppDestination.filmsStore
          ? EntertainmentCategory.films
          : EntertainmentCategory.games;
      EntertainmentTitle? item;
      var quality = ReadQuality.complete;
      if (lookup is CatalogMetadataLookupRepository) {
        final read = await (lookup as CatalogMetadataLookupRepository)
            .lookupRead(category, route.itemId!, token);
        token.check();
        if (read.quality == ReadQuality.unavailable || read.values.length > 1) {
          throw read.failure ?? const AppFailure(FailureKind.unsupported);
        }
        item = read.values.firstOrNull;
        quality = read.quality;
      } else if (lookup is CatalogLookupRepository) {
        item = await (lookup as CatalogLookupRepository).lookup(
          category,
          route.itemId!,
          token,
        );
      } else {
        throw const AppFailure(FailureKind.unsupported);
      }
      if (_disposed || token.isCancelled || generation != session.generation) {
        return;
      }
      if (item == null || item.id != route.itemId) {
        throw const AppFailure(FailureKind.unsupported);
      }
      value = RouteActivationState(title: item, quality: quality);
    } catch (error) {
      if (!_disposed &&
          !token.isCancelled &&
          generation == session.generation) {
        value = RouteActivationState(
          failure: error is AppFailure
              ? error
              : const AppFailure(FailureKind.network),
        );
      }
    }
  }

  EntertainmentTitle? takeTitle() {
    final title = value.title;
    if (title != null) value = const RouteActivationState();
    return title;
  }

  bool takeActivity() {
    final requested = value.activity;
    if (requested) value = const RouteActivationState();
    return requested;
  }

  @override
  void dispose() {
    _disposed = true;
    _token?.cancel();
    navigation.removeListener(invalidate);
    session.removeListener(invalidate);
    super.dispose();
  }
}
