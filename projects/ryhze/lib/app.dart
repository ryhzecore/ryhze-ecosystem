import 'dart:async';
import 'dart:ui' show AppExitResponse;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'app/app_services.dart';
import 'design/controls.dart';
import 'design/artwork_presentation.dart';
import 'domain/app_route.dart';
import 'design/tokens.dart';
import 'features/entertainment/entertainment_screen.dart';
import 'features/library/data/library_repository.dart';

class RyhzeApp extends StatefulWidget {
  const RyhzeApp({super.key, this.libraryRepository, this.services});
  final LibraryRepository? libraryRepository;
  final AppServices? services;
  @override
  State<RyhzeApp> createState() => _RyhzeAppState();
}

class _RyhzeAppState extends State<RyhzeApp> with WidgetsBindingObserver {
  late AppServices services =
      widget.services ??
      AppServices.memory(libraryRepository: widget.libraryRepository);
  bool _appleInterrupted = false;

  bool get _apple =>
      !kIsWeb &&
      (defaultTargetPlatform == TargetPlatform.iOS ||
          defaultTargetPlatform == TargetPlatform.macOS);
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
  }

  @override
  Future<bool> didPushRouteInformation(
    RouteInformation routeInformation,
  ) async {
    final route = AppRoute.parse(routeInformation.uri);
    if (route == null) return false;
    await services.routes.activate(route);
    return true;
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.paused ||
        state == AppLifecycleState.detached ||
        (_apple &&
            defaultTargetPlatform == TargetPlatform.iOS &&
            state == AppLifecycleState.inactive)) {
      if (_apple) {
        services.interruptLifecycle();
        _appleInterrupted = true;
      } else {
        // Native Windows power events own its suspension/recovery contract.
        services.coordinator.cancelActive();
      }
    } else if (_apple &&
        state == AppLifecycleState.resumed &&
        _appleInterrupted) {
      _appleInterrupted = false;
      unawaited(services.recoverAfterResume());
    }
  }

  @override
  Future<AppExitResponse> didRequestAppExit() async {
    await services.close();
    return AppExitResponse.exit;
  }

  @override
  void didUpdateWidget(covariant RyhzeApp oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.services != widget.services) {
      _appleInterrupted = false;
      services.close();
      services =
          widget.services ??
          AppServices.memory(libraryRepository: widget.libraryRepository);
    } else if (oldWidget.libraryRepository != widget.libraryRepository) {
      services.library.setRepository(
        widget.libraryRepository ?? const UnavailableLibraryRepository(),
      );
    }
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    services.close();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: services,
    builder: (context, _) => MaterialApp(
      key: ValueKey((services, services.session.generation)),
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context).copyWith(
          disableAnimations:
              MediaQuery.disableAnimationsOf(context) ||
              services.preferences.read().reduceMotion,
        ),
        child: ArtworkScope(
          source: services.artwork,
          child: services.lifecycleFailure == null
              ? child!
              : Column(
                  children: [
                    SafeArea(
                      bottom: false,
                      child: Material(
                        color: RyhzeTokens.surface,
                        child: Padding(
                          padding: const EdgeInsets.all(12),
                          child: Semantics(
                            liveRegion: true,
                            child: Wrap(
                              spacing: 12,
                              runSpacing: 8,
                              crossAxisAlignment: WrapCrossAlignment.center,
                              children: [
                                Text(services.lifecycleFailure!.message),
                                RyhzeControl(
                                  key: const Key('retry-lifecycle-recovery'),
                                  label: 'Retry recovery',
                                  onPressed: services.recoverAfterResume,
                                  child: const Text('Retry recovery'),
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                    ),
                    Expanded(child: child!),
                  ],
                ),
        ),
      ),
      title: 'Ryhze',
      debugShowCheckedModeBanner: false,
      theme: RyhzeTokens.theme,
      shortcuts: <ShortcutActivator, Intent>{
        ...WidgetsApp.defaultShortcuts,
        const SingleActivator(LogicalKeyboardKey.arrowLeft):
            const DirectionalFocusIntent(TraversalDirection.left),
        const SingleActivator(LogicalKeyboardKey.arrowRight):
            const DirectionalFocusIntent(TraversalDirection.right),
        const SingleActivator(LogicalKeyboardKey.arrowUp):
            const DirectionalFocusIntent(TraversalDirection.up),
        const SingleActivator(LogicalKeyboardKey.arrowDown):
            const DirectionalFocusIntent(TraversalDirection.down),
        const SingleActivator(LogicalKeyboardKey.gameButtonA):
            const ActivateIntent(),
        const SingleActivator(LogicalKeyboardKey.gameButtonB):
            const DismissIntent(),
      },
      home: EntertainmentScreen(services: services),
    ),
  );
}
