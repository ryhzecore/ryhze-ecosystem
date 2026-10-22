import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/store/catalog_browse_controller.dart';
import 'package:ryhze/features/store/catalog_repository.dart';

class _Repository implements CatalogRepository {
  final requests = <Completer<RepositoryRead<EntertainmentTitle>>>[];
  final tokens = <CancellationToken>[];
  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) {
    final request = Completer<RepositoryRead<EntertainmentTitle>>();
    requests.add(request);
    tokens.add(cancellation);
    return request.future;
  }
}

RepositoryRead<EntertainmentTitle> _read({
  ReadQuality quality = ReadQuality.complete,
  List<EntertainmentTitle> items = Catalog.games,
}) => RepositoryRead(
  items,
  quality: quality,
  observedAt: DateTime.utc(2026, 10, 10),
  failure: quality == ReadQuality.unavailable
      ? const AppFailure(FailureKind.network)
      : null,
);

void main() {
  test('bundled source preserves existing synchronous browsing snapshot', () {
    final controller = CatalogBrowseController(LocalCatalogRepository());
    expect(
      controller.state(EntertainmentCategory.games).titles.single.id,
      Catalog.larcenous.id,
    );
    expect(
      controller.state(EntertainmentCategory.films).titles.single,
      same(Catalog.tommyRiley),
    );
    controller.dispose();
  });
  test(
    'failed refresh keeps last known metadata and complete empty clears it',
    () async {
      final repository = _Repository();
      final controller = CatalogBrowseController(repository);
      var loading = controller.load(EntertainmentCategory.games);
      await controller.load(EntertainmentCategory.games);
      expect(repository.requests, hasLength(1));
      repository.requests.last.complete(_read());
      await loading;
      loading = controller.load(EntertainmentCategory.games);
      expect(
        controller.state(EntertainmentCategory.games).titles,
        hasLength(1),
      );
      repository.requests.last.complete(
        _read(quality: ReadQuality.unavailable, items: []),
      );
      await loading;
      expect(
        controller.state(EntertainmentCategory.games).read!.quality,
        ReadQuality.stale,
      );
      expect(
        controller.state(EntertainmentCategory.games).titles,
        hasLength(1),
      );
      loading = controller.load(EntertainmentCategory.games);
      repository.requests.last.complete(_read(items: []));
      await loading;
      expect(controller.state(EntertainmentCategory.games).titles, isEmpty);
      expect(
        controller.state(EntertainmentCategory.games).read!.quality,
        ReadQuality.complete,
      );
      controller.dispose();
    },
  );
  test(
    'interruption cancels work and rejects completion from replaced request',
    () async {
      final repository = _Repository();
      final controller = CatalogBrowseController(repository);
      final first = controller.load(EntertainmentCategory.games);
      controller.cancel();
      expect(repository.tokens.first.isCancelled, isTrue);
      final second = controller.load(EntertainmentCategory.games);
      repository.requests.last.complete(_read(items: []));
      await second;
      repository.requests.first.complete(_read());
      await first;
      expect(controller.state(EntertainmentCategory.games).titles, isEmpty);
      controller.dispose();
    },
  );
  test(
    'category state is independent and disposal cancels every request',
    () async {
      final repository = _Repository();
      final controller = CatalogBrowseController(repository);
      final games = controller.load(EntertainmentCategory.games);
      final films = controller.load(EntertainmentCategory.films);
      repository.requests.first.complete(_read());
      await games;
      expect(
        controller.state(EntertainmentCategory.games).titles,
        hasLength(1),
      );
      expect(controller.state(EntertainmentCategory.films).loading, isTrue);
      controller.dispose();
      expect(repository.tokens.last.isCancelled, isTrue);
      repository.requests.last.complete(_read(items: []));
      await films;
    },
  );
}
