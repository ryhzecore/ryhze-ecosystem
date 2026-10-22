import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/app_route.dart';
import 'package:ryhze/features/collections/collection_repository.dart';
import 'package:ryhze/features/entertainment/entertainment_screen.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/title_detail.dart';
import 'package:ryhze/features/store/catalog_repository.dart';
import 'package:ryhze/features/workspace/workspace_panel.dart';
import 'package:ryhze/platform/windows/windows_activation.dart';

class _TrackedStore extends MemoryTransactionStore {
  int closes = 0;

  @override
  Future<void> close() async {
    closes++;
    await super.close();
  }
}

class _DeferredCatalog extends LocalCatalogRepository {
  final result = Completer<EntertainmentTitle?>();
  int requests = 0;

  @override
  Future<EntertainmentTitle?> lookup(
    EntertainmentCategory category,
    String id,
    CancellationToken cancellation,
  ) {
    requests++;
    return result.future;
  }
}

// Real Dart composition and widgets over a synthetic native channel. OS IPC,
// file access and credential protection are tested by the native harness.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('ryhze.tests/native-composition');
  const codec = StandardMethodCodec();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final acknowledged = <int>[];
  var initial = <Object?>[];

  setUp(() {
    initial = [];
    acknowledged.clear();
    messenger.setMockMethodCallHandler(channel, (call) async {
      switch (call.method) {
        case 'connect':
          return {
            'schemaVersion': 1,
            'singleInstance': true,
            'activations': initial,
          };
        case 'ack':
          acknowledged.add((call.arguments as Map)['sequence'] as int);
          return null;
        default:
          throw MissingPluginException();
      }
    });
  });
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  Future<WindowsActivationClient> connect() async =>
      (await WindowsActivationClient.connect(channel: channel))!;

  Future<void> emit(int sequence, String? uri) async {
    await messenger.handlePlatformMessage(
      channel.name,
      codec.encodeMethodCall(
        MethodCall('activate', {'sequence': sequence, 'uri': uri}),
      ),
      null,
    );
  }

  Future<void> mount(WidgetTester tester, AppServices services) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1280, 800);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(services.dispose);
    await tester.pumpWidget(RyhzeApp(services: services));
    await tester.pumpAndSettle();
  }

  Future<void> close(WidgetTester tester, AppServices services) async {
    await tester.pumpWidget(const SizedBox.shrink());
    // Keep async close inside the test's fake-async zone, not addTearDown.
    await services.close();
  }

  testWidgets(
    'cold Activity route survives mount and owns modal before next queued route',
    (tester) async {
      initial = [
        {'sequence': 1, 'uri': 'ryhze://app/activity'},
        {'sequence': 2, 'uri': 'ryhze://app/films/store'},
      ];
      final services = await AppServices.bootstrap(
        openStore: () async => MemoryTransactionStore(),
        connectLifecycle: () async => null,
        connectSecurity: () async => null,
        connectUpdates: () async => null,
        connectTaskbar: () async => null,
        connectNotifications: () async => null,
        connectInstallations: () async => null,
        connectContentStaging: () async => null,
        connectContentTransfer: () async => null,
        connectArtwork: () async => null,
        connectActivation: connect,
        catalogRepository: LocalCatalogRepository(),
      );
      await tester.pump();
      expect(services.routes.value.activity, isTrue);
      await mount(tester, services);
      expect(find.byType(WorkspacePanel), findsOneWidget);
      expect(find.text('No jobs for this account.'), findsOneWidget);
      expect(services.navigation.destination, AppDestination.gamesStore);
      expect(acknowledged, [1]);
      Navigator.of(tester.element(find.byType(WorkspacePanel))).pop();
      await tester.pumpAndSettle();
      expect(services.navigation.destination, AppDestination.filmsStore);
      expect(acknowledged, [1, 2]);
      expect(services.jobs.read(services.session.scope), isEmpty);
      expect(tester.takeException(), isNull);
      await close(tester, services);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'warm Activity activation waits for existing modal and preserves browsing',
    (tester) async {
      final services = AppServices.memory();
      services.navigation.go(const AppRoute(AppDestination.gamesLibrary));
      services.attachWindowsActivation(await connect());
      await mount(tester, services);
      final overlay = services.overlays.run(
        () => showDialog<void>(
          context: tester.element(find.byType(EntertainmentScreen)),
          builder: (_) => const AlertDialog(content: Text('Existing modal')),
        ),
      );
      await tester.pumpAndSettle();
      await emit(1, 'ryhze://app/activity');
      await emit(2, 'ryhze://app/films/store');
      await tester.pump();
      expect(acknowledged, isEmpty);
      expect(find.byType(WorkspacePanel), findsNothing);
      Navigator.of(tester.element(find.byType(AlertDialog))).pop();
      await overlay;
      await tester.pumpAndSettle();
      expect(find.byType(WorkspacePanel), findsOneWidget);
      expect(find.text('No jobs for this account.'), findsOneWidget);
      expect(services.navigation.destination, AppDestination.gamesLibrary);
      expect(acknowledged, [1]);
      Navigator.of(tester.element(find.byType(WorkspacePanel))).pop();
      await tester.pumpAndSettle();
      expect(services.navigation.destination, AppDestination.filmsStore);
      expect(acknowledged, [1, 2]);
      expect(tester.takeException(), isNull);
      await close(tester, services);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'cold exact item resolved by bootstrap survives the first widget mount',
    (tester) async {
      initial = [
        {
          'sequence': 1,
          'uri': 'ryhze://app/games/store?item=${Catalog.larcenous.id}',
        },
        {'sequence': 2, 'uri': 'ryhze://app/films/store'},
      ];
      final services = await AppServices.bootstrap(
        openStore: () async => MemoryTransactionStore(),
        connectLifecycle: () async => null,
        connectSecurity: () async => null,
        connectUpdates: () async => null,
        connectTaskbar: () async => null,
        connectNotifications: () async => null,
        connectInstallations: () async => null,
        connectContentStaging: () async => null,
        connectContentTransfer: () async => null,
        connectArtwork: () async => null,
        connectActivation: connect,
        catalogRepository: LocalCatalogRepository(),
      );
      await tester.pump();
      expect(services.routes.value.title?.id, Catalog.larcenous.id);
      await mount(tester, services);
      expect(find.byType(TitleDetail), findsOneWidget);
      expect(
        tester.widget<TitleDetail>(find.byType(TitleDetail)).title.id,
        Catalog.larcenous.id,
      );
      expect(acknowledged, [1]);
      expect(services.navigation.destination, AppDestination.gamesStore);
      expect(services.jobs.read(services.session.scope), isEmpty);
      Navigator.of(tester.element(find.byType(TitleDetail))).pop();
      await tester.pumpAndSettle();
      expect(services.navigation.destination, AppDestination.filmsStore);
      expect(acknowledged, [1, 2]);
      expect(tester.takeException(), isNull);
      await close(tester, services);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'warm route waits for an owned modal before navigation and acknowledgement',
    (tester) async {
      final services = AppServices.memory();
      services.attachWindowsActivation(await connect());
      await mount(tester, services);
      final overlay = services.overlays.run(
        () => showDialog<void>(
          context: tester.element(find.byType(EntertainmentScreen)),
          builder: (_) =>
              const AlertDialog(content: Text('Held fixture modal')),
        ),
      );
      await tester.pumpAndSettle();
      await emit(1, 'ryhze://app/films/store');
      await tester.pump();
      expect(services.navigation.destination, AppDestination.gamesStore);
      expect(acknowledged, isEmpty);
      expect(find.byType(AlertDialog), findsOneWidget);
      Navigator.of(tester.element(find.byType(AlertDialog))).pop();
      await overlay;
      await tester.pumpAndSettle();
      expect(services.navigation.destination, AppDestination.filmsStore);
      expect(acknowledged, [1]);
      // Native retry after acknowledgement cannot navigate or acknowledge twice.
      await emit(1, 'ryhze://app/films/store');
      await tester.pump();
      expect(acknowledged, [1]);
      expect(tester.takeException(), isNull);
      await close(tester, services);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'item lookup finishing under another modal is presented after that modal closes',
    (tester) async {
      final catalog = _DeferredCatalog();
      final services = AppServices(
        storage: MemoryTransactionStore(),
        catalogRepository: catalog,
      );
      services.attachWindowsActivation(await connect());
      await mount(tester, services);
      await emit(1, 'ryhze://app/games/store?item=${Catalog.larcenous.id}');
      await tester.pump();
      expect(catalog.requests, 1);
      final overlay = services.overlays.run(
        () => showDialog<void>(
          context: tester.element(find.byType(EntertainmentScreen)),
          builder: (_) =>
              const AlertDialog(content: Text('Lookup overlapped modal')),
        ),
      );
      await tester.pumpAndSettle();
      catalog.result.complete(Catalog.larcenous);
      await tester.pump();
      expect(find.byType(TitleDetail), findsNothing);
      expect(find.byType(AlertDialog), findsOneWidget);
      Navigator.of(tester.element(find.byType(AlertDialog))).pop();
      await overlay;
      await tester.pumpAndSettle();
      expect(find.byType(TitleDetail), findsOneWidget);
      expect(
        tester.widget<TitleDetail>(find.byType(TitleDetail)).title.id,
        Catalog.larcenous.id,
      );
      expect(catalog.requests, 1);
      expect(acknowledged, [1]);
      expect(tester.takeException(), isNull);
      await close(tester, services);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  test('replacement during lookup resumes retained queue and keeps warm handler', () async {
    final catalog = _DeferredCatalog();
    final services = AppServices(
      storage: MemoryTransactionStore(),
      catalogRepository: catalog,
    );
    addTearDown(services.dispose);
    final first = {
      'sequence': 1,
      'uri': 'ryhze://app/games/store?item=${Catalog.larcenous.id}',
    };
    initial = [first];
    services.attachWindowsActivation(await connect());
    await Future<void>.delayed(Duration.zero);
    expect(catalog.requests, 1);
    // The native snapshot retains unacknowledged entries when reconnecting.
    initial = [
      first,
      {'sequence': 2, 'uri': 'ryhze://app/films/store'},
    ];
    services.attachWindowsActivation(await connect());
    expect(acknowledged, isEmpty);
    // Unknown-item resolution completes the old await without opening a modal.
    catalog.result.complete(null);
    await Future<void>.delayed(Duration.zero);
    expect(services.navigation.destination, AppDestination.filmsStore);
    expect(acknowledged, [1, 2]);
    await emit(3, 'ryhze://app/games/library');
    await Future<void>.delayed(Duration.zero);
    expect(services.navigation.destination, AppDestination.gamesLibrary);
    expect(acknowledged, [1, 2, 3]);
    await services.close();
  });

  test('optional native failures preserve the opened healthy store', () async {
    final store = _TrackedStore();
    var attempted = 0;
    final services = await AppServices.bootstrap(
      openStore: () async => store,
      connectContentStaging: () async {
        attempted++;
        throw MissingPluginException();
      },
      connectLifecycle: () async {
        attempted++;
        throw MissingPluginException();
      },
      connectSecurity: () async {
        attempted++;
        throw const FormatException('synthetic unavailable native contract');
      },
      connectActivation: () async {
        attempted++;
        throw PlatformException(code: 'unavailable');
      },
      catalogRepository: LocalCatalogRepository(),
    );
    expect(attempted, 4);
    expect(services.storage, same(store));
    expect(store.closes, 0);
    expect(services.storageFailure, isNull);
    expect(services.windowsPowerEventsAvailable, isFalse);
    expect(services.windowsActivationAvailable, isFalse);
    expect(services.windowsSecurity, isNull);
    expect(services.windowsCredentials, isNull);
    await services.savePreferences(const Preferences(reduceMotion: true));
    expect(services.preferences.read().reduceMotion, isTrue);
    expect(services.session.scope.isLocal, isTrue);
    await services.close();
    expect(store.closes, 1);
  });

  test(
    'local construction keeps deterministic catalog and no native bootstrap',
    () async {
      final services = AppServices.memory();
      expect(services.catalog, isA<LocalCatalogRepository>());
      expect(services.windowsActivationAvailable, isFalse);
      expect(services.windowsPowerEventsAvailable, isFalse);
      expect(services.windowsSecurity, isNull);
      expect(services.windowsCredentials, isNull);
      await services.close();
    },
  );
}
