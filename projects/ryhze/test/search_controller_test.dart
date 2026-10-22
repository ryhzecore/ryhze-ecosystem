import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/store/catalog_repository.dart';
import 'package:ryhze/features/store/search_controller.dart';

class SearchFake implements CatalogRepository {
  final requests = <Completer<RepositoryRead<EntertainmentTitle>>>[];
  final tokens = <CancellationToken>[];
  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) {
    final completer = Completer<RepositoryRead<EntertainmentTitle>>();
    requests.add(completer);
    tokens.add(cancellation);
    return completer.future;
  }
}

void main() {
  test(
    'older search cannot replace newer results; cancellation propagates',
    () async {
      final repository = SearchFake();
      final controller = CatalogSearchController(
        repository,
        EntertainmentCategory.games,
        debounce: Duration.zero,
      );
      controller.search('old');
      await Future<void>.delayed(Duration.zero);
      controller.search('new');
      await Future<void>.delayed(Duration.zero);
      expect(repository.tokens.first.isCancelled, isTrue);
      repository.requests.last.complete(
        RepositoryRead(
          [],
          quality: ReadQuality.complete,
          observedAt: DateTime.now(),
        ),
      );
      await Future<void>.delayed(Duration.zero);
      repository.requests.first.complete(
        RepositoryRead(
          [Catalog.larcenous],
          quality: ReadQuality.complete,
          observedAt: DateTime.now(),
        ),
      );
      await Future<void>.delayed(Duration.zero);
      expect(controller.value.query, 'new');
      expect(controller.value.result!.values, isEmpty);
      controller.search('disposed');
      await Future<void>.delayed(Duration.zero);
      controller.dispose();
      expect(repository.tokens.last.isCancelled, isTrue);
      repository.requests.last.completeError(
        StateError('secret-bearing provider error'),
      );
      await Future<void>.delayed(Duration.zero);
    },
  );
}
