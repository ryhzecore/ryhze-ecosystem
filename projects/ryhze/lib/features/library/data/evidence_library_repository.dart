import '../../../core/app_failure.dart';
import '../../../domain/contracts.dart';
import '../../../providers/provider_registry.dart';
import '../../session/session_controller.dart';
import '../model/library_entry.dart';
import 'library_repository.dart';

abstract interface class EntitlementSource {
  String get authority;
  String get provider;
  Future<RepositoryRead<Entitlement>> read(
    AccountScope scope,
    CancellationToken cancellation,
  );
}

/// Production replacement seam for the legacy Library contract. Only verified
/// adapter evidence may be projected; fixtures remain outside the owned view.
class EvidenceLibraryRepository
    implements LibraryRepository, CancellableLibraryRepository {
  EvidenceLibraryRepository(
    this.source,
    this.scope, {
    DateTime Function()? clock,
  }) : clock = clock ?? DateTime.now;
  final EntitlementSource source;
  final AccountScope scope;
  final DateTime Function() clock;
  @override
  Future<LibraryLoadResult> load() => loadCancellable(CancellationToken());
  @override
  Future<LibraryLoadResult> loadCancellable(
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    try {
      final raw = await source.read(scope, cancellation);
      cancellation.check();
      if (raw.values.any(
        (entry) =>
            entry.item.provider != source.provider ||
            entry.item.namespace.isNotEmpty,
      )) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final now = clock().toUtc();
      final projected = EntitlementProjection.verify(
        raw,
        account: scope.key,
        authority: source.authority,
        now: now,
      );
      if (projected.quality == ReadQuality.unavailable ||
          raw.quality == ReadQuality.stale) {
        return LibraryLoadResult.unavailable('Verification unavailable');
      }
      final entries = projected.values
          .map(
            (evidence) => LibraryEntry(
              identity: LibraryIdentity(
                providerId: evidence.item.provider,
                titleId: evidence.item.item,
              ),
              ownership: LibraryOwnership(
                state: LibraryOwnershipState.verifiedOwned,
                reason:
                    'Provider verified access at ${evidence.observedAt.toUtc().toIso8601String()}.',
              ),
              installation: const LibraryInstallation(
                state: LibraryInstallationState.unknown,
                reason: 'Local installation has not been inspected.',
              ),
            ),
          )
          .toList();
      // Keep uncertain records explicit so the existing projection displays its
      // tested partial-verification explanation without promoting them to owned.
      for (final evidence in raw.values) {
        if ((evidence.state == AccessState.unknown ||
                evidence.state == AccessState.unavailable) &&
            !entries.any(
              (entry) =>
                  entry.identity.providerId == evidence.item.provider &&
                  entry.identity.titleId == evidence.item.item,
            )) {
          entries.add(
            LibraryEntry(
              identity: LibraryIdentity(
                providerId: evidence.item.provider,
                titleId: evidence.item.item,
              ),
              ownership: const LibraryOwnership(
                state: LibraryOwnershipState.unknown,
                reason: 'Verification is unavailable.',
              ),
              installation: const LibraryInstallation(
                state: LibraryInstallationState.unknown,
                reason: 'Installation is unknown.',
              ),
            ),
          );
        }
      }
      return LibraryLoadResult.verified(
        entries,
        partial: projected.quality == ReadQuality.partial,
      );
    } on AppFailure catch (failure) {
      if (failure.kind == FailureKind.cancelled) rethrow;
      return LibraryLoadResult.failed(failure);
    } catch (_) {
      return LibraryLoadResult.failure('Adapter failure');
    }
  }
}
