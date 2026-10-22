import 'dart:async';

import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../../domain/contracts.dart';
import '../entertainment/model/catalog.dart';
import 'catalog_repository.dart';

class SearchState {
  const SearchState({
    this.query = '',
    this.loading = false,
    this.result,
    this.failure,
  });
  final String query;
  final bool loading;
  final RepositoryRead<EntertainmentTitle>? result;
  final AppFailure? failure;
}

class CatalogSearchController extends ValueNotifier<SearchState> {
  CatalogSearchController(
    this.repository,
    this.category, {
    this.debounce = const Duration(milliseconds: 180),
  }) : super(const SearchState());
  final CatalogRepository repository;
  final EntertainmentCategory category;
  final Duration debounce;
  Timer? _timer;
  CancellationToken? _cancellation;
  int _generation = 0;
  bool _disposed = false;
  void search(String query) {
    if (_disposed) return;
    _timer?.cancel();
    _cancellation?.cancel();
    final generation = ++_generation;
    final cancellation = _cancellation = CancellationToken();
    value = SearchState(query: query, loading: true);
    _timer = Timer(debounce, () async {
      try {
        final result = await repository.search(category, query, cancellation);
        if (_disposed ||
            generation != _generation ||
            cancellation.isCancelled) {
          return;
        }
        value = SearchState(query: query, result: result);
      } catch (error) {
        if (_disposed ||
            generation != _generation ||
            cancellation.isCancelled) {
          return;
        }
        value = SearchState(
          query: query,
          failure: error is AppFailure
              ? error
              : const AppFailure(FailureKind.network),
        );
      }
    });
  }

  @override
  void dispose() {
    _disposed = true;
    _generation++;
    _timer?.cancel();
    _cancellation?.cancel();
    super.dispose();
  }
}
