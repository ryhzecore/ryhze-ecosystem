import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/store/catalog_browse_controller.dart';
import 'package:ryhze/features/store/catalog_repository.dart';

class _Catalog implements CatalogRepository {
  final pending = <Completer<RepositoryRead<EntertainmentTitle>>>[];
  final tokens = <CancellationToken>[];
  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) {
    tokens.add(cancellation);
    final request = Completer<RepositoryRead<EntertainmentTitle>>();
    pending.add(request);
    return request.future;
  }
}

RepositoryRead<EntertainmentTitle> _read({bool empty = false}) =>
    RepositoryRead(
      empty ? [] : Catalog.games.take(1).toList(),
      quality: ReadQuality.complete,
      observedAt: DateTime.utc(2026, 10, 11),
    );

const _games = EntertainmentCategory.games;
const _films = EntertainmentCategory.films;

void main() {
  test(
    'cancel owns a stable snapshot and preserves the first callback error',
    () async {
      final repository = _Catalog();
      final controller = CatalogBrowseController(repository);
      addTearDown(controller.dispose);
      final initial = controller.load(_games);
      final previous = _read();
      repository.pending.single.complete(previous);
      await initial;
      final games = controller.load(_games);
      final films = controller.load(_films);
      final expected = StateError('synthetic first failure');
      var laterCalled = false;
      final reentrant = <Future<void>>[];
      repository.tokens[1].onCancel(() {
        expect(controller.state(_games).loading, isFalse);
        expect(controller.state(_films).loading, isFalse);
        expect(controller.state(_games).read, same(previous));
        reentrant.add(controller.load(_games));
        reentrant.add(controller.load(_films));
        throw expected;
      });
      repository.tokens[1].onCancel(() => laterCalled = true);
      repository.tokens[2].onCancel(() {
        reentrant.add(controller.load(_games));
        throw StateError('synthetic second failure');
      });
      expect(controller.cancel, throwsA(same(expected)));
      await Future.wait(reentrant);
      expect(laterCalled, isTrue);
      expect(
        repository.tokens.skip(1).every((token) => token.isCancelled),
        isTrue,
      );
      expect(repository.pending, hasLength(3));
      final next = controller.load(_games);
      repository.pending[1].complete(_read(empty: true));
      repository.pending[2].complete(_read());
      await Future.wait([games, films]);
      expect(controller.state(_games).loading, isTrue);
      expect(controller.state(_games).read, same(previous));
      expect(controller.state(_films).read, isNull);
      final newest = _read(empty: true);
      repository.pending[3].complete(newest);
      await next;
      expect(controller.state(_games).read, same(newest));
    },
  );

  test(
    'reset clears scope before callbacks and announces only settled state',
    () async {
      final repository = _Catalog();
      final controller = CatalogBrowseController(repository);
      addTearDown(controller.dispose);
      final initial = controller.load(_games);
      repository.pending.single.complete(_read());
      await initial;
      final games = controller.load(_games);
      final films = controller.load(_films);
      final expected = StateError('synthetic reset failure');
      final reentrant = <Future<void>>[];
      var notifications = 0;
      controller.addListener(() {
        notifications++;
        expect(repository.tokens[1].isCancelled, isTrue);
        expect(repository.tokens[2].isCancelled, isTrue);
      });
      repository.tokens[1].onCancel(() {
        expect(controller.state(_games).read, isNull);
        controller.cancel();
        controller.notifyListeners();
        reentrant.add(controller.load(_films));
        throw expected;
      });
      repository.tokens[2].onCancel(
        () => reentrant.add(controller.load(_games)),
      );
      expect(controller.reset, throwsA(same(expected)));
      await Future.wait(reentrant);
      expect(notifications, 1);
      expect(repository.pending, hasLength(3));
      repository.pending[1].complete(_read());
      repository.pending[2].complete(_read());
      await Future.wait([games, films]);
      for (final category in EntertainmentCategory.values) {
        expect(controller.state(category).read, isNull);
        expect(controller.state(category).loading, isFalse);
      }
    },
  );

  test(
    'nested reset wins over an older cancel without mutating the sweep',
    () async {
      final repository = _Catalog();
      final controller = CatalogBrowseController(repository);
      addTearDown(controller.dispose);
      final initial = controller.load(_games);
      repository.pending.single.complete(_read());
      await initial;
      final games = controller.load(_games);
      final films = controller.load(_films);
      repository.tokens[1].onCancel(controller.reset);
      expect(controller.cancel, returnsNormally);
      expect(repository.tokens[2].isCancelled, isTrue);
      expect(controller.state(_games).read, isNull);
      repository.pending[1].complete(_read());
      repository.pending[2].complete(_read());
      await Future.wait([games, films]);
      expect(controller.state(_games).read, isNull);
    },
  );

  test(
    'throwing disposal cancels every category and releases notifier',
    () async {
      final repository = _Catalog();
      final controller = CatalogBrowseController(repository);
      final games = controller.load(_games);
      final films = controller.load(_films);
      final expected = StateError('synthetic disposal failure');
      var notified = 0;
      controller.addListener(() => notified++);
      repository.tokens[0].onCancel(() {
        expect(controller.state(_games).loading, isFalse);
        expect(controller.state(_films).loading, isFalse);
        controller.dispose();
        throw expected;
      });
      repository.tokens[1].onCancel(() => throw StateError('later failure'));
      expect(controller.dispose, throwsA(same(expected)));
      expect(repository.tokens.every((token) => token.isCancelled), isTrue);
      expect(controller.dispose, returnsNormally);
      expect(() => controller.addListener(() {}), throwsFlutterError);
      await controller.load(_games);
      expect(repository.pending, hasLength(2));
      repository.pending[0].complete(_read());
      repository.pending[1].complete(_read());
      await Future.wait([games, films]);
      expect(notified, 0);
      expect(controller.state(_games).read, isNull);
    },
  );

  test(
    'disposal during initial notification prevents repository work',
    () async {
      final repository = _Catalog();
      final controller = CatalogBrowseController(repository);
      var laterCalled = false;
      controller.addListener(controller.dispose);
      controller.addListener(() {
        laterCalled = true;
        expect(controller.state(_games).loading, isFalse);
      });
      await controller.load(_games);
      expect(laterCalled, isTrue);
      expect(repository.pending, isEmpty);
      expect(controller.dispose, returnsNormally);
      expect(() => controller.addListener(() {}), throwsFlutterError);
    },
  );
}
