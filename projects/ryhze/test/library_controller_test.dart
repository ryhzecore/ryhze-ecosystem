import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/library/data/demo_library_repository.dart';
import 'package:ryhze/features/library/data/library_repository.dart';
import 'package:ryhze/features/library/library_controller.dart';
import 'package:ryhze/features/library/model/library_entry.dart';

class _Repository implements LibraryRepository {
  _Repository(this.callback);

  final Future<LibraryLoadResult> Function() callback;

  @override
  Future<LibraryLoadResult> load() => callback();
}

class _DeferredRepository implements LibraryRepository {
  final requests = <Completer<LibraryLoadResult>>[];

  @override
  Future<LibraryLoadResult> load() {
    final request = Completer<LibraryLoadResult>();
    requests.add(request);
    return request.future;
  }
}

LibraryEntry _entry(
  String id, {
  String provider = 'verified-test-provider',
  String? title,
  List<String> tags = const [],
  LibraryOwnershipState ownership = LibraryOwnershipState.verifiedOwned,
  LibraryInstallationState installation = LibraryInstallationState.unknown,
}) => LibraryEntry(
  identity: LibraryIdentity(providerId: provider, titleId: id),
  title: title,
  tags: tags,
  ownership: LibraryOwnership(state: ownership, reason: 'Test evidence.'),
  installation: LibraryInstallation(
    state: installation,
    reason: 'Test installation result.',
  ),
);

LibraryController _loaded(List<LibraryEntry> entries) => LibraryController(
  repository: _Repository(() async => LibraryLoadResult.verified(entries)),
);

void main() {
  test('default unavailable is not a verified empty Library', () async {
    final controller = LibraryController();
    addTearDown(controller.dispose);
    expect(controller.status, LibraryStatus.unavailable);
    final notifications = <LibraryStatus>[];
    controller.addListener(() => notifications.add(controller.status));
    await controller.load();
    expect(notifications, [LibraryStatus.loading, LibraryStatus.unavailable]);
    expect(controller.statusMessage, contains('No ownership provider'));
    expect(controller.entries, isEmpty);
    expect(controller.isDemo, isFalse);
  });

  test(
    'successful verification can establish an empty owned collection',
    () async {
      final controller = _loaded([]);
      addTearDown(controller.dispose);
      await controller.load();
      expect(controller.status, LibraryStatus.verifiedEmpty);
      expect(controller.entries, isEmpty);
    },
  );

  test(
    'installed files and Store metadata do not establish ownership',
    () async {
      final unowned = _entry(
        Catalog.larcenous.id,
        title: Catalog.larcenous.title,
        ownership: LibraryOwnershipState.notOwned,
        installation: LibraryInstallationState.installed,
      );
      final unknown = _entry(
        'unknown-ownership',
        ownership: LibraryOwnershipState.unknown,
        installation: LibraryInstallationState.installed,
      );
      final controller = _loaded([unowned, unknown]);
      addTearDown(controller.dispose);
      await controller.load();
      expect(controller.status, LibraryStatus.unavailable);
      expect(
        controller.statusMessage,
        contains('empty Library has not been established'),
      );
      controller.toggleFavourite(unowned.identity);
      controller.select(unknown.identity);
      controller.setFilter(LibraryFilter.installed);
      expect(controller.visibleEntries, isEmpty);
      expect(controller.isFavourite(unowned.identity), isFalse);
      expect(controller.selectedEntry, isNull);
    },
  );

  test('known not-owned results can establish an empty Library', () async {
    final controller = _loaded([
      _entry(
        'not-owned',
        ownership: LibraryOwnershipState.notOwned,
        installation: LibraryInstallationState.installed,
      ),
    ]);
    addTearDown(controller.dispose);
    await controller.load();
    expect(controller.status, LibraryStatus.verifiedEmpty);
    expect(controller.entries, isEmpty);
  });

  test(
    'partially unknown ownership keeps known owned games and explains the gap',
    () async {
      final owned = _entry('known-owned');
      final controller = _loaded([
        owned,
        _entry(
          'not-verified',
          ownership: LibraryOwnershipState.unknown,
          installation: LibraryInstallationState.installed,
        ),
      ]);
      addTearDown(controller.dispose);
      await controller.load();
      expect(controller.status, LibraryStatus.loaded);
      expect(controller.entries, [owned]);
      expect(
        controller.statusMessage,
        contains('Some ownership records could not be verified'),
      );
    },
  );

  test(
    'owned records retain all installation states and absent metadata',
    () async {
      final entries = [
        for (final status in LibraryInstallationState.values)
          _entry('absent-from-store-${status.name}', installation: status),
      ];
      final controller = _loaded(entries);
      addTearDown(controller.dispose);
      await controller.load();
      expect(controller.status, LibraryStatus.loaded);
      expect(
        controller.entries,
        hasLength(LibraryInstallationState.values.length),
      );
      for (final entry in controller.entries) {
        expect(entry.title, entry.identity.titleId);
        expect(entry.hasMetadata, isFalse);
        expect(entry.description, contains('No description'));
        expect(entry.launchSupport.state, LibraryActionState.unsupported);
        expect(entry.installSupport.state, LibraryActionState.unsupported);
      }
      controller.setFilter(LibraryFilter.installed);
      expect(controller.visibleEntries, hasLength(1));
      expect(
        controller.visibleEntries.single.installation.state,
        LibraryInstallationState.installed,
      );
      controller.setFilter(LibraryFilter.allOwned);
      expect(controller.visibleEntries, hasLength(entries.length));
    },
  );

  test(
    'all words, tags, alphabetical order and session favourites combine',
    () async {
      final beta = _entry('beta', title: 'Beta Harbor', tags: ['Puzzle']);
      final alpha = _entry(
        'alpha',
        title: 'alpha Garden',
        tags: ['Puzzle', 'Exploration'],
        installation: LibraryInstallationState.installed,
      );
      final gamma = _entry(
        'gamma',
        title: 'Gamma Trail',
        tags: ['Exploration'],
      );
      final controller = _loaded([gamma, beta, alpha]);
      addTearDown(controller.dispose);
      await controller.load();
      expect(controller.visibleEntries, [alpha, beta, gamma]);
      controller.setQuery('  ALPHA   puzzle  ');
      expect(controller.visibleEntries, [alpha]);
      controller.toggleFavourite(beta.identity);
      controller.setFilter(LibraryFilter.favourites);
      expect(controller.visibleEntries, isEmpty);
      controller.setQuery('puzzle');
      expect(controller.visibleEntries, [beta]);
      controller.toggleFavourite(beta.identity);
      expect(controller.visibleEntries, isEmpty);
      controller.setQuery('');
      controller.setFilter(LibraryFilter.installed);
      expect(controller.visibleEntries, [alpha]);
      expect(controller.entries, [alpha, beta, gamma]);
    },
  );

  test(
    'identity is exact and provider-scoped even when titles match',
    () async {
      final lower = _entry('same', provider: 'provider', title: 'Same');
      final upper = _entry('same', provider: 'Provider', title: 'Same');
      final other = _entry('other', provider: 'provider', title: 'Same');
      final controller = _loaded([lower, other, upper]);
      addTearDown(controller.dispose);
      await controller.load();
      expect(controller.entries, [upper, other, lower]);
      controller.toggleFavourite(lower.identity);
      expect(controller.isFavourite(upper.identity), isFalse);
      controller.select(upper.identity);
      expect(controller.selectedEntry, same(upper));
      expect(
        lower.identity,
        const LibraryIdentity(providerId: 'provider', titleId: 'same'),
      );
    },
  );

  test(
    'selection survives view changes and reload until Back or removal',
    () async {
      final alpha = _entry('alpha', title: 'Alpha');
      final beta = _entry('beta', title: 'Beta');
      var entries = [alpha, beta];
      final controller = LibraryController(
        repository: _Repository(
          () async => LibraryLoadResult.verified(entries),
        ),
      );
      addTearDown(controller.dispose);
      await controller.load();
      controller.select(beta.identity);
      controller.setQuery('alpha');
      controller.setFilter(LibraryFilter.favourites);
      await controller.load();
      expect(controller.selectedEntry, same(beta));
      expect(controller.query, 'alpha');
      expect(controller.filter, LibraryFilter.favourites);
      controller.back();
      expect(controller.selectedEntry, isNull);
      controller.select(beta.identity);
      entries = [alpha];
      await controller.load();
      expect(controller.selectedId, isNull);
    },
  );

  test('favourites are session-only and never create records', () async {
    final entry = _entry('one');
    final first = _loaded([entry]);
    final second = _loaded([entry]);
    addTearDown(first.dispose);
    addTearDown(second.dispose);
    await first.load();
    first.toggleFavourite(entry.identity);
    first.toggleFavourite(
      const LibraryIdentity(providerId: 'missing', titleId: 'catalogue-only'),
    );
    await first.load();
    await second.load();
    expect(first.isFavourite(entry.identity), isTrue);
    expect(second.isFavourite(entry.identity), isFalse);
    expect(first.entries, [entry]);
  });

  test(
    'demo stays marked, fictional and separate from owned preferences',
    () async {
      final owned = _entry('owned', title: 'Verified fixture');
      final controller = _loaded([owned]);
      addTearDown(controller.dispose);
      await controller.load();
      controller.setQuery('Verified');
      controller.setFilter(LibraryFilter.favourites);
      controller.toggleFavourite(owned.identity);
      controller.select(owned.identity);
      final demoLoad = controller.enterDemo();
      expect(controller.isDemo, isTrue);
      expect(controller.status, LibraryStatus.loading);
      await demoLoad;
      expect(controller.status, LibraryStatus.demo);
      expect(controller.query, '');
      expect(controller.filter, LibraryFilter.allOwned);
      expect(controller.selectedId, isNull);
      expect(controller.entries.every((entry) => entry.isDemo), isTrue);
      expect(
        controller.entries.any(
          (entry) => entry.identity.titleId == Catalog.larcenous.id,
        ),
        isFalse,
      );
      final demo = controller.entries.first;
      controller.toggleFavourite(demo.identity);
      controller.setQuery(demo.title);
      controller.select(demo.identity);
      await controller.leaveDemo();
      expect(controller.isDemo, isFalse);
      expect(controller.entries, [owned]);
      expect(controller.query, 'Verified');
      expect(controller.filter, LibraryFilter.favourites);
      expect(controller.selectedEntry, same(owned));
      expect(controller.isFavourite(demo.identity), isFalse);
      await controller.enterDemo();
      expect(controller.query, demo.title);
      expect(controller.selectedId, demo.identity);
      expect(controller.isFavourite(demo.identity), isTrue);
      expect(controller.isFavourite(owned.identity), isFalse);
    },
  );

  test(
    'injected demo and real results never share preferences for the same ID',
    () async {
      final demo = _entry(
        'same',
        ownership: LibraryOwnershipState.demonstration,
      );
      final real = _entry('same');
      var result = LibraryLoadResult.demo([demo]);
      final controller = LibraryController(
        repository: _Repository(() async => result),
      );
      addTearDown(controller.dispose);
      await controller.load();
      controller.toggleFavourite(demo.identity);
      controller.setQuery('demo query');
      controller.select(demo.identity);
      result = LibraryLoadResult.verified([real]);
      await controller.load();
      expect(controller.isDemo, isFalse);
      expect(controller.query, '');
      expect(controller.selectedId, isNull);
      expect(controller.isFavourite(real.identity), isFalse);
      expect(controller.entries, [real]);
      result = LibraryLoadResult.demo([demo]);
      await controller.load();
      expect(controller.query, 'demo query');
      expect(controller.selectedId, demo.identity);
      expect(controller.isFavourite(demo.identity), isTrue);
    },
  );

  test('injected Demo exits to unavailable and can be entered again', () async {
    var fixtureLoads = 0;
    final fixture = _entry(
      'fixture',
      ownership: LibraryOwnershipState.demonstration,
    );
    final controller = LibraryController(
      repository: _Repository(() async {
        fixtureLoads++;
        return LibraryLoadResult.demo([fixture]);
      }),
    );
    addTearDown(controller.dispose);
    await controller.load();
    controller.setQuery('fixture');
    controller.toggleFavourite(fixture.identity);
    controller.select(fixture.identity);
    await controller.leaveDemo();
    expect(fixtureLoads, 1);
    expect(controller.isDemo, isFalse);
    expect(controller.status, LibraryStatus.unavailable);
    expect(controller.statusMessage, contains('No ownership provider'));
    expect(controller.entries, isEmpty);
    expect(controller.query, '');
    expect(controller.selectedId, isNull);
    expect(controller.isFavourite(fixture.identity), isFalse);
    await controller.enterDemo();
    expect(controller.isDemo, isTrue);
    expect(controller.status, LibraryStatus.demo);
    expect(controller.entries.every((entry) => entry.isDemo), isTrue);
    expect(controller.query, 'fixture');
    await controller.leaveDemo();
    expect(controller.status, LibraryStatus.unavailable);
    expect(fixtureLoads, 1);
  });

  test('leaving an injected Demo invalidates its pending refresh', () async {
    final repository = _DeferredRepository();
    final controller = LibraryController(repository: repository);
    addTearDown(controller.dispose);
    final initial = controller.load();
    repository.requests[0].complete(
      LibraryLoadResult.demo([
        _entry('fixture', ownership: LibraryOwnershipState.demonstration),
      ]),
    );
    await initial;
    final refresh = controller.load();
    await controller.leaveDemo();
    repository.requests[1].complete(
      LibraryLoadResult.verified([_entry('late')]),
    );
    await refresh;
    expect(controller.status, LibraryStatus.unavailable);
    expect(controller.isDemo, isFalse);
    expect(controller.entries, isEmpty);
    expect(repository.requests, hasLength(2));
  });

  test(
    'each demo fixture explains simulated status and unavailable actions',
    () async {
      final result = await const DemoLibraryRepository().load();
      expect(result.kind, LibraryResultKind.demo);
      expect(result.message, contains('fictional'));
      expect(result.entries, hasLength(5));
      expect(
        result.entries.map((entry) => entry.installation.state).toSet(),
        LibraryInstallationState.values.toSet(),
      );
      for (final entry in result.entries) {
        expect(entry.isDemo, isTrue);
        expect(entry.ownership.reason, contains('not verified ownership'));
        expect(entry.installation.reason, contains('Simulated'));
        expect(entry.launchSupport.reason, contains('Demo only'));
        expect(entry.installSupport.reason, contains('Demo only'));
      }
    },
  );

  test(
    'typed failure is distinct from unavailable and thrown details are hidden',
    () async {
      final controller = LibraryController(
        repository: _Repository(
          () async =>
              LibraryLoadResult.failure('The verification service is offline.'),
        ),
      );
      addTearDown(controller.dispose);
      await controller.load();
      expect(controller.status, LibraryStatus.error);
      expect(
        controller.statusMessage,
        'The service is unavailable. Try again later.',
      );
      await controller.setRepository(
        _Repository(() async => throw StateError('private-provider-detail')),
      );
      expect(controller.status, LibraryStatus.error);
      expect(
        controller.statusMessage,
        isNot(contains('private-provider-detail')),
      );
      expect(controller.entries, isEmpty);
    },
  );

  test('latest reload wins when completions arrive out of order', () async {
    final repository = _DeferredRepository();
    final controller = LibraryController(repository: repository);
    addTearDown(controller.dispose);
    final old = controller.load();
    final latest = controller.load();
    final entry = _entry('latest');
    repository.requests[1].complete(LibraryLoadResult.verified([entry]));
    await latest;
    repository.requests[0].complete(
      LibraryLoadResult.verified([_entry('stale')]),
    );
    await old;
    expect(controller.entries, [entry]);
    expect(controller.status, LibraryStatus.loaded);
  });

  test('stale failure cannot replace newer loaded data', () async {
    final repository = _DeferredRepository();
    final controller = LibraryController(repository: repository);
    addTearDown(controller.dispose);
    final old = controller.load();
    final latest = controller.load();
    repository.requests[1].complete(
      LibraryLoadResult.verified([_entry('latest')]),
    );
    await latest;
    repository.requests[0].completeError(StateError('stale error'));
    await old;
    expect(controller.entries.single.identity.titleId, 'latest');
    expect(controller.status, LibraryStatus.loaded);
  });

  test(
    'source replacement ignores old requests and resets session preferences',
    () async {
      final original = _DeferredRepository();
      final replacement = _DeferredRepository();
      final controller = LibraryController(repository: original);
      addTearDown(controller.dispose);
      final old = controller.load();
      controller.setQuery('old query');
      controller.setFilter(LibraryFilter.installed);
      final current = controller.setRepository(replacement);
      expect(controller.query, '');
      expect(controller.filter, LibraryFilter.allOwned);
      original.requests.single.complete(
        LibraryLoadResult.verified([_entry('old')]),
      );
      await old;
      expect(controller.status, LibraryStatus.loading);
      expect(controller.entries, isEmpty);
      replacement.requests.single.complete(
        LibraryLoadResult.verified([_entry('new')]),
      );
      await current;
      expect(controller.entries.single.identity.titleId, 'new');
    },
  );

  test(
    'leaving demo cannot be overwritten by outstanding owned verification',
    () async {
      final repository = _DeferredRepository();
      final controller = LibraryController(repository: repository);
      addTearDown(controller.dispose);
      final old = controller.load();
      await controller.enterDemo();
      final current = controller.leaveDemo();
      repository.requests[0].complete(
        LibraryLoadResult.verified([_entry('old')]),
      );
      await old;
      expect(controller.status, LibraryStatus.loading);
      expect(controller.isDemo, isFalse);
      repository.requests[1].complete(
        LibraryLoadResult.unavailable('Provider unavailable.'),
      );
      await current;
      expect(controller.status, LibraryStatus.unavailable);
      expect(controller.entries, isEmpty);
    },
  );

  for (final fail in [false, true]) {
    test('completion after disposal is ignored (failure=$fail)', () async {
      final repository = _DeferredRepository();
      final controller = LibraryController(repository: repository);
      var notifications = 0;
      controller.addListener(() => notifications++);
      final pending = controller.load();
      controller.dispose();
      if (fail) {
        repository.requests.single.completeError(StateError('late failure'));
      } else {
        repository.requests.single.complete(
          LibraryLoadResult.verified([_entry('late')]),
        );
      }
      await pending;
      expect(notifications, 1);
      expect(controller.entries, isEmpty);
    });
  }

  test(
    'synchronous source change during loading does not call stale repository',
    () async {
      final repository = _DeferredRepository();
      final controller = LibraryController(repository: repository);
      // Disposing a ChangeNotifier from its own listener is prohibited by Flutter.
      // A source change is permitted and exercises the same pre-await stale guard.
      var changed = false;
      controller.addListener(() {
        if (!changed) {
          changed = true;
          unawaited(
            controller.setRepository(
              _Repository(
                () async => LibraryLoadResult.unavailable('New source.'),
              ),
            ),
          );
        }
      });
      addTearDown(controller.dispose);
      await controller.load();
      await Future<void>.value();
      expect(repository.requests, isEmpty);
      expect(controller.status, LibraryStatus.unavailable);
      expect(controller.statusMessage, 'This capability is not available.');
    },
  );

  final invalidResults = <String, LibraryLoadResult>{
    'duplicate identity': LibraryLoadResult.verified([
      _entry('same'),
      _entry('same'),
    ]),
    'empty provider': LibraryLoadResult.verified([_entry('id', provider: ' ')]),
    'empty title identity': LibraryLoadResult.verified([_entry('')]),
    'demo in verified result': LibraryLoadResult.verified([
      _entry('demo', ownership: LibraryOwnershipState.demonstration),
    ]),
    'real ownership in demo result': LibraryLoadResult.demo([_entry('real')]),
  };
  for (final invalid in invalidResults.entries) {
    test('invalid result fails closed: ${invalid.key}', () async {
      final controller = LibraryController(
        repository: _Repository(() async => invalid.value),
      );
      addTearDown(controller.dispose);
      await controller.load();
      expect(controller.status, LibraryStatus.error);
      expect(controller.entries, isEmpty);
    });
  }

  test(
    'repository and presentation collections cannot mutate after validation',
    () async {
      final tags = ['Adventure'];
      final entry = _entry('one', tags: tags);
      final input = [entry];
      final result = LibraryLoadResult.verified(input);
      input.clear();
      tags.add('Changed');
      final controller = LibraryController(
        repository: _Repository(() async => result),
      );
      addTearDown(controller.dispose);
      await controller.load();
      expect(controller.entries, [entry]);
      expect(entry.tags, ['Adventure']);
      expect(() => controller.entries.clear(), throwsUnsupportedError);
      expect(() => controller.visibleEntries.clear(), throwsUnsupportedError);
      expect(() => result.entries.clear(), throwsUnsupportedError);
    },
  );
}
