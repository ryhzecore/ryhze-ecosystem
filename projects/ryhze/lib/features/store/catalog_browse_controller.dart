import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../../domain/contracts.dart';
import '../entertainment/model/catalog.dart';
import 'catalog_repository.dart';

class CatalogBrowseState {
  const CatalogBrowseState({this.read, this.loading = false, this.failure});
  final RepositoryRead<EntertainmentTitle>? read;
  final bool loading;
  final AppFailure? failure;
  List<EntertainmentTitle> get titles => read?.values ?? const [];
}

/// One repository supplies browsing, search and stable item lookup. A refresh
/// retains a usable snapshot; it never turns an outage into an empty catalogue.
class CatalogBrowseController extends ChangeNotifier {
  CatalogBrowseController(this.repository) {
    if (repository is LocalCatalogRepository) {
      for (final category in EntertainmentCategory.values) {
        _states[category] = CatalogBrowseState(
          read: RepositoryRead(
            category == EntertainmentCategory.games
                ? Catalog.games
                : Catalog.films,
            quality: ReadQuality.complete,
            observedAt: DateTime.now().toUtc(),
          ),
        );
      }
    }
  }
  final CatalogRepository repository;
  final _states = <EntertainmentCategory, CatalogBrowseState>{};
  final _requests = <EntertainmentCategory, CancellationToken>{};
  bool _disposed = false, _invalidating = false;
  bool _pendingNotification = false, _notifierDisposed = false;
  int _notificationDepth = 0;
  CatalogBrowseState state(EntertainmentCategory category) =>
      _states[category] ?? const CatalogBrowseState();
  bool get usesBundledCatalogue => repository is LocalCatalogRepository;

  Future<void> load(EntertainmentCategory category) async {
    if (_disposed || _invalidating || _requests.containsKey(category)) return;
    final token = CancellationToken();
    _requests[category] = token;
    final previous = state(category).read;
    _states[category] = CatalogBrowseState(read: previous, loading: true);
    notifyListeners();
    try {
      if (_disposed ||
          token.isCancelled ||
          !identical(_requests[category], token)) {
        return;
      }
      final result = await repository.search(category, '', token);
      token.check();
      if (_disposed || !identical(_requests[category], token)) return;
      _states[category] = CatalogBrowseState(
        read:
            result.quality == ReadQuality.unavailable &&
                previous != null &&
                previous.values.isNotEmpty
            ? RepositoryRead(
                previous.values,
                quality: ReadQuality.stale,
                observedAt: previous.observedAt,
                failure: result.failure,
              )
            : result,
        failure: result.failure,
      );
    } catch (error) {
      if (_disposed ||
          token.isCancelled ||
          !identical(_requests[category], token)) {
        return;
      }
      final failure = error is AppFailure
          ? error
          : const AppFailure(FailureKind.network);
      _states[category] = CatalogBrowseState(
        read: previous == null
            ? null
            : RepositoryRead(
                previous.values,
                quality: ReadQuality.stale,
                observedAt: previous.observedAt,
                failure: failure,
              ),
        failure: failure,
      );
    } finally {
      if (!_disposed && identical(_requests[category], token)) {
        _requests.remove(category);
        notifyListeners();
      }
    }
  }

  void cancel() {
    if (_disposed) return;
    _invalidate();
  }

  void _invalidate({bool clear = false}) {
    final wasInvalidating = _invalidating;
    _invalidating = true;
    final pending = List<CancellationToken>.of(_requests.values);
    _requests.clear();
    if (clear) {
      _states.clear();
    } else {
      for (final category in EntertainmentCategory.values) {
        final old = state(category);
        _states[category] = CatalogBrowseState(
          read: old.read,
          failure: old.failure,
        );
      }
    }
    if (!_disposed) _pendingNotification = true;
    (Object, StackTrace)? failure;
    try {
      for (final token in pending) {
        try {
          token.cancel();
        } catch (error, stack) {
          failure ??= (error, stack);
        }
      }
    } finally {
      _invalidating = wasInvalidating;
    }
    if (!_disposed && !_invalidating && _pendingNotification) {
      _pendingNotification = false;
      notifyListeners();
    }
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }

  void reset() {
    if (_disposed) return;
    _invalidate(clear: true);
  }

  @override
  void notifyListeners() {
    if (_disposed) return;
    if (_invalidating) {
      _pendingNotification = true;
      return;
    }
    _notificationDepth++;
    try {
      super.notifyListeners();
    } finally {
      _notificationDepth--;
      _disposeNotifierIfReady();
    }
  }

  void _disposeNotifierIfReady() {
    if (_disposed && _notificationDepth == 0 && !_notifierDisposed) {
      _notifierDisposed = true;
      super.dispose();
    }
  }

  @override
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _pendingNotification = false;
    try {
      _invalidate(clear: true);
    } finally {
      // Active notification stacks retain their array until dispatch unwinds.
      if (_notificationDepth == 0 && !_notifierDisposed) {
        _notifierDisposed = true;
        super.dispose();
      }
    }
  }
}
