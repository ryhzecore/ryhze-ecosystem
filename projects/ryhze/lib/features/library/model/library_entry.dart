/// A provider namespace plus that provider's title identity. Both are exact,
/// case-sensitive values; display titles and Store membership are not identities.
class LibraryIdentity {
  const LibraryIdentity({required this.providerId, required this.titleId});

  final String providerId;
  final String titleId;

  @override
  bool operator ==(Object other) =>
      other is LibraryIdentity &&
      providerId == other.providerId &&
      titleId == other.titleId;

  @override
  int get hashCode => Object.hash(providerId, titleId);

  @override
  String toString() => '${providerId.length}:$providerId$titleId';
}

enum LibraryOwnershipState { verifiedOwned, unknown, notOwned, demonstration }

class LibraryOwnership {
  const LibraryOwnership({required this.state, required this.reason});

  final LibraryOwnershipState state;
  final String reason;
}

enum LibraryInstallationState {
  installed,
  notInstalled,
  unknown,
  unsupported,
  failed,
}

class LibraryInstallation {
  const LibraryInstallation({required this.state, required this.reason});

  final LibraryInstallationState state;
  final String reason;
}

/// No operational state is exposed until a tested platform action exists.
/// Describing installed files alone cannot enable Play or Install.
enum LibraryActionState { unavailable, unsupported, failed }

class LibraryActionSupport {
  const LibraryActionSupport({required this.state, required this.reason});

  final LibraryActionState state;
  final String reason;
}

class LibraryEntry {
  LibraryEntry({
    required this.identity,
    this._title,
    this._description,
    List<String> tags = const [],
    required this.ownership,
    required this.installation,
    this.launchSupport = const LibraryActionSupport(
      state: LibraryActionState.unsupported,
      reason: 'Launching games is not implemented.',
    ),
    this.installSupport = const LibraryActionSupport(
      state: LibraryActionState.unsupported,
      reason: 'Downloading and installing games is not implemented.',
    ),
  }) : tags = List.unmodifiable(tags);

  final LibraryIdentity identity;
  final String? _title;
  final String? _description;
  final List<String> tags;
  final LibraryOwnership ownership;
  final LibraryInstallation installation;
  final LibraryActionSupport launchSupport;
  final LibraryActionSupport installSupport;

  bool get isDemo => ownership.state == LibraryOwnershipState.demonstration;
  bool get hasMetadata => _title != null && _title.trim().isNotEmpty;
  String get title => hasMetadata ? _title!.trim() : identity.titleId;
  String get description =>
      _description != null && _description.trim().isNotEmpty
      ? _description.trim()
      : 'No description is available for this provider title.';
}
