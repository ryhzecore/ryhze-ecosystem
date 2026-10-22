import '../../domain/contracts.dart';
import '../../core/app_failure.dart';

/// Device-local file observations, never an entitlement or a launch plan.
/// A manifest may be stale, incomplete, shared or left by another Steam user.
class InstallationObservation {
  const InstallationObservation({
    required this.item,
    required this.title,
    required this.libraryRoot,
    required this.relativeDirectory,
    required this.providerState,
  });
  final ProviderItemRef item;
  final String title;
  final String libraryRoot;
  final String relativeDirectory;
  final String providerState;
}

abstract interface class InstallationRepository {
  Future<RepositoryRead<InstallationObservation>> inspect(
    CancellationToken cancellation,
  );
}
