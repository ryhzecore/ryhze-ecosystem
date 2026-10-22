import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';

import 'data/demo_library_repository.dart';
import 'data/library_repository.dart';
import 'data/library_preferences.dart';
import 'model/library_entry.dart';

enum LibraryStatus { unavailable, loading, error, verifiedEmpty, loaded, demo }

enum LibraryFilter { allOwned, favourites, installed }

class _LibraryViewState {
  String query = '';
  LibraryFilter filter = LibraryFilter.allOwned;
  LibraryIdentity? selectedId;
  final Set<LibraryIdentity> favourites = {};
}

/// Owns Library state independently of Store and Films. User favourites can
/// persist through an injected account-scoped repository. Demo remains transient.
/// Replacing a source clears or explicitly rebinds both preference scopes.
class LibraryController extends ChangeNotifier {
  LibraryController({
    LibraryRepository? repository,
    LibraryPreferences? preferences,
  }) : _ownedRepository = repository ?? const UnavailableLibraryRepository(),
       _repository = repository ?? const UnavailableLibraryRepository(),
       _preferences = preferences {
    _ownedView.favourites.addAll(preferences?.read() ?? {});
  }

  LibraryPreferences? _preferences;
  bool _savingFavourite = false;
  AppFailure? _preferenceFailure;
  AppFailure? get preferenceFailure => _preferenceFailure;
  bool get durableFavourites => !isDemo && (_preferences?.durable ?? false);
  LibraryRepository _ownedRepository;
  LibraryRepository _repository;
  _LibraryViewState _ownedView = _LibraryViewState();
  _LibraryViewState _demoView = _LibraryViewState();
  List<LibraryEntry> _entries = const [];
  LibraryStatus _status = LibraryStatus.unavailable;
  String _statusMessage = UnavailableLibraryRepository.reason;
  bool _demoMode = false;
  bool _resultIsDemo = false;
  bool _disposed = false;
  int _request = 0;
  CancellationToken? _cancellation;
  bool _stale = false;
  bool get isStale => _stale;

  _LibraryViewState get _view => isDemo ? _demoView : _ownedView;
  LibraryStatus get status => _status;
  String get statusMessage => _statusMessage;
  bool get isDemo => _demoMode || _resultIsDemo;
  String get query => _view.query;
  LibraryFilter get filter => _view.filter;
  LibraryIdentity? get selectedId => _view.selectedId;
  List<LibraryEntry> get entries => _entries;

  LibraryEntry? get selectedEntry {
    for (final entry in _entries) {
      if (entry.identity == selectedId) return entry;
    }
    return null;
  }

  List<LibraryEntry> get visibleEntries {
    final words = query.trim().toLowerCase().split(RegExp(r'\s+'));
    return List.unmodifiable(
      _entries.where((entry) {
        if (filter == LibraryFilter.favourites &&
            !_view.favourites.contains(entry.identity)) {
          return false;
        }
        if (filter == LibraryFilter.installed &&
            entry.installation.state != LibraryInstallationState.installed) {
          return false;
        }
        final searchable =
            '${entry.title} ${entry.identity.providerId} '
                    '${entry.identity.titleId} ${entry.tags.join(' ')}'
                .toLowerCase();
        return words.every(searchable.contains);
      }),
    );
  }

  bool isFavourite(LibraryIdentity id) => _view.favourites.contains(id);

  void setQuery(String value) {
    if (_disposed || query == value) return;
    _view.query = value;
    notifyListeners();
  }

  void setFilter(LibraryFilter value) {
    if (_disposed || filter == value) return;
    _view.filter = value;
    notifyListeners();
  }

  void select(LibraryIdentity id) {
    if (_disposed || !_entries.any((entry) => entry.identity == id)) return;
    if (selectedId == id) return;
    _view.selectedId = id;
    notifyListeners();
  }

  void back() {
    if (_disposed || selectedId == null) return;
    _view.selectedId = null;
    notifyListeners();
  }

  Future<void> toggleFavourite(LibraryIdentity id) async {
    if (_disposed ||
        _savingFavourite ||
        !_entries.any((entry) => entry.identity == id)) {
      return;
    }
    _preferenceFailure = null;
    final view = _view;
    final preferences = _preferences;
    final prior = Set<LibraryIdentity>.of(view.favourites);
    if (!view.favourites.remove(id)) view.favourites.add(id);
    notifyListeners();
    if (_disposed ||
        isDemo ||
        preferences == null ||
        !identical(preferences, _preferences) ||
        !identical(view, _ownedView)) {
      return;
    }
    _savingFavourite = true;
    try {
      await preferences.save(Set.of(view.favourites));
    } catch (_) {
      if (!_disposed &&
          identical(preferences, _preferences) &&
          identical(view, _ownedView)) {
        view.favourites
          ..clear()
          ..addAll(prior);
        _preferenceFailure = const AppFailure(FailureKind.storage);
        _statusMessage = 'The favourite could not be saved. Try again.';
        notifyListeners();
      }
    } finally {
      if (!_disposed) _savingFavourite = false;
    }
  }

  Future<void> enterDemo() async {
    if (_disposed || _demoMode) return;
    _demoMode = true;
    _resultIsDemo = false;
    _repository = const DemoLibraryRepository();
    await load();
  }

  Future<void> leaveDemo() async {
    if (_disposed || !isDemo) return;
    if (!_demoMode) {
      // An injected fixture source has no separate owned-data repository to
      // restore. Exit to truthful unavailable access, not another demo fetch.
      _ownedRepository = const UnavailableLibraryRepository();
    }
    _demoMode = false;
    _resultIsDemo = false;
    _repository = _ownedRepository;
    await load();
  }

  Future<void> setRepository(
    LibraryRepository repository, {
    LibraryPreferences? preferences,
  }) async {
    if (_disposed) return;
    _ownedRepository = repository;
    _repository = repository;
    _preferences = preferences;
    _preferenceFailure = null;
    _ownedView = _LibraryViewState()
      ..favourites.addAll(preferences?.read() ?? {});
    _demoView = _LibraryViewState();
    _demoMode = false;
    _resultIsDemo = false;
    await load();
  }

  Future<void> load() => _load();

  Future<void> refresh() => _load(preserveKnownData: true);

  Future<void> _load({bool preserveKnownData = false}) async {
    if (_disposed) return;
    final previous = _cancellation;
    final cancellation = _cancellation = CancellationToken();
    final request = ++_request;
    final repository = _repository;
    final retained = preserveKnownData ? _entries : const <LibraryEntry>[];
    final retainedStatus = _status;
    _stale = false;
    _entries = const [];
    _status = LibraryStatus.loading;
    _statusMessage = isDemo
        ? 'Loading fictional Demo Library…'
        : 'Loading Library…';
    if (retained.isNotEmpty) {
      _entries = retained;
      _status = retainedStatus;
      _statusMessage = 'Refreshing verification…';
    }
    try {
      // Publish the new boundary and clear old-account entries before an old
      // adapter's cancellation callback can throw or replace the source again.
      previous?.cancel();
      if (_disposed || request != _request) return;
      notifyListeners();
      // A listener can replace the source synchronously during notification.
      if (_disposed || request != _request) return;
      final result = repository is CancellableLibraryRepository
          ? await (repository as CancellableLibraryRepository).loadCancellable(
              cancellation,
            )
          : await repository.load();
      if (_disposed || request != _request) return;
      _accept(result);
      if (retained.isNotEmpty &&
          (result.kind == LibraryResultKind.failure ||
              result.kind == LibraryResultKind.unavailable)) {
        _entries = retained;
        _status = retainedStatus;
        _stale = true;
        _statusMessage = retainedStatus == LibraryStatus.demo
            ? 'Demo refresh unavailable. Fictional records retained.'
            : 'Previously verified records · verification is unavailable. Actions remain disabled.';
      }
    } catch (_) {
      if (_disposed || request != _request) return;
      _entries = const [];
      _status = LibraryStatus.error;
      // Provider exceptions can contain sensitive details. Do not render them.
      _statusMessage = 'Library could not be loaded. Try again.';
      if (retained.isNotEmpty) {
        _entries = retained;
        _status = retainedStatus;
        _stale = true;
        _statusMessage = retainedStatus == LibraryStatus.demo
            ? 'Demo refresh unavailable. Fictional records retained.'
            : 'Previously verified records · verification is unavailable. Actions remain disabled.';
      }
    }
    notifyListeners();
  }

  void _accept(LibraryLoadResult result) {
    _statusMessage = result.message;
    switch (result.kind) {
      case LibraryResultKind.unavailable:
        _status = LibraryStatus.unavailable;
      case LibraryResultKind.failure:
        _status = LibraryStatus.error;
      case LibraryResultKind.verified:
      case LibraryResultKind.demo:
        final demo = result.kind == LibraryResultKind.demo;
        if (_demoMode && !demo) {
          throw StateError('Invalid Demo Library result.');
        }
        final identities = <LibraryIdentity>{};
        for (final entry in result.entries) {
          if (entry.identity.providerId.trim().isEmpty ||
              entry.identity.titleId.trim().isEmpty ||
              !identities.add(entry.identity) ||
              entry.isDemo != demo) {
            throw StateError('Invalid Library result.');
          }
        }
        final owned = result.entries
            .where(
              (entry) =>
                  demo ||
                  entry.ownership.state == LibraryOwnershipState.verifiedOwned,
            )
            .toList();
        owned.sort((left, right) {
          final title = left.title.toLowerCase().compareTo(
            right.title.toLowerCase(),
          );
          if (title != 0) return title;
          final provider = left.identity.providerId.compareTo(
            right.identity.providerId,
          );
          return provider != 0
              ? provider
              : left.identity.titleId.compareTo(right.identity.titleId);
        });
        _entries = List.unmodifiable(owned);
        _resultIsDemo = demo;
        final hasUnknownOwnership = result.entries.any(
          (entry) => entry.ownership.state == LibraryOwnershipState.unknown,
        );
        if (!demo && (hasUnknownOwnership || result.partial)) {
          _status = owned.isEmpty
              ? LibraryStatus.unavailable
              : LibraryStatus.loaded;
          _statusMessage = owned.isEmpty
              ? 'Ownership could not be verified. An empty Library has not been established.'
              : 'Showing verified owned games. Some ownership records could not be verified.';
        } else {
          _status = demo
              ? LibraryStatus.demo
              : owned.isEmpty
              ? LibraryStatus.verifiedEmpty
              : LibraryStatus.loaded;
        }
        if (!owned.any((entry) => entry.identity == selectedId)) {
          _view.selectedId = null;
        }
    }
  }

  @override
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _request++;
    final cancellation = _cancellation;
    _cancellation = null;
    try {
      cancellation?.cancel();
    } finally {
      super.dispose();
    }
  }
}
