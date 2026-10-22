import '../core/app_failure.dart';
import '../domain/contracts.dart';

enum ProviderCapability {
  identity,
  catalog,
  ownedLibrary,
  installDetection,
  installHandoff,
  launchHandoff,
  offers,
  playback,
}

enum CapabilityStatus {
  unsupported,
  approvalRequired,
  authenticationRequired,
  available,
}

class CapabilityAvailability {
  const CapabilityAvailability(this.status, this.explanation);
  final CapabilityStatus status;
  final String explanation;
  bool get available => status == CapabilityStatus.available;
}

class ProviderDescriptor {
  ProviderDescriptor(
    this.id,
    this.name,
    Map<ProviderCapability, CapabilityAvailability> capabilities,
  ) : capabilities = Map.unmodifiable(capabilities);
  final String id, name;
  final Map<ProviderCapability, CapabilityAvailability> capabilities;
  CapabilityAvailability capability(ProviderCapability value) =>
      capabilities[value] ??
      const CapabilityAvailability(
        CapabilityStatus.unsupported,
        'No operational adapter is available.',
      );
}

class ProviderRegistry {
  // This registry describes unavailable capabilities, never connected accounts.
  final List<ProviderDescriptor> providers = List.unmodifiable([
    ProviderDescriptor('steam', 'Steam', {
      ProviderCapability.ownedLibrary: const CapabilityAvailability(
        CapabilityStatus.approvalRequired,
        'Owned-library access requires a documented authorized adapter and visible game details.',
      ),
    }),
    ProviderDescriptor('epic', 'Epic Games', {
      ProviderCapability.ownedLibrary: const CapabilityAvailability(
        CapabilityStatus.unsupported,
        'Configured-product Ecom access does not establish access to an entire personal Library.',
      ),
    }),
    ProviderDescriptor('films', 'Film providers', {
      ProviderCapability.playback: const CapabilityAvailability(
        CapabilityStatus.approvalRequired,
        'Film licensing, commerce and playback providers have not been selected.',
      ),
    }),
  ]);
}

/// An adapter must validate authority, account, pagination and expiry before
/// returning a projection. This validator never infers rights from local files.
class EntitlementProjection {
  static RepositoryRead<Entitlement> verify(
    RepositoryRead<Entitlement> input, {
    required String account,
    required String authority,
    required DateTime now,
  }) {
    if (account.isEmpty || authority.isEmpty) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final identities = <ProviderItemRef>{};
    bool? fixture;
    for (final record in input.values) {
      if (!identities.add(record.item) ||
          record.account != account ||
          record.authority != authority ||
          record.observedAt.isAfter(now) ||
          (fixture != null && fixture != record.fixture)) {
        throw const AppFailure(FailureKind.invalidData);
      }
      fixture = record.fixture;
    }
    final verified = input.values.where(
      (record) => record.qualifies(
        accountScope: account,
        expectedAuthority: authority,
        now: now,
      ),
    );
    final uncertain = input.values.any(
      (record) =>
          record.state == AccessState.unknown ||
          record.state == AccessState.unavailable ||
          record.fixture,
    );
    return RepositoryRead(
      verified,
      quality: uncertain || input.quality != ReadQuality.complete
          ? verified.isEmpty
                ? ReadQuality.unavailable
                : ReadQuality.partial
          : ReadQuality.complete,
      observedAt: input.observedAt,
      failure: input.failure,
    );
  }
}

/// Labeled fixture parser for contract tests. Never registers an operational
/// provider and never produces evidence that qualifies as verified ownership.
class FixtureEntitlementAdapter {
  RepositoryRead<Entitlement> parse(Object? input, {required DateTime now}) {
    try {
      final response = input as Map;
      if (response['fixture'] != true || response['complete'] is! bool) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final records = response['records'] as List;
      if (records.length > 5000) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final identities = <ProviderItemRef>{};
      final entries = records.map((value) {
        final record = value as Map;
        final ref = ProviderItemRef(
          record['provider'] as String,
          record['item'] as String,
        );
        if (!identities.add(ref)) {
          throw const AppFailure(FailureKind.invalidData);
        }
        final observed = DateTime.parse(record['observedAt'] as String);
        final expiry = record['expiresAt'] == null
            ? null
            : DateTime.parse(record['expiresAt'] as String);
        if (observed.isAfter(now) ||
            (expiry != null && !expiry.isAfter(observed))) {
          throw const AppFailure(FailureKind.invalidData);
        }
        return Entitlement(
          account: record['account'] as String,
          item: ref,
          authority: 'fixture',
          observedAt: observed,
          expiresAt: expiry,
          state: AccessState.values.byName(record['state'] as String),
          fixture: true,
        );
      }).toList();
      return RepositoryRead(
        entries,
        quality: response['complete'] == true
            ? ReadQuality.complete
            : ReadQuality.partial,
        observedAt: now,
      );
    } catch (_) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
}
