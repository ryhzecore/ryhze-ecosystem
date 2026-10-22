import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../collections/collection_repository.dart';
import '../session/session_controller.dart';

/// Stable catalog IDs own Store collection intent, never entitlement evidence.
class StoreController extends ChangeNotifier {
  StoreController({this.repository, this.session, this.writable = true});
  final CollectionRepository? repository;
  final SessionController? session;
  final bool writable;
  final Map<String, ValueNotifier<bool>> _favourites = {};
  final Set<String> _pending = {};
  AppFailure? _failure;
  AppFailure? get failure => _failure;
  bool _disposed = false;
  ValueNotifier<bool> favourite(String id) => _favourites.putIfAbsent(
    id,
    () => ValueNotifier(
      repository
              ?.read(session?.scope ?? const AccountScope.local())
              .any((item) => item.id == id) ??
          false,
    ),
  );
  Future<void> toggle(String id, {String? title}) async {
    if (_disposed || _pending.contains(id)) return;
    if (!writable) {
      _failure = const AppFailure(FailureKind.storage);
      notifyListeners();
      return;
    }
    final value = favourite(id);
    final previous = value.value;
    final generation = session?.generation;
    final scope = session?.scope ?? const AccountScope.local();
    value.value = !previous;
    _pending.add(id);
    _failure = null;
    try {
      await repository?.setSaved(scope, SavedTitle(id, title ?? id), !previous);
    } catch (_) {
      if (!_disposed && generation == session?.generation) {
        value.value = previous;
        _failure = const AppFailure(FailureKind.storage);
      }
    } finally {
      if (!_disposed && generation == session?.generation) {
        _pending.remove(id);
        notifyListeners();
      }
    }
  }

  void resetScope() {
    _pending.clear();
    _failure = null;
    final saved =
        repository?.read(session?.scope ?? const AccountScope.local()) ?? [];
    for (final entry in _favourites.entries) {
      entry.value.value = saved.any((item) => item.id == entry.key);
    }
    notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    for (final value in _favourites.values) {
      value.dispose();
    }
    super.dispose();
  }
}
