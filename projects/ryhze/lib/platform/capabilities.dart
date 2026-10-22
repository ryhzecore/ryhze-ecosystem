import 'package:flutter/foundation.dart';

/// UI depends on capability descriptions, never on OS process APIs.
/// Apple storage has separate operation-specific contracts. No platform label
/// enables external launchers or implies successful native acceptance.
class PlatformCapabilities {
  const PlatformCapabilities({
    required this.name,
    required this.desktop,
    required this.externalLaunchers,
  });
  final String name;
  final bool desktop;
  final bool externalLaunchers;
  static PlatformCapabilities get current {
    if (kIsWeb) {
      return const PlatformCapabilities(
        name: 'Browser preview',
        desktop: false,
        externalLaunchers: false,
      );
    }
    return switch (defaultTargetPlatform) {
      TargetPlatform.windows => const PlatformCapabilities(
        name: 'Windows',
        desktop: true,
        externalLaunchers: false,
      ),
      TargetPlatform.linux => const PlatformCapabilities(
        name: 'Linux / Steam Deck',
        desktop: true,
        externalLaunchers: false,
      ),
      TargetPlatform.android => const PlatformCapabilities(
        name: 'Android',
        desktop: false,
        externalLaunchers: false,
      ),
      TargetPlatform.iOS => const PlatformCapabilities(
        name: 'iOS',
        desktop: false,
        externalLaunchers: false,
      ),
      TargetPlatform.macOS => const PlatformCapabilities(
        name: 'macOS',
        desktop: true,
        externalLaunchers: false,
      ),
      TargetPlatform.fuchsia => const PlatformCapabilities(
        name: 'Unsupported platform',
        desktop: false,
        externalLaunchers: false,
      ),
    };
  }
}
