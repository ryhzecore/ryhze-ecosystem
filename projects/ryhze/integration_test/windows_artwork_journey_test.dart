import 'dart:typed_data';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:crypto/crypto.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/design/artwork_presentation.dart';
import 'package:ryhze/design/artwork_reference.dart';
import 'package:ryhze/design/artwork_source.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/store_sections.dart';
import 'package:ryhze/features/entertainment/widgets/title_detail.dart';
import 'package:ryhze/platform/windows/windows_artwork_cache.dart';
import 'package:ryhze/providers/public_artwork_cache.dart';
import 'package:ryhze/providers/public_artwork_repository.dart';
import 'package:ryhze/providers/public_artwork_transport.dart';
import 'package:ryhze/providers/ryhze_catalog_repository.dart';

// Live public catalogue/PNG plus the actual Windows inert artwork cache.
// Offline failure and +2h age are explicitly injected, never live outage proof.
// No private store, account, credentials, launch or production registry changes.
void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('Windows public artwork persistence and presentation journey', (
    tester,
  ) async {
    expect(Platform.isWindows, isTrue);
    const source = String.fromEnvironment('RYHZE_SOURCE');
    final report = <String, Object?>{
      'source': source,
      'workload': 'windows-public-artwork-native-cache-v1',
      'status': 'running',
      'cleanup_completed': false,
      'completed_phases': <String>[],
      'read_timings_us': <String, int>{},
      'restart_scope': 'Repository and channel client; same Windows process.',
      'performance_scope':
          'Single-run operation timings; no optimization comparison.',
      'limits': [
        'Public artwork is presentation metadata, not gameplay or ownership evidence.',
        'Offline failure and +2h age are injected; the physical network remains unchanged.',
        'Repository/channel re-creation does not prove process or operating-system restart.',
        'Cancellation exercises a public read request; socket abort is not instrumented.',
        'No physical power, screen reader, installer or image-golden acceptance.',
      ],
    };
    binding.reportData ??= <String, dynamic>{};
    binding.reportData!['artwork_journey'] = report;
    final repositories = <PublicArtworkRepository>[];
    final tokens = <CancellationToken>[];
    WindowsArtworkCache? native;
    var bodyCompleted = false;

    Future<T> bounded<T>(
      Future<T> work,
      String label, {
      Duration limit = const Duration(seconds: 25),
    }) async {
      report['pending_operation'] = label;
      try {
        final result = await work.timeout(limit);
        report.remove('pending_operation');
        return result;
      } catch (_) {
        report.putIfAbsent('failed_operation', () => label);
        rethrow;
      }
    }

    CancellationToken token() {
      final result = CancellationToken();
      tokens.add(result);
      return result;
    }

    PublicArtworkRepository remember(PublicArtworkRepository repository) {
      repositories.add(repository);
      return repository;
    }

    Future<void> pump([Duration? elapsed]) => bounded(
      tester.pump(elapsed),
      'render frame',
      limit: const Duration(seconds: 10),
    );

    Future<void> until(bool Function() ready, String label) async {
      final watch = Stopwatch()..start();
      while (!ready() && watch.elapsed < const Duration(seconds: 15)) {
        await pump(const Duration(milliseconds: 50));
      }
      expect(ready(), isTrue, reason: label);
      await pump();
    }

    Future<void> tap(Finder finder) async {
      await bounded(
        tester.ensureVisible(finder),
        'reveal control',
        limit: const Duration(seconds: 10),
      );
      await bounded(
        tester.tap(finder),
        'tap control',
        limit: const Duration(seconds: 10),
      );
      await pump(const Duration(milliseconds: 250));
    }

    Future<ArtworkRead> read(
      PublicArtworkRepository repository,
      ArtworkReference reference,
      String label,
    ) async {
      final watch = Stopwatch()..start();
      final result = await bounded(repository.read(reference, token()), label);
      (report['read_timings_us'] as Map<String, int>)[label] =
          watch.elapsedMicroseconds;
      return result;
    }

    void done(String phase) {
      (report['completed_phases'] as List<String>).add(phase);
      report.remove('pending_operation');
      debugPrint('Artwork journey completed phase: $phase');
    }

    try {
      expect(
        RegExp(r'^[a-f0-9]{40}$').hasMatch(source),
        isTrue,
        reason: 'Pass the exact clean source commit as RYHZE_SOURCE.',
      );
      final catalogue = await bounded(
        RyhzeCatalogRepository().search(
          EntertainmentCategory.games,
          '',
          token(),
        ),
        'live catalogue',
      );
      expect(catalogue.quality, ReadQuality.complete);
      expect(catalogue.failure, isNull);
      final titles = catalogue.values.where((title) => title.artwork != null);
      expect(
        titles,
        isNotEmpty,
        reason: 'Approved public catalogue must supply artwork.',
      );
      final title = titles.first;
      final reference = title.artwork!;
      expect(reference.note.toLowerCase(), contains('not gameplay'));
      report['catalogue'] = {
        'quality': catalogue.quality.name,
        'public_metadata_count': catalogue.values.length,
        'observed_at_utc': catalogue.observedAt.toUtc().toIso8601String(),
      };
      done('live public catalogue reference');

      native = await bounded(
        WindowsArtworkCache.connect(),
        'connect native cache',
      );
      expect(
        native,
        isNotNull,
        reason: 'Native cache must be implemented and available.',
      );
      final cache = native!;
      report['native_cache_available'] = true;
      final beforeInitial = await bounded(
        cache.health(),
        'initial cache health',
      );
      report['initial_cache_health'] = _health(beforeInitial);
      final initialClear = await bounded(
        cache.clear(),
        'initial explicit cache clear',
      );
      final initialEmpty = await bounded(
        cache.health(),
        'initial cleared health',
      );
      expect(initialClear.removed, beforeInitial.entries);
      expect(initialClear.preserved, beforeInitial.preserved);
      expect(initialEmpty.entries, 0);
      expect(initialEmpty.bytes, 0);
      expect(initialEmpty.preserved, beforeInitial.preserved);
      report['initial_clear'] = _clear(initialClear);
      report['initial_cleared_health'] = _health(initialEmpty);
      done('native managed cache clear');

      final network = remember(PublicArtworkRepository(cache: cache));
      final fetched = await read(network, reference, 'network');
      expect(fetched.origin, ArtworkOrigin.network);
      expect(fetched.stale, isFalse);
      final payloadHash = sha256.convert(fetched.bytes).toString();
      final codec = await bounded(
        ui.instantiateImageCodec(fetched.bytes),
        'decode public PNG',
      );
      late int imageWidth, imageHeight;
      try {
        final frame = await bounded(
          codec.getNextFrame(),
          'decode public PNG frame',
        );
        imageWidth = frame.image.width;
        imageHeight = frame.image.height;
        frame.image.dispose();
      } finally {
        codec.dispose();
      }
      final persisted = await bounded(
        cache.get(reference.key),
        'verify native persistence',
      );
      expect(persisted, isNotNull);
      expect(sha256.convert(persisted!.bytes).toString(), payloadHash);
      expect(
        persisted.storedAt.millisecondsSinceEpoch,
        fetched.storedAt.millisecondsSinceEpoch,
      );
      report['image'] = {
        'item_id': reference.itemId,
        'revision': reference.revision,
        'key': reference.key,
        'path': reference.path,
        'note': reference.note,
        'sha256': payloadHash,
        'encoded_bytes': fetched.bytes.length,
        'width': imageWidth,
        'height': imageHeight,
        'stored_at_utc': fetched.storedAt.toUtc().toIso8601String(),
      };
      report['network_read'] = {
        'origin': fetched.origin.name,
        'sha256': payloadHash,
        'stale': fetched.stale,
        'native_persisted': true,
      };
      done('live artwork network fetch and native persistence');

      await bounded(network.close(), 'close first repository');
      final reopenedCache = await bounded(
        WindowsArtworkCache.connect(),
        'reconnect native cache',
      );
      expect(reopenedCache, isNotNull);
      final restarted = remember(PublicArtworkRepository(cache: reopenedCache));
      final disk = await read(restarted, reference, 'disk');
      expect(disk.origin, ArtworkOrigin.disk);
      expect(disk.stale, isFalse);
      expect(sha256.convert(disk.bytes).toString(), payloadHash);
      report['disk_read'] = {
        'origin': disk.origin.name,
        'sha256': payloadHash,
        'stale': disk.stale,
      };
      done('repository restart reads native disk');

      final warm = await read(restarted, reference, 'memory');
      expect(warm.origin, ArtworkOrigin.memory);
      expect(warm.stale, isFalse);
      expect(sha256.convert(warm.bytes).toString(), payloadHash);
      report['memory_read'] = {
        'origin': warm.origin.name,
        'sha256': payloadHash,
        'stale': warm.stale,
      };
      done('warm memory read');

      await bounded(
        tester.pumpWidget(_ArtworkJourneyView(source: restarted, title: title)),
        'mount Store',
        limit: const Duration(seconds: 10),
      );
      await until(
        () => find.text(reference.note).evaluate().length >= 2,
        'Store images and attribution',
      );
      expect(find.text(title.title), findsWidgets);
      expect(
        tester.widget<StoreHero>(find.byType(StoreHero)).title.id,
        title.id,
      );
      expect(tester.widget<GameTile>(find.byType(GameTile)).title.id, title.id);
      await tap(find.byKey(const Key('hero-explore')));
      final detail = find.byType(TitleDetail);
      await until(
        () => find
            .descendant(of: detail, matching: find.text(reference.note))
            .evaluate()
            .isNotEmpty,
        'detail image and attribution',
      );
      expect(
        find.descendant(
          of: detail,
          matching: find.byKey(const Key('public-artwork-image')),
        ),
        findsOneWidget,
      );
      final artSize = tester.getSize(find.byKey(const Key('detail-art-16-9')));
      expect(artSize.width / artSize.height, closeTo(16 / 9, .001));
      await tap(find.byKey(const Key('detail-back')));
      await until(
        () => find.byType(TitleDetail).evaluate().isEmpty,
        'detail disposal',
      );
      report['presentation'] = {
        'store_note': reference.note,
        'detail_note': reference.note,
        'detail_back_completed': true,
        'aspect_ratio': '16:9',
      };
      done('Store and detail image presentation');

      await bounded(
        tester.pumpWidget(const SizedBox()),
        'dispose fresh presentation',
      );
      await bounded(restarted.close(), 'close restarted repository');
      final offlineTransport = _InjectedOfflineTransport();
      final offlineClock = fetched.storedAt.add(const Duration(hours: 2));
      final offline = remember(
        PublicArtworkRepository(
          cache: reopenedCache,
          transport: offlineTransport,
          clock: () => offlineClock,
        ),
      );
      final stale = await read(offline, reference, 'offline');
      expect(stale.stale, isTrue);
      expect(stale.origin, ArtworkOrigin.disk);
      expect(sha256.convert(stale.bytes).toString(), payloadHash);
      await bounded(
        tester.pumpWidget(_ArtworkJourneyView(source: offline, title: title)),
        'mount offline Store',
      );
      final offlineCaption =
          '${reference.note} · Offline artwork may be out of date';
      await until(
        () => find.text(offlineCaption).evaluate().length >= 2,
        'visible offline labels',
      );
      report['offline'] = {
        'injected_failure': 'network',
        'injected_clock_offset_ms': const Duration(hours: 2).inMilliseconds,
        'origin': stale.origin.name,
        'stale': stale.stale,
        'sha256': payloadHash,
        'visible_caption': offlineCaption,
        'failed_transport_attempts': offlineTransport.attempts,
      };
      done('injected offline retention and visible label');

      final cancelledRepository = remember(PublicArtworkRepository());
      final cancelledToken = token();
      final pendingRead = cancelledRepository.read(reference, cancelledToken);
      cancelledToken.cancel();
      final cancellation = await bounded(
        pendingRead.then<String>(
          (_) => 'unexpected success',
          onError: (Object error, StackTrace stack) =>
              error is AppFailure ? error.kind.name : 'unclassified failure',
        ),
        'cancel public artwork read',
      );
      expect(cancellation, 'cancelled');
      await bounded(cancelledRepository.close(), 'settle cancelled repository');
      await bounded(cache.drain(), 'drain native cache');
      report['cancellation'] = {
        'failure': cancellation,
        'repository_closed': true,
        'native_drained': true,
        'socket_abort': 'not instrumented',
      };
      done('explicit cancellation and native drain');

      final beforeClear = await bounded(
        cache.health(),
        'health before final clear',
      );
      final attemptsBeforeClear = offlineTransport.attempts;
      final cleared = await bounded(
        offline.clear(),
        'final explicit cache clear',
      );
      await until(
        () => find.byKey(const Key('public-artwork-image')).evaluate().isEmpty,
        'clear removes visible bytes',
      );
      await pump(const Duration(milliseconds: 300));
      final afterClear = await bounded(
        cache.health(),
        'health after final clear',
      );
      final memoryAfterClear = await bounded(
        offline.health(),
        'memory after clear',
      );
      expect(cleared.removed, beforeClear.entries);
      expect(cleared.preserved, beforeClear.preserved);
      expect(afterClear.entries, 0);
      expect(afterClear.bytes, 0);
      expect(afterClear.entries, lessThanOrEqualTo(beforeClear.entries));
      expect(afterClear.bytes, lessThanOrEqualTo(beforeClear.bytes));
      expect(afterClear.preserved, beforeClear.preserved);
      expect(memoryAfterClear.memoryEntries, 0);
      expect(memoryAfterClear.memoryBytes, 0);
      expect(memoryAfterClear.active, 0);
      expect(memoryAfterClear.queued, 0);
      expect(offlineTransport.attempts, attemptsBeforeClear);
      expect(find.textContaining('Artwork hidden'), findsNWidgets(2));
      report['cache_before_clear'] = _health(beforeClear);
      report['clear'] = _clear(cleared);
      report['cache_after_clear'] = _health(afterClear);
      report['clear_presentation'] = {
        'visible_images': 0,
        'memory_entries': memoryAfterClear.memoryEntries,
        'memory_bytes': memoryAfterClear.memoryBytes,
        'active': memoryAfterClear.active,
        'queued': memoryAfterClear.queued,
        'automatic_refetches': offlineTransport.attempts - attemptsBeforeClear,
      };
      done('explicit cache clear without automatic refill');
      expect(tester.takeException(), isNull);
      bodyCompleted = true;
    } finally {
      report['status'] = 'failed';
      for (final cancellation in tokens) {
        cancellation.cancel();
      }
      var widgetDisposed = false;
      try {
        await bounded(
          tester.pumpWidget(const SizedBox()),
          'dispose final presentation',
          limit: const Duration(seconds: 10),
        );
        widgetDisposed = true;
      } finally {
        // Bounded all-settled teardown attempts every owner; a failure cannot
        // convert a partially completed body into a passing receipt.
        var ownersClosed = true;
        for (final repository in repositories) {
          try {
            await bounded(
              repository.close(),
              'close artwork repository',
              limit: const Duration(seconds: 35),
            );
          } catch (_) {
            ownersClosed = false;
          }
        }
        try {
          await bounded(
            native?.drain() ?? Future<void>.value(),
            'final native drain',
            limit: const Duration(seconds: 35),
          );
        } catch (_) {
          ownersClosed = false;
        }
        report['cleanup_completed'] = widgetDisposed && ownersClosed;
        if (bodyCompleted && widgetDisposed && ownersClosed) {
          report['status'] = 'completed';
          report.remove('pending_operation');
        }
      }
    }
  }, timeout: const Timeout(Duration(minutes: 2, seconds: 30)));
}

Map<String, int> _health(PublicArtworkCacheHealth value) => {
  'entries': value.entries,
  'bytes': value.bytes,
  'preserved': value.preserved,
};

Map<String, int> _clear(PublicArtworkClearResult value) => {
  'removed': value.removed,
  'preserved': value.preserved,
};

class _InjectedOfflineTransport implements PublicArtworkTransport {
  int attempts = 0;
  @override
  Future<Uint8List> fetch(
    ArtworkReference reference,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    attempts++;
    throw const AppFailure(FailureKind.network);
  }
}

class _ArtworkJourneyView extends StatefulWidget {
  const _ArtworkJourneyView({required this.source, required this.title});
  final ArtworkSource source;
  final EntertainmentTitle title;
  @override
  State<_ArtworkJourneyView> createState() => _ArtworkJourneyViewState();
}

class _ArtworkJourneyViewState extends State<_ArtworkJourneyView> {
  final favourite = ValueNotifier(false);
  @override
  void dispose() {
    favourite.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => MaterialApp(
    theme: RyhzeTokens.theme,
    builder: (context, child) =>
        ArtworkScope(source: widget.source, child: child!),
    home: Builder(
      builder: (context) => Scaffold(
        body: LayoutBuilder(
          builder: (context, constraints) {
            void open() => showDialog<void>(
              context: context,
              builder: (context) => TitleDetail(
                title: widget.title,
                favourite: favourite,
                onFavourite: () => favourite.value = !favourite.value,
              ),
            );
            return SingleChildScrollView(
              child: Column(
                children: [
                  StoreHero(
                    width: constraints.maxWidth,
                    gutter: 24,
                    category: EntertainmentCategory.games,
                    title: widget.title,
                    favourite: favourite,
                    onFavourite: () => favourite.value = !favourite.value,
                    onExplore: open,
                  ),
                  Padding(
                    padding: const EdgeInsets.all(24),
                    child: SizedBox(
                      width: 480,
                      child: GameTile(title: widget.title, onOpen: open),
                    ),
                  ),
                ],
              ),
            );
          },
        ),
      ),
    ),
  );
}
