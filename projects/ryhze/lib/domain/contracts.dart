import '../core/app_failure.dart';

class ProviderItemRef {
  ProviderItemRef(this.provider, this.item, {this.namespace = ''}) {
    if (namespace.length > 256 || RegExp(r'[\x00-\x1f]').hasMatch(namespace)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    for (final part in [provider, item]) {
      if (part.trim().isEmpty ||
          part.length > 256 ||
          RegExp(r'[\x00-\x1f]').hasMatch(part)) {
        throw const AppFailure(FailureKind.invalidData);
      }
    }
  }
  final String provider, item, namespace;
  @override
  bool operator ==(Object other) =>
      other is ProviderItemRef &&
      provider == other.provider &&
      item == other.item &&
      namespace == other.namespace;
  @override
  int get hashCode => Object.hash(provider, item, namespace);
}

enum AccessState { verified, revoked, expired, unknown, unavailable }

class Entitlement {
  Entitlement({
    required this.account,
    required this.item,
    required this.authority,
    required this.observedAt,
    required this.state,
    this.expiresAt,
    this.fixture = false,
  });
  final String account;
  final ProviderItemRef item;
  final String authority;
  final DateTime observedAt;
  final DateTime? expiresAt;
  final AccessState state;
  final bool fixture;
  bool qualifies({
    required String accountScope,
    required String expectedAuthority,
    required DateTime now,
  }) =>
      !fixture &&
      account == accountScope &&
      authority.isNotEmpty &&
      authority == expectedAuthority &&
      state == AccessState.verified &&
      !observedAt.isAfter(now) &&
      (expiresAt == null || expiresAt!.isAfter(now));
}

enum ReadQuality { complete, partial, stale, unavailable }

class RepositoryRead<T> {
  RepositoryRead(
    Iterable<T> values, {
    required this.quality,
    required this.observedAt,
    this.failure,
  }) : values = List.unmodifiable(values);
  final List<T> values;
  final ReadQuality quality;
  final DateTime observedAt;
  final AppFailure? failure;
}

class Offer {
  Offer({
    required this.item,
    required this.seller,
    required this.region,
    required this.minorUnits,
    required this.currency,
    required this.observedAt,
    required this.destination,
  }) {
    if (minorUnits < 0 ||
        !RegExp(r'^[A-Z]{3}$').hasMatch(currency) ||
        seller.isEmpty ||
        !RegExp(r'^[A-Z]{2}$').hasMatch(region)) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
  final ProviderItemRef item;
  final String seller, region, currency;
  final int minorUnits;
  final DateTime observedAt;
  final Uri destination;
  // Display-only terms. Purchase requires provider destination/time revalidation.
}
